#!/bin/bash
set -e

# fpp-failover install script: build the native plugin against the running
# FPP's headers.  Safe to re-run (Reinstall All Plugins / plugin update).

: "${FPPDIR:=/opt/fpp}"
: "${SRCDIR:=${FPPDIR}/src}"

BASEDIR=$(dirname "$0")
cd "$BASEDIR/.."

make "SRCDIR=${SRCDIR}"

# No restartFlag: the plugin declares FPP_PLUGIN_SUPPORTS_UNLOAD and ships a
# callbacks script, so the Plugin Manager loads it into the running fppd as
# soon as this script finishes.
