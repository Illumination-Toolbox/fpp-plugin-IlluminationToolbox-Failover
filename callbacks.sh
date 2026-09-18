#!/bin/bash

# Tells fppd this is a native (C++) plugin: it loads lib<repoName>.so from
# the plugin directory.  Having a callbacks script is also what lets the
# Plugin Manager load/unload the plugin live without an fppd restart.

for var in "$@"
do
	case $var in
		-l|--list)
			echo "c++";
			exit 0;
		;;
		--)
			break
		;;
		*)
			exit 1
		;;
	esac
done
