#!/bin/sh

# distcc -- A simple distributed compiler system
#
# Copyright 2009 Google Inc.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
# USA.

# Author: Mark Mentovai

# This is a temporary integration shim to allow xcodebuild-driven builds
# to use distcc pump mode.  Because pump mode is enabled in part by the
# DISTCC_HOSTS environment variable, and xcodebuild resets that variable,
# this script is used to catch what Xcode does and re-reset the variable
# when pump mode is desired.
#
# Move /Developer/usr/bin/distcc to /Developer/usr/bin/distcc.orig, place
# this script at /Developer/usr/bin/distcc, and place toolwhip distcc at
# /Developer/usr/bin/distcc.my.
#
# Move /usr/bin/distccd to /usr/bin/distccd.orig and place toolwhip distccd
# at /usr/bin/distccd.  The launch daemon that runs distccd does not know how
# to start toolwhip distccd, so, um, don't run toolwhip distccd from launchd
# for now.  Xcode runs distccd --host-info to determine localy capabilities,
# so you need to replace /usr/bin/distccd even on systems where you don't
# run distccd servers, because you need to get the distcc versions reported
# to Xcode to match across the board.
#
# To use, set DISTCC_HOSTS and run "pump xcodebuild".  DISTCC_HOSTS needs to
# be set when you start pump.  xcodebuild will reset DISTCC_HOSTS according to
# the user's preferences (Bonjour, whatever) and this script will then catch
# that and rewrite it.  Rewriting only occurs if it appears that things are
# happening in pump mode by the presence of the INCLUDE_SERVER_PORT variable.
# Rewriting also only occurs if there are no occurrences of ,cpp in
# DISTCC_HOSTS.

if [ -n "${INCLUDE_SERVER_PORT}" ] && \
    echo "${DISTCC_HOSTS}" | grep -Fvq ,cpp ; then
  for host in ${DISTCC_HOSTS} ; do
    if [ "${host}" = "localhost" ] ; then
      # Don't rewrite localhost.  When distcc finds localhost by itself
      # without anything appended, it just runs the compilation locally
      # without trying to connect to a local distccd.
      NEW_DISTCC_HOSTS="${NEW_DISTCC_HOSTS} ${host}"
    else
      NEW_DISTCC_HOSTS="${NEW_DISTCC_HOSTS} ${host},lzo,cpp"
    fi
  done
  DISTCC_HOSTS="${NEW_DISTCC_HOSTS}"
fi

exec "$(dirname "${0}")/distcc.my" "${@}"

exit 1
