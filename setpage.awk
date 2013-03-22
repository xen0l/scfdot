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

#
# This script reads a PostScript file and adds commands which inform an HP
# DesignJet (5500ps, though it also works on an 800ps, too) of the size of the
# page.  Otherwise, it won't print more than about 50" long.
#

# Not sure if this is exactly the right size, or if it will always precede
# BeginSetup, but it works for dot-produced .ps files.
/^%%BoundingBox:/ { width = $4; height = $5 }

/^%%BeginSetup$/ {
	if (width == 0 || height == 0) {
		exit 1;
	}
	print;

	# I doubt this is necessary, but the PPD spec suggests it.
	print "%%BeginFeature: *CustomPageSize";

	# Here we print the parameters to the code below.  We add an inch of
	# 'far' margin (the 'near' margin should have already been set by dot
	# to be whatever margin was set to in the .dot file).
	printf "%d %d\n", width + 72, height + 72;

	# This is the magic code from HP's ppd file.  Actually, the ppd code
	# begins with pop pop pop, but that just discards the orientation and
	# offset parameters, so if we don't print them in the first place, it
	# won't matter.
	print "<</PageSize [ 5 -2 roll ] /ImagingBBox null>>setpagedevice";

	print "%%EndFeature";
	next;
}

{ print }
