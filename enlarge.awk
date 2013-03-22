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
# Enlarge the bounding box of a PostScript file by amounts specified on the
# command line.  They should be specified by assignments to the left, right,
# top, and bottom variables, in inches.  For example,
#
#	awk -f enlarge.awk file.ps top=2 bottom=2 > newfile.ps
#

BEGIN {
	left = 0;
	bottom = 0;
	right = 0;
	top = 0;
}

/^%%BoundingBox:/ {
	printf "%s %d %d %d %d\n", $1, $2 - left * 72, $3 - bottom * 72, \
	    $4 + right * 72, $5 + top * 72;
	next;
}

{ print }
