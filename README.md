# Player Failover plugin for FPP

Hot player failover for the [Falcon Player (FPP)](https://github.com/FalconChristmas/fpp),
part of [Illumination Toolbox](https://illuminationtoolbox.com).

Two FPP players carry the same show. The **primary** runs it. The **backup** runs
in FPP Remote mode and mirrors the primary frame for frame. When the primary dies
mid-song, the backup keeps that song running and becomes the MultiSync source
itself. The remotes see an unbroken stream of sync packets: no restart, no gap,
no mode change on disk, no fppd restart. When the primary comes back, the show is
handed back at an item boundary.

Remotes need nothing special; they follow whichever player is sending sync.
Installing the plugin on remotes adds visibility, a witness check against false
failovers, and an optional local fallback sequence.

Supports FPP 10.0, 10.1.x and 10.2, and is tested against each release; see
[Tested FPP releases](#tested-fpp-releases).

Everything stays on your network: the players and remotes talk only to each
other. With the [Illumination Toolbox plugin](https://github.com/Illumination-Toolbox/fpp-plugin-IlluminationToolbox)
also installed and linked, Control Booth shows which player has the show and
can take over, yield or fail back from anywhere; see
[Illumination Toolbox](#illumination-toolbox).

## How it works

| Node | FPP mode | Standing by | Active |
|---|---|---|---|
| Primary | Player, MultiSync on | plays a private silent *hold* playlist that is protected from schedule override, so its scheduler cannot start the show | normal FPP player |
| Backup | **Remote** | mirrors the primary like any remote; its own outputs stay live | plugin sends the sync packets, fppd switched to Player mode in memory |

**Hot takeover.** The backup polls the primary over HTTP once a second, listens
for heartbeat packets, and while a sequence plays it hears the primary's own sync
packets several times a second. When those stop for the *hot detect* window (or
nothing is heard for the *failure threshold*), and no witness remote still hears
the primary, the backup:

1. opens the MultiSync send socket and starts sending sync packets for the
   sequence it is already playing, at the frame it is already on;
2. switches fppd to Player mode in memory and rebuilds the schedule, with the hold
   playlist blocking the scheduler until the mirrored item ends;
3. a few tens of milliseconds before the item ends, releases the hold; its own
   playlist starts the next item, which the plugin announces to the remotes.

fppd's native sync sending stays off (it was started as a remote), so the plugin
is the only sync source. It announces sequences and media the same way fppd
would, from the same per-frame hook fppd uses.

**Fail-back.** After *recovery threshold* successful polls, the primary asks the
backup for a handoff. The backup finishes its current item, drops back to Remote
mode in memory and mirrors again; the primary starts the next item a *handoff
margin* after the backup's item ends.

**Cold standby.** A backup left in Player mode, and the primary whenever it must
stand by, use the hold-playlist mechanism instead. A cold takeover restarts the
show at the item the peer was last seen playing.

| Situation | What happens |
|---|---|
| Primary dies mid-song | Backup becomes the sync source within the hot-detect window (default 1.5 s); remotes never notice. Next item comes from the backup's playlist. |
| Primary dies between items or outside show hours | Backup switches to Player mode in memory and its scheduler starts the scheduled playlist at the next item. |
| Primary returns | Graceful handoff at the next item boundary. |
| Both boot at once | Primary holds until it sees the backup standing by (about one poll), then starts. Backup boots as a remote and mirrors. |
| Both players die | Remotes go dark after their current item, unless they have a fallback sequence configured. |
| Backup reboots while active | It comes back as a remote. If the primary is still dead it takes over again (cold, from the next scheduled position). |

## Install

FPP → Content Setup → Plugin Manager → install from the plugin list, or paste
the address of the plugin's `pluginInfo.json` under *Install from URL*:

```
https://raw.githubusercontent.com/Illumination-Toolbox/fpp-plugin-IlluminationToolbox-Failover/main/pluginInfo.json
```

Or clone it into `/home/fpp/media/plugins/fpp-plugin-IlluminationToolbox-Failover`
and run `scripts/fpp_install.sh`. The install builds
`libfpp-plugin-IlluminationToolbox-Failover.so` against the running FPP's
headers. Install it on both players, and on any remote that should act as a
witness or run a fallback sequence.

## Configure

Open **Status/Control → Player Failover** on each node.

**Primary player**

- FPP mode: Player, with **MultiSync enabled - then restart fppd**. FPP reads
  that setting only when fppd starts; turned on at runtime it is saved but the
  player never opens its send socket, so the backup and remotes hear nothing.
  The plugin's status page warns while this is the case.
- Turn on **Send MultiSync to ALL remotes via Multicast** (or Broadcast). FPP's
  default is unicast only, which it sends only to nodes its discovery cache
  currently believes are in Remote mode. That cache goes stale for a long time
  after a node changes mode, and while it is stale the backup and remotes
  receive no heartbeats, which disables the witness check.
- Role: *Primary player*
- Peer address: the backup's IP
- Fail-back: *Automatic* (default) or *Manual*

**Backup player**

- FPP mode: **Remote** (restart fppd after changing it)
- Role: *Backup player*
- Peer address: the primary's IP
- Witness check: on, if your remotes have the plugin installed

**Remotes** (optional)

- Role: *Remote*
- Fallback sequence: an `.fseq` in the remote's sequences folder to loop when no
  player is alive, or empty to stay dark

Both players need the same playlists, sequences, media and schedule. Either
upload from xLights to both, or turn on **Content sync** on the backup (see
below). Plugin settings take effect immediately.

## Content pipeline: cutting edge on the primary, stable on the backup

New sequences from xLights go to the **primary** and run there first. The
backup keeps the last *promoted* set, so a failover always plays a show you have
already seen work. The backup (or a remote) tracks the primary with FPP's own
rsync daemon, the same transport the MultiSync page uses:

1. On the **primary**: Settings → Services → **Enable rsync**. Do the same on
   the backup if you want to roll the primary back from it.
2. On the backup (or remote): set the peer address, turn on *Content pipeline*,
   choose *Promotion* (manual or automatic), the check interval, and the
   folders to mirror.

| Action | What it does |
|---|---|
| **Check** (automatic, every *check interval*) | Dry run. The status page lists every file that is new, changed or would be deleted here if you promoted now. Touches nothing. |
| **Promote** (button, `Failover Promote Content`, or automatic) | Saves the current stable set as a rollback point, then pulls the primary's content over it. In automatic mode this happens once the primary's change set has stayed unchanged for the *soak time*; a soak of a day means last night's uploads become stable only after they have run a show. |
| **Roll back** (per snapshot: here, primary, or both) | Restores that snapshot. Rolling back the primary first snapshots what the primary has now ("pre-rollback"), so a rollback is itself reversible. |

What is mirrored: the chosen media folders (default sequences, music, videos,
effects, images, scripts), plus playlists and the schedule on a backup. The
schedule is reloaded in fppd after it changes. Never copied: settings, channel
outputs, network config, or anything else host-specific. Nothing is deleted
unless *Delete extras* is on. Snapshots are hard-link copies, so each one costs
only the space of the files that changed; the oldest are pruned beyond
*Rollback points to keep*. Files are written to a temporary name and renamed
into place, so a sequence that is playing keeps its old copy until it closes.
Automatic checks and promotes run only while the node is standing by and the
primary is reachable; the buttons and commands work any time.

If the backup's channel outputs go to the same controllers as the primary's,
both are sending while the backup mirrors. Give the backup's E1.31 universes a
lower priority, or leave its outputs off, unless the backup drives its own
lights.

## Manual control

The status page has buttons for *Take Over (graceful)*, *Take Over Now*,
*Yield*, *Fail Back to Primary* and *Clear Manual Standby*. The same actions are
available as FPP commands (`Failover Take Over`, `Failover Yield`,
`Failover Fail Back`, `Failover Promote Content`, `Failover Check Content`,
`Failover Rollback Content`) for schedules, presets and MQTT, and as HTTP
endpoints:

```
GET  /api/plugin-apis/failover?action=status
POST /api/plugin-apis/failover?action=takeover   {"graceful": true|false, "manual": true}
POST /api/plugin-apis/failover?action=yield      {"manual": true}
POST /api/plugin-apis/failover?action=failback
POST /api/plugin-apis/failover?action=resume
POST /api/plugin-apis/failover?action=promoteContent
POST /api/plugin-apis/failover?action=checkContent
POST /api/plugin-apis/failover?action=rollbackContent {"snapshot": "<id>|latest", "target": "local|primary|both"}
```

The action is given as a query parameter rather than a path segment
(`/failover/status`) because that is the form that works on every FPP 10
release. On FPP 10.0 to 10.1.x, a plugin loaded into a running fppd - which is
what the Plugin Manager does on install and update - gets its routes registered
with drogon after the server has started, and drogon never compiles late
subpath routes, so `/failover/status` returns 404 until fppd restarts. The
reference plugin `fpp-brightness` has the same problem. FPP 10.2 moved plugin
routes into its own registry and fixed it, so there the subpath form works too;
the plugin accepts it, and an `{"action": "..."}` body, on every release.

The routes are also listed on FPP's own API page (`/api/openapi.json`).

A manual takeover or yield pins the primary in standby until *Fail Back* or
*Clear Manual Standby* is used, so a test failover does not immediately undo
itself.

## Illumination Toolbox

This plugin sends nothing to the internet. The
[Illumination Toolbox plugin](https://github.com/Illumination-Toolbox/fpp-plugin-IlluminationToolbox),
when it is installed on the same player and linked to your toolbox account,
reads `GET /api/plugin-apis/failover?action=status` every fifteen seconds and
relays a trimmed copy - role, state, the other player's name, address and
health, what is playing, and the last ten events - so Control Booth can show
which player has the show. With its remote requests turned on, Control Booth
can also press this plugin's *Take Over*, *Yield* and *Fail Back* buttons for
you, through the same `POST /api/plugin-apis/failover?action=` calls the status
page makes. Turn that off in the toolbox plugin and nothing outside your
network can reach this one.

## Tested FPP releases

`tests/fpp_matrix.sh` runs the plugin inside a real fppd, using the official
`falconchristmas/fpp:<release>` image as three containers - primary, backup and
remote - and builds it against that release's own headers. With a real `.fseq`
it also runs the hot-standby test. Each release below passed all 17 checks,
playing a real 3-minute, 93k-channel, 40fps sequence:

| FPP | Subpath route `/failover/status` | Hot standby | Result |
|---|---|---|---|
| 10.0 | 404 (query form used) | position never went backwards | 17 / 17 |
| 10.1.2 | 404 (query form used) | position never went backwards | 17 / 17 |
| 10.2 | 200 | position never went backwards | 17 / 17 |

The checks: the plugin builds against that release's headers; loads into a
running fppd with no restart; answers on the query route on every release and on
the subpath route from 10.2; its API docs merge into FPP's spec at the right
path; the two players discover each other; a backup booted in Remote mode
reports hot standby; witness discovery finds the remote; all three nodes play
the real sequence; the primary is killed mid-song and an independent remote,
sampled once a second, shows the sync source move to the backup while its
playback position never goes backwards; the takeover happens once with no flap;
the backup's on-disk mode stays Remote; and twelve immediate unload/reload
cycles on the backup, while its output thread is calling the plugin every
frame, leave fppd running and the plugin answering.

A typical hot-standby trace, from FPP 10.2 - the remote's position in seconds,
and the last octet of whoever it is taking sync from:

```
remote position: 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33
sync source    : 10 10 10 10 10 10 11 11 11 11 11 11 11 11 11 11 11 11 11 11
                                   ^ primary (.10) killed, backup (.11) takes over
```

Earlier rounds also covered cold failover resuming at the item the primary was
on, graceful fail-back at an item boundary, the witness check vetoing a takeover
and releasing it when its evidence went stale, and the content pipeline's
check, promote, snapshot and rollback against FPP's own rsync daemon.

Containers on one Docker bridge have near-zero packet loss and drive no real
output hardware, so the takeover times here are a floor. Bench-test two real
players before relying on this for a show.

## Things to know

- A hot backup in standby shows FPP's usual "playlist was scheduled but system
  is running in Remote mode" warning. That is expected.
- A holding node still runs scheduled FPP *commands* from its schedule. Only
  scheduled playlists are blocked.
- While the backup acts as the player, the FPP UI's mode dropdown still says
  Remote (the on-disk setting is untouched). The plugin's status page shows the
  real state.
- If the plugin is uninstalled or unloaded while the backup is acting as the
  player, the running fppd stays in Player mode (the show is mid-flight) but
  the on-disk mode is left as you configured it and a restart is flagged. After
  that restart the node is a Remote-mode hot standby again. The mode is
  deliberately not persisted: a backup that came back from a reboot as a second
  full player would run the schedule against the primary and fight it for the
  remotes.
- Cold takeovers resume at item granularity. Mid-item seek is not attempted on
  a Player-mode node.

## Files the plugin touches

- `config/plugin.fpp-plugin-IlluminationToolbox-Failover` — settings
- `plugindata/fpp-plugin-IlluminationToolbox-Failover/state.json` — epoch and manual-standby flag
- `playlists/fpp-failover-hold.json` — the hold playlist (recreated if deleted, removed on uninstall)
- `plugindata/fpp-plugin-IlluminationToolbox-Failover/sync-status.json`, `sync-pending.json` — last promote/rollback result, last check result
- `plugindata/fpp-plugin-IlluminationToolbox-Failover/snapshots/<id>/` — rollback points (hard-link copies)
- `logs/plugin-fpp-plugin-IlluminationToolbox-Failover.log` — event log
- The media folders, playlists and schedule chosen for content sync (backup and remote only)
- `MultiSyncMulticast` in FPP's settings is switched on if no MultiSync send method is enabled when the backup first takes over

## License

GPL v2, see `LICENSE`.
