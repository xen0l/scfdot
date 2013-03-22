#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at CDDL.LICENSE.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at CDDL.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

# Build scfdot, invoke it to generate a graph description, and run dot to
# render it to PostScript.  The resulting file will be <hostname>.ps .

# How to invoke dot on this system.
DOT = dot

# Options to pass to scfdot.  This limits the graph to 300" by 42", includes
# legend.ps as the legend (built below), and consolidates inetd services into
# a single node.  See the comment at the top of scfdot.c for other options.
SCFDOTOPTS = -s 300,42 -l legend.ps -x consolidate_inetd_svcs

# Margin, in inches, to include above and below the legend.
LEGEND_MARGIN = 3

# Options to pass to dot when rendering the graph.  Consider increasing
# mclimit, which dictates how long dot spends optimizing node placement.  It
# defaults to 1.0; 100 should produce good output, but it may take a long
# time.
DOTOPTS =
#DOTOPTS = -Gmclimit=100


HOSTNAME:sh = hostname

all: $(HOSTNAME).ps

%.ps: %.dot legend.ps
	@# Redirect to /tmp so if the command fails, $@ won't be updated
	$(DOT) -Tps $(DOTOPTS) $< > /tmp/$@
	@# Setpage tells the plotter how big the page is
	awk -f setpage.awk /tmp/$@ > $@

$(HOSTNAME).dot: scfdot
	./scfdot $(SCFDOTOPTS) > $@

scfdot: scfdot.c
	$(CC) -o scfdot scfdot.c -lscf

legend.ps: legend.dot enlarge.awk
	$(DOT) -Tps legend.dot > /tmp/legend.ps
	awk -f enlarge.awk top=$(LEGEND_MARGIN) bottom=$(LEGEND_MARGIN) \
	    /tmp/legend.ps > legend.ps

legend.dot: scfdot
	./scfdot -L > $@

lint: scfdot.c
	lint scfdot.c -lscf

clean:
	rm -f $(HOSTNAME).dot $(HOSTNAME).ps legend.dot legend.ps scfdot
