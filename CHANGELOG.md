# Changelog

The version is `PLUGIN_VERSION` in `src/FPPFailover.cpp`, reported by
`GET /api/plugin-apis/failover?action=status`. FPP's Plugin Manager installs
from `main` and shows git history as release notes.

## 1.3.0 — 2026-10-05

Joins Illumination Toolbox.

- Renamed to `fpp-plugin-IlluminationToolbox-Failover` and moved to the
  Illumination-Toolbox GitHub organisation. The install directory, shared
  library, settings file, data folder and log file follow the new name. The
  hold playlist is still `fpp-failover-hold` and the HTTP routes are still
  `/api/plugin-apis/failover`, so the Illumination Toolbox plugin's relay
  and Control Booth work unchanged.
- Players on 1.2.0 do not hear heartbeats from 1.3.0 and the reverse; update
  both players and any witness remotes together. To move over, uninstall the
  old plugin, install this one and enter the settings again.
- `pluginInfo.json` gains an icon, release notes from git history and a
  privacy declaration: it talks only to the players and remotes you set up,
  and nothing goes to the internet.

## 1.2.0 — 2026-10-03

Updated for FPP 10.2; verified on 10.0, 10.1.2 and 10.2.

## 1.1.0 — 2026-09-18

First version: hot player failover, the content pipeline and rollback. Later
the same day, hot standby verified with a real sequence, and a takeover flap
and mode persistence fixed.
