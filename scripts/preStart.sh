#!/bin/sh

# fppd loads libfpp-plugin-IlluminationToolbox-Failover.so at its own startup.  fpp_install.sh builds it,
# and FPP's core-upgrade path rebuilds every plugin with a Makefile, so the
# only case to handle here is the library being missing outright (for example
# an SD image cloned from a machine of a different architecture).

: "${SRCDIR:=/opt/fpp/src}"

BASEDIR=$(dirname "$0")
cd "$BASEDIR/.."

if [ ! -f libfpp-plugin-IlluminationToolbox-Failover.so ]; then
    echo "fpp-plugin-IlluminationToolbox-Failover: libfpp-plugin-IlluminationToolbox-Failover.so is missing, rebuilding"
    make "SRCDIR=${SRCDIR}"
fi
