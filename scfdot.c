/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at CDDL.LICENSE.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at CDDL.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)scfdot.c	1.23	05/10/18 SMI"

/*
 * Generate a dot file for the SMF dependency graph on this machine.
 *
 * We operate in two modes: with and without -L.  Without -L, we print nodes
 * for each instance and edges for each dependency.  Fortunately dot allows
 * forward references, so we can do this in one pass.  Options are
 *
 *   -s width,height	Size, in inches, that the graph should be limited to.
 *
 *   -l legend.ps	PostScript file which should be used as the legend.
 *
 *   -x opts		Simplify the graph.  opts should be a comma-separated
 *			list of
 *
 *     omit_net_deps		Omit most of the dependencies on
 *				network/loopback and network/physical.  (See
 *				allowable_net_dep().)
 *
 *     consolidate_inetd_svcs	Consolidate services which only depend on
 *				network/inetd into a single node.
 *
 *     consolidate_rpcbind_svcs  Consolidate services which only depend on
 *				network/inetd and rpc/bind into a single node.
 *
 * Other hard-coded graph settings (rankdir, nodesep, margin) were intended
 * for a 42" plotter.
 *
 * -L causes the program to print a dot file for use as a legend.  It
 * currently consists of eight nodes which demonstrate the color scheme and
 * the dependency types.  The nodes are enclosed in a box which is labeled
 * "legend".
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/utsname.h>
#include <assert.h>
#include <libscf.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* Private libscf function */
extern int scf_parse_svc_fmri(char *fmri, const char **scope,
    const char **service, const char **instance, const char **propertygroup,
    const char **property);


/*
 * We color nodes by FMRI and enabledness.  For each class we specify
 * a foreground color, which will be the color of the text and the outline,
 * and a background color, which will fill the node.
 *
 * In this scheme we'll color services who's FMRIs begin with "system" orange,
 * "network" blue, "milestone" green, and other services light gray.  For each
 * category we'll color the disabled services a faded shade of their enabled
 * counterparts.  The foreground of the enabled services will all be black,
 * and the backgrounds will be
 */

#define	ORANGE		"#ED9B4F"
#define	BLUE		"#A3B8CB"
#define	GREEN		"#C5D5A9"
#define	GRAY		"#EDEFF2"

/*
 * To make disabled services appear faded, I took the above colors and halved
 * their saturation.
 */

#define	LTBLACK		"#808080"

#define	LTORANGE	"#EDC39C"
#define	LTBLUE		"#B7C1CB"
#define	LTGREEN		"#CDD5C0"
#define	LTGRAY		"#F0F1F2"

static const struct coloring {
	const char	*cat;
	const char	*colors[2][2];
} category_colors[] = {
	{ "system/", { { "black", ORANGE }, { LTBLACK, LTORANGE } } },
	{ "network/", { { "black", BLUE }, { LTBLACK, LTBLUE } } },
	{ "milestone/", { { "black", GREEN }, { LTBLACK, LTGREEN } } },
	{ NULL, { { "black", GRAY }, { LTBLACK, LTGRAY } } },
};

/* Graph simplification options, for use with getsubopt(). */
static const char * const x_opts[] = {
	"omit_net_deps",
	"consolidate_inetd_svcs",
	"consolidate_rpcbind_svcs",
	NULL
};

static int omit_net_deps = 0;
static int consolidate_inetd_svcs = 0;
static int consolidate_rpcbind_svcs = 0;

/* Consolidation strings */
static char *inetd_svcs, *rpcbind_svcs;
static size_t inetd_svcs_sz, rpcbind_svcs_sz;


static scf_handle_t *h;

/* Scratch libscf objects, to save time. */
static scf_service_t *g_svc;
static scf_instance_t *g_inst;
static scf_snapshot_t *g_snap;
static scf_propertygroup_t *g_pg;
static scf_property_t *g_prop;
static scf_value_t *g_val;
static scf_iter_t *g_institer, *g_pgiter, *g_valiter;

static ssize_t max_fmri_len, max_name_len, max_value_len;


static void
strappend(const char *str, char **bufp, size_t *bufszp)
{
	size_t str_sz;

	str_sz = strlen(str);

	if (strlen(*bufp) + str_sz + 1 > *bufszp) {
		*bufszp += str_sz;
		*bufp = realloc(*bufp, *bufszp);
		if (*bufp == NULL) {
			perror("realloc");
			exit(1);
		}
	}

	(void) strcat(*bufp, str);
}

static void
scfdie_lineno(int lineno)
{
	(void) fprintf(stderr, "%s:%d: Unexpected libscf error: %s.\n",
	    __FILE__, lineno, scf_strerror(scf_error()));
	exit(1);
}

#define	scfdie()	scfdie_lineno(__LINE__)

/*
 * Return 1 if inst is enabled, 0 otherwise.  Uses g_pg, g_prop, and g_val.
 */
static int
is_enabled(scf_instance_t *inst)
{
	uint8_t b;

	if (scf_instance_get_pg(inst, SCF_PG_GENERAL, g_pg) != 0) {
		if (scf_error() != SCF_ERROR_NOT_FOUND)
			scfdie();
		return (0);
	}

	if (scf_pg_get_property(g_pg, SCF_PROPERTY_ENABLED, g_prop) != 0) {
		if (scf_error() != SCF_ERROR_NOT_FOUND)
			scfdie();
		return (0);
	}

	if (scf_property_get_value(g_prop, g_val) != 0) {
		switch (scf_error()) {
		case SCF_ERROR_NOT_FOUND:
		case SCF_ERROR_CONSTRAINT_VIOLATED:
			return (0);

		default:
			scfdie();
		}
	}

	if (scf_value_get_boolean(g_val, &b) != 0) {
		if (scf_error() != SCF_ERROR_TYPE_MISMATCH)
			scfdie();
		return (0);
	}

	return (b != 0);
}

/*
 * Fill in buf with the restarter of instance i.  Uses g_pg, g_prop, and
 * g_val.
 */
static void
get_restarter(scf_instance_t *i, char *buf, size_t bufsz)
{
	if (scf_instance_get_pg_composed(i, NULL, SCF_PG_GENERAL, g_pg) != 0)
		scfdie();

	buf[0] = '\0';
	if (scf_pg_get_property(g_pg, SCF_PROPERTY_RESTARTER, g_prop) != 0) {
		if (scf_error() != SCF_ERROR_NOT_FOUND)
			scfdie();
		return;
	}

	if (scf_property_get_value(g_prop, g_val) != 0) {
		switch (scf_error()) {
		case SCF_ERROR_NOT_FOUND:
		case SCF_ERROR_CONSTRAINT_VIOLATED:
			return;

		default:
			scfdie();
		}
	}

	if (scf_value_get_astring(g_val, buf, bufsz) < 0) {
		if (scf_error() != SCF_ERROR_TYPE_MISMATCH)
			scfdie();
	}
}

static void
usage(const char *argv0, int help, FILE *stream)
{
	(void) fprintf(stream,
	    "Usage: %1$s [-s width,height] [-l legend.ps] [-x opts]\n"
	    "       %1$s -L\n", argv0);
	if (help) {
		const char * const *opt;

		(void) fprintf(stream,
		    "Where opts is a comma-separated list of\n");

		for (opt = &x_opts[0]; *opt != NULL; ++opt)
			(void) fprintf(stream, "\t%s\n", *opt);
	}
	exit(help ? 0 : 2);
}

/*
 * Make name suitable for dot.
 */
static void
clean_name(char *name)
{
	for (; *name != '\0'; ++name) {
		if (*name == '-')
			*name = '_';
	}
}

/*
 * Print an edge for a dependency.  port should be the name of the dependency
 * group.
 */
static void
print_dependency(const char *from, const char *port, const char *to,
    const char *opts, int weight)
{
	(void) printf("\"%s\":%s:e -> \"%s\"", from, port, to);

	if (weight != 1 || (opts != NULL && opts[0] != '\0')) {
		(void) fputs(" [", stdout);
		if (opts != NULL && opts[0] != '\0') {
			(void) fputs(opts, stdout);
			(void) putchar(',');
		}
		if (weight != 1)
			(void) printf("weight=%d,", weight);
		(void) putchar(']');
	}

	(void) puts(";");
}

/*
 * Choose a coloring for the given service.  Returns a pointer to an array of
 * two string pointers, the first being the text color and the second being
 * the fill color.
 */
static const char * const *
choose_color(const char *fmri, int enabled)
{
	const struct coloring *cp;

	if (strncmp(fmri, "svc:/", sizeof ("svc:/") - 1) == 0)
		fmri += sizeof ("svc:/") - 1;

	for (cp = &category_colors[0]; cp->cat != NULL; ++cp) {
		if (strncmp(fmri, cp->cat, strlen(cp->cat)) == 0)
			break;
	}

	return (cp->colors[enabled ? 0 : 1]);
}

/*
 * Print a node for a service.  dependencies should either be an empty string
 * or a string of "<dependency port name> dependency name" strings joined by
 * pipes ("|").
 */
static void
print_service_node(const char *fmri, const char *label,
    const char *dependencies, const char * const *colors)
{
	const char *fg = colors[0];
	const char *bg = colors[1];

	if (dependencies[0] != '\0')
		(void) printf("\"%s\" [shape=record,color=\"%s\",style=filled,"
		    "fillcolor=\"%s\",fontcolor=\"%s\","
		    "label=\"{<foo> %s | {%s}}\"];\n", fmri, fg, bg, fg, label,
		    dependencies);
	else
		(void) printf("\"%s\" [shape=record,color=\"%s\",style=filled,"
		    "fillcolor=\"%s\",fontcolor=\"%s\",label=\"%s\"];\n",
		    fmri, fg, bg, fg, label);
}

/*
 * Print some fake service nodes in a box to demonstrate the coloring and the
 * dependency types.
 */
static void
print_legend()
{
	const char *fmri;

	(void) printf("digraph legend {\n"
	    "node [fontname=\"Helvetica\",fontsize=11];\n"
	    "ranksep=\"2\";\n"
	    "rankdir=LR;\n");

	(void) printf("\nsubgraph clusterlegend {\n"
	    "label=\"legend\";\n"
	    "color=\"black\";\n");

	(void) putchar('\n');

	fmri = "svc:/system/disabled:default";
	print_service_node(fmri, fmri + 5, "<dg>dependency_group",
	    choose_color(fmri, 0));
	fmri = "svc:/system/enabled:default";
	print_service_node(fmri, fmri + 5, "", choose_color(fmri, 1));
	print_dependency("svc:/system/disabled:default", "dg",
	    "svc:/system/enabled:default", "label=\"require_all\",style=bold",
	    10);

	fmri = "svc:/network/disabled:default";
	print_service_node(fmri, fmri + 5, "<dg>dependency_group",
	    choose_color(fmri, 0));
	fmri = "svc:/network/enabled:default";
	print_service_node(fmri, fmri + 5, "", choose_color(fmri, 1));
	print_dependency("svc:/network/disabled:default", "dg",
	    "svc:/network/enabled:default", "label=\"require_any\"", 1);

	fmri = "svc:/milestone/disabled:default";
	print_service_node(fmri, fmri + 5, "<dg>dependency_group",
	    choose_color(fmri, 0));
	fmri = "svc:/milestone/enabled:default";
	print_service_node(fmri, fmri + 5, "", choose_color(fmri, 1));
	print_dependency("svc:/milestone/disabled:default", "dg",
	    "svc:/milestone/enabled:default",
	    "label=\"optional_all\",style=dashed", 1);

	fmri = "svc:/other/disabled:default";
	print_service_node(fmri, fmri + 5, "<dg>dependency_group",
	    choose_color(fmri, 0));
	fmri = "svc:/other/enabled:default";
	print_service_node(fmri, fmri + 5, "", choose_color(fmri, 1));
	print_dependency("svc:/other/disabled:default", "dg",
	    "svc:/other/enabled:default",
	    "label=\"exclude_all\",arrowtail=odot", 1);

	(void) printf("}\n}\n");
}

/*
 * Return true if we shouldn't omit fmri's dependency on network/loopback or
 * network/physical, even under -x omit_net_deps.  (Otherwise they'd have no
 * dependents, which would produce a bad graph.)
 */
static int
allowable_net_dep(const char *fmri)
{
	return (strcmp(fmri, "svc:/system/identity:node") == 0 ||
	    strcmp(fmri, "svc:/system/identity:domain") == 0 ||
	    strcmp(fmri, "svc:/network/initial:default") == 0 ||
	    strcmp(fmri, "svc:/milestone/single-user:default") == 0 ||
	    strcmp(fmri, "svc:/network/inetd:default") == 0 ||
	    strcmp(fmri, "svc:/network/http:apache2") == 0);
}

/* dependency name accumulator */
static char *allpgs;
static size_t allpgs_sz;

static void
add_dep(const char *str)
{
	/* Append sprintf("<%s> %s|", str, str) */
	strappend("<", &allpgs, &allpgs_sz);
	strappend(str, &allpgs, &allpgs_sz);
	strappend("> ", &allpgs, &allpgs_sz);
	strappend(str, &allpgs, &allpgs_sz);
	strappend("|", &allpgs, &allpgs_sz);
}

static char *fmri, *dep_fmri;			/* max_fmri_len + 1 long */
static char *instname, *pgname;			/* max_name_len + 1 long */
static char *depname, *depname_copy, *grouping;	/* max_value_len + 1 long */

/*
 * For the given instance, generate a node and the appropriate edges.
 */
static int
process_instance(scf_instance_t *i, const char *svcname)
{
	int enabled;
	int ndeps;
	int inetd_svc;
	int non_rpcbind;

	scf_snapshot_t *running;		/* NULL or == g_snap */

	assert(i);

	/*
	 * Node generation: Collect the name, restarter, dependency names, and
	 * enabled status and call print_service_node().  The dependency names
	 * will be accumulated in allpgs.
	 */

	if (scf_instance_get_name(i, instname, max_name_len + 1) == -1) {
		(void) fprintf(stderr, "instance_get_name() failed: %s",
		    scf_strerror(scf_error()));
		return (-1);
	}

	(void) snprintf(fmri, max_fmri_len + 1, "svc:/%s:%s", svcname,
	    instname);

	ndeps = 0;
	allpgs[0] = '\0';
	inetd_svc = 0;

	get_restarter(i, depname, max_value_len + 1);

	if (depname[0] != '\0') {
		++ndeps;
		add_dep("restarter");
		inetd_svc = (strstr(depname, "network/inetd:default") != NULL);
	}

	if (scf_instance_get_snapshot(i, "running", g_snap) == 0) {
		running = g_snap;
	} else {
		if (scf_error() != SCF_ERROR_NOT_FOUND)
			scfdie();
		running = NULL;
	}

	if (scf_iter_instance_pgs_typed_composed(g_pgiter, i, running,
	    SCF_GROUP_DEPENDENCY) != 0)
		scfdie();

	non_rpcbind = 0;

	while (scf_iter_next_pg(g_pgiter, g_pg) > 0) {
		if (scf_pg_get_property(g_pg, SCF_PROPERTY_ENTITIES, NULL) !=
		    0) {
			if (scf_error() == SCF_ERROR_NOT_FOUND)
				continue;
			scfdie();
		}

		++ndeps;

		if (scf_pg_get_name(g_pg, pgname, max_name_len + 1) < 0)
			scfdie();

		if (!non_rpcbind && strcmp(pgname, "rpcbind") != 0)
			non_rpcbind = 1;

		clean_name(pgname);
		add_dep(pgname);
	}

	if (consolidate_inetd_svcs && inetd_svc && ndeps == 1) {
		strappend(fmri + sizeof ("svc:/") - 1, &inetd_svcs,
		    &inetd_svcs_sz);
		strappend("\\n", &inetd_svcs, &inetd_svcs_sz);
		return (0);
	}

	if (consolidate_rpcbind_svcs && inetd_svc && non_rpcbind == 0 &&
	    ndeps == 2) {
		/*
		 * Exclude network/rpc/meta and rpc/smserver since they have
		 * dependents.
		 */
		if (strcmp(svcname, "network/rpc/meta") != 0 &&
		    strcmp(svcname, "network/rpc/smserver") != 0) {
			strappend(fmri + sizeof ("svc:/") - 1, &rpcbind_svcs,
			    &rpcbind_svcs_sz);
			strappend("\\n", &rpcbind_svcs, &rpcbind_svcs_sz);
			return (0);
		}
	}

	if (allpgs[0] != '\0')
		allpgs[strlen(allpgs) - 1] = '\0';	/* nuke trailing | */

	enabled = is_enabled(i);

	print_service_node(fmri, fmri + sizeof ("svc:/") - 1, allpgs,
	    choose_color(fmri, enabled));

	/*
	 * Edges: One for the restarter, if it is not the default (svc.startd)
	 * (denoted by an empty string), and one for each dependency service.
	 * Remember that dependency groups can name multiple services, and
	 * each service can have multiple instances.
	 */

	if (depname[0] != '\0')
		print_dependency(fmri, "restarter", depname, "", 1);

	if (scf_iter_instance_pgs_typed_composed(g_pgiter, i, running,
	    SCF_GROUP_DEPENDENCY) != 0)
		scfdie();

	for (;;) {
		int r;

		r = scf_iter_next_pg(g_pgiter, g_pg);
		if (r == 0)
			break;
		if (r < 0)
			scfdie();

		if (scf_pg_get_name(g_pg, pgname, max_name_len + 1) < 0)
			scfdie();
		clean_name(pgname);

		/* The grouping will dictate how we draw the edge */
		if (scf_pg_get_property(g_pg, SCF_PROPERTY_GROUPING, g_prop) !=
		    0)
			scfdie();

		if (scf_property_get_value(g_prop, g_val) != 0)
			scfdie();

		if (scf_value_get_astring(g_val, grouping, max_value_len + 1) <
		    0)
			scfdie();

		/* ENTITIES holds the FMRIs of the dependencies */
		if (scf_pg_get_property(g_pg, SCF_PROPERTY_ENTITIES, g_prop) !=
		    0)
			scfdie();

		if (scf_iter_property_values(g_valiter, g_prop) != 0)
			scfdie();

		for (;;) {
			const char *sname, *iname;
			int weight = 1;
			char opts[100];
			size_t l;

			r = scf_iter_next_value(g_valiter, g_val);
			if (r == 0)
				break;
			if (r < 0)
				scfdie();

			if (scf_value_get_astring(g_val, depname,
			    max_value_len + 1) < 0)
				scfdie();

			(void) strcpy(depname_copy, depname);

			/*
			 * This will fail if the dependency is on a file:,
			 * which is legitimate, but we'll skip.
			 *
			 * This function leaves sname & iname pointing into
			 * depname_copy, which may be modified.
			 */
			if (scf_parse_svc_fmri(depname_copy, NULL, &sname,
			    &iname, NULL, NULL) != 0)
				continue;

			if (scf_handle_decode_fmri(h, depname, NULL, g_svc,
			    g_inst, NULL, NULL, 0) != 0) {
				if (scf_error() != SCF_ERROR_NOT_FOUND)
					scfdie();
				continue;
			}

			if (omit_net_deps &&
			    (strcmp(sname, "network/loopback") == 0 ||
			    strcmp(sname, "network/physical") == 0) &&
			    !allowable_net_dep(fmri))
				continue;

			opts[0] = '\0';
			l = 0;
			if (strcmp(grouping, SCF_DEP_OPTIONAL_ALL) == 0) {
				l = strlcat(opts, "style=dashed,",
				    sizeof (opts));
			} else if (strcmp(grouping, SCF_DEP_EXCLUDE_ALL) == 0) {
				l = strlcat(opts, "arrowtail=odot,",
				    sizeof (opts));
			} else if (strcmp(grouping, SCF_DEP_REQUIRE_ALL) == 0) {
				weight += 2;
				l = strlcat(opts, "style=bold,", sizeof (opts));
			} else if (strcmp(grouping, SCF_DEP_REQUIRE_ANY) == 0) {
				weight += 1;
			}

			if (l >= sizeof (opts)) {
				(void) fputs("opts[] is too small.\n", stderr);
				exit(1);
			}

			if (opts[0] != '\0')
				opts[strlen(opts) - 1] = '\0';

			if (iname == NULL) {
				/*
				 * This is a service dependency.  Look up the
				 * service and output edges connecting that
				 * service node to each of its instances.
				 */

				if (scf_iter_service_instances(g_institer,
				    g_svc) != 0)
					scfdie();

				for (;;) {
					int i = 0;

					r = scf_iter_next_instance(g_institer,
					    g_inst);
					if (r == 0)
						break;
					if (r < 0)
						scfdie();

					if (scf_instance_to_fmri(g_inst,
					    dep_fmri, max_fmri_len + 1) == -1)
						scfdie();

					if (enabled && is_enabled(g_inst))
						i = 2;

					print_dependency(fmri, pgname,
					    dep_fmri, opts, weight + i);
				}
			} else {
				if (enabled && is_enabled(g_inst))
					weight += 2;

				print_dependency(fmri, pgname, depname, opts,
				    weight);
			}
		}
	}

	return (0);
}

/*
 * If requested, print the legend.  Otherwise print some graph settings and
 * call process_instance() for each service instance in the repository.
 */
int
main(int argc, char **argv)
{
	struct utsname utn;
	int r;
	time_t now;
	char timebuf[30];
	scf_scope_t *scope;
	scf_service_t *svc;
	scf_instance_t *inst;
	scf_iter_t *svciter, *institer;
	char *svcname;

	char *size = NULL;
	char *legendfile = NULL;

	for (;;) {
		int o = getopt(argc, argv, "s:l:x:L?");
		if (o == -1)
			break;

		switch (o) {
		case 's':
			size = optarg;
			break;

		case 'l':
			legendfile = optarg;
			break;

		case 'x':
			while (*optarg != '\0') {
				char *valp;
				int so;

				so = getsubopt(&optarg, (char * const *)x_opts,
				    &valp);
				if (so == -1 || valp != NULL)
					usage(argv[0], 0, stderr);

				switch (so) {
				case 0:
					omit_net_deps = 1;
					break;

				case 1:
					consolidate_inetd_svcs = 1;
					break;

				case 2:
					consolidate_rpcbind_svcs = 1;
					break;

				default:
					abort();
				}
			}
			break;

		case 'L':
			print_legend();
			return (0);

		case '?':
			usage(argv[0], optopt == '?', stdout);

		default:
			usage(argv[0], 0, stderr);
		}
	}

	r = uname(&utn);
	assert(r >= 0);

	now = time(NULL);
	(void) cftime(timebuf, NULL, &now);

	(void) printf("digraph scf {\n");
	(void) printf("label=\"%s %s %s\\n%s\";\n", utn.sysname, utn.version,
	    utn.machine, timebuf);
	(void) printf("node [shape=box,fontname=\"Helvetica\",fontsize=11];\n");
	if (size != NULL)
		(void) printf("size=\"%s\";\n", size);
	(void) printf("ranksep=\"2\";\n"
	    "rankdir=LR;\n"
	    "margin=1;\n");

	if (legendfile != NULL)
		/*
		 * The legend is just a node with the given PostScript as its
		 * shape.  dot will put it on the highest rank.  It usually
		 * appears too close to another node (system/zones, in
		 * particular); avoid that with a sufficiently large margin.
		 * (See expand.awk .)
		 */
		(void) printf("\n/* legend */\n"
		    "legend [shape=epsf,shapefile=\"%s\",label=\"\"];\n",
		    legendfile);

	(void) putchar('\n');

	h = scf_handle_create(SCF_VERSION);
	if (scf_handle_bind(h) != 0)
		scfdie();

	if ((scope = scf_scope_create(h)) == NULL ||
	    (svc = scf_service_create(h)) == NULL ||
	    (g_svc = scf_service_create(h)) == NULL ||
	    (inst = scf_instance_create(h)) == NULL ||
	    (g_inst = scf_instance_create(h)) == NULL ||
	    (svciter = scf_iter_create(h)) == NULL ||
	    (institer = scf_iter_create(h)) == NULL ||
	    (g_institer = scf_iter_create(h)) == NULL ||
	    (g_pgiter = scf_iter_create(h)) == NULL ||
	    (g_valiter = scf_iter_create(h)) == NULL ||
	    (g_pg = scf_pg_create(h)) == NULL ||
	    (g_prop = scf_property_create(h)) == NULL ||
	    (g_val = scf_value_create(h)) == NULL)
		scfdie();

	if ((max_name_len = scf_limit(SCF_LIMIT_MAX_NAME_LENGTH)) < 0 ||
	    (max_value_len = scf_limit(SCF_LIMIT_MAX_VALUE_LENGTH)) < 0 ||
	    (max_fmri_len = scf_limit(SCF_LIMIT_MAX_FMRI_LENGTH)) < 0)
		scfdie();

	allpgs_sz = 100;
	inetd_svcs_sz = 100;
	rpcbind_svcs_sz = 100;

	if ((svcname = malloc(max_name_len + 1)) == NULL ||
	    (instname = malloc(max_name_len + 1)) == NULL ||
	    (pgname = malloc(max_name_len + 1)) == NULL ||
	    (depname = malloc(max_value_len + 1)) == NULL ||
	    (depname_copy = malloc(max_value_len + 1)) == NULL ||
	    (grouping = malloc(max_value_len + 1)) == NULL ||
	    (fmri = malloc(max_fmri_len + 1)) == NULL ||
	    (dep_fmri = malloc(max_fmri_len + 1)) == NULL ||
	    (allpgs = malloc(allpgs_sz)) == NULL ||
	    (inetd_svcs = malloc(inetd_svcs_sz)) == NULL ||
	    (rpcbind_svcs = malloc(rpcbind_svcs_sz)) == NULL) {
		perror("malloc");
		exit(1);
	}

	inetd_svcs[0] = '\0';
	rpcbind_svcs[0] = '\0';

	if (scf_handle_get_scope(h, SCF_SCOPE_LOCAL, scope) != 0)
		scfdie();

	if (scf_iter_scope_services(svciter, scope) != 0)
		scfdie();

	for (;;) {
		r = scf_iter_next_service(svciter, svc);
		if (r == 0)
			break;
		if (r != 1)
			scfdie();

		if (scf_iter_service_instances(institer, svc) != 0)
			scfdie();

		if (scf_service_get_name(svc, svcname, max_name_len + 1) < 0)
			scfdie();

		if (strcmp(svcname, "system/svc/restarter") == 0)
			/* Otherwise this shows up as an unconnected node. */
			continue;

		for (;;) {
			r = scf_iter_next_instance(institer, inst);
			if (r == 0)
				break;
			if (r != 1)
				scfdie();

			if (process_instance(inst, svcname) != 0) {
				(void) fputs("process_instance() failed",
				    stderr);
				exit(1);
			}
		}
	}

	if (inetd_svcs[0] != '\0') {
		print_service_node("inetd_services", inetd_svcs,
		    "<restarter> restarter", choose_color("network/", 1));
		print_dependency("inetd_services", "restarter",
		    "svc:/network/inetd:default", "", 1);
	}

	if (rpcbind_svcs[0] != '\0') {
		print_service_node("rpcbind_services", rpcbind_svcs,
		    "<restarter> restarter | <rpcbind> rpcbind",
		    choose_color("network/", 1));
		print_dependency("rpcbind_services", "restarter",
		    "svc:/network/inetd:default", "", 1);
		print_dependency("rpcbind_services", "rpcbind",
		    "svc:/network/rpc/bind:default", "", 1);
	}

	(void) printf("}\n");
	return (0);
}
