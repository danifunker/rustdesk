#!/bin/sh
#
# /etc/dt/config/Xsession.d/0100.rdeskvint -- start R-DeskVint with the desktop.
#
# Written by rdeskvint-enable; removed by rdeskvint-disable. Do not edit this
# copy, it is regenerated. The original is @BASEDIR@/lib/rdeskvint.xsession.
#
# CDE *sources* the files in Xsession.d rather than running them, so anything
# that blocks here blocks the whole desktop coming up. Hence the `&`, and hence
# no waiting, no probing and no output on stdout.
#
# DISPLAY and XAUTHORITY are already set by the time this runs, and the agent
# inherits them along with the user's identity -- which is the point of starting
# it here rather than from an rc script: it sees the session it is sharing, and
# input it injects arrives as that user.

if [ -x @BASEDIR@/bin/rustdesk-agent ]; then
	@BASEDIR@/bin/rustdesk-agent --log info >> "$HOME/.rdeskvint.log" 2>&1 &
fi
