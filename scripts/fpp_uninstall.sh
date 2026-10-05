#!/bin/bash

# fpp-plugin-IlluminationToolbox-Failover uninstall script.  Safe to run more than once.

: "${FPPDIR:=/opt/fpp}"
. "${FPPDIR}/scripts/common" 2>/dev/null || true
: "${MEDIADIR:=/home/fpp/media}"

# The plugin creates a private hold playlist that blocks this node's schedule
# while it is standing by.  Without the plugin it must not be left behind.
rm -f "${MEDIADIR}/playlists/fpp-failover-hold.json"

# Plugin state (epoch, manual standby flag).
rm -rf "${MEDIADIR}/plugindata/fpp-plugin-IlluminationToolbox-Failover"

exit 0
