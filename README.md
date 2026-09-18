# fpp-failover

Hot player failover for the [Falcon Player (FPP)](https://github.com/FalconChristmas/fpp).

Two FPP players carry the same show. The **primary** runs it. The **backup** runs
in FPP Remote mode and mirrors the primary frame for frame. When the primary dies
mid-song, the backup keeps that song running and becomes the MultiSync source
itself. The remotes see an unbroken stream of sync packets: no restart, no gap,
no mode change on disk, no fppd restart. When the primary comes back, the show is
handed back at an item boundary.

Remotes need nothing special; they follow whichever player is sending sync.
Installing the plugin on remotes adds visibility, a witness check against false
failovers, and an optional local fallback sequence.

Requires FPP 10.0 or newer.

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

Until it is listed in FPP's Plugin Manager, install from the Plugin Manager's
"Install from URL" with the raw `pluginInfo.json` URL of this repository, or
clone it into `/home/fpp/media/plugins/fpp-failover` and run
`scripts/fpp_install.sh`. The install builds `libfpp-failover.so` against the
running FPP's headers.

## Configure

Open **Status/Control → Player Failover** on each node.

**Primary player**

- FPP mode: Player, MultiSync enabled, and turn on **Send MultiSync to ALL
  remotes via Multicast** (or Broadcast). FPP's default is unicast only, which
  it sends only to nodes its discovery cache currently believes are in Remote
  mode. That cache goes stale for a long time after a node changes mode, and
  while it is stale the backup and remotes receive no heartbeats, which
  disables the witness check.
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
(`/failover/status`). FPP 10.0 registers plugin subpath routes but does not
dispatch them, so `/failover/status` returns 404 on that release; the reference
plugin `fpp-brightness` has the same problem. The plugin still accepts the
subpath form, and an `{"action": "..."}` body, for when that is fixed.

A manual takeover or yield pins the primary in standby until *Fail Back* or
*Clear Manual Standby* is used, so a test failover does not immediately undo
itself.

## Verified against real FPP

The plugin has been run on FPP 10.0 (the official `falconchristmas/fpp:latest`
image) as three containers - primary, backup and remote - on one network,
playing a real 2.5-minute, 91k-channel, 40fps `.fseq`.

Verified there: building against the real headers; loading and unloading into a
live `fppd` with no restart; peer discovery; the hold playlist blocking the
standby's scheduler; cold failover resuming at the item the primary was on;
graceful fail-back at an item boundary; the witness check vetoing a takeover and
then releasing it when its evidence went stale; MultiSync heartbeats; and the
content pipeline's check, promote, snapshot and rollback against FPP's own rsync
daemon.

**Hot standby, measured.** With the backup booted in Remote mode and mirroring a
real sequence, the primary was killed 16 seconds into the song. An independent
remote node was sampled once a second throughout:

```
t+5s   playing HotChocolate.fseq  21s  master=172.20.0.2
t+7s   playing HotChocolate.fseq  23s  master=172.20.0.2   <- primary killed
t+8s   playing HotChocolate.fseq  24s  master=172.20.0.3   <- backup is now the source
t+9s   playing HotChocolate.fseq  25s  master=172.20.0.3
```

The sync source changed in about two seconds and the playback position never
went backwards: the song did not restart, and the remote followed straight
through the swap. When that sequence reached its end, the backup's own playlist
picked up the next item and the remote followed that too, with the on-disk mode
still reading `remote` the whole time.

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

- `config/plugin.fpp-failover` — settings
- `plugindata/fpp-failover/state.json` — epoch and manual-standby flag
- `playlists/fpp-failover-hold.json` — the hold playlist (recreated if deleted, removed on uninstall)
- `plugindata/fpp-failover/sync-status.json`, `sync-pending.json` — last promote/rollback result, last check result
- `plugindata/fpp-failover/snapshots/<id>/` — rollback points (hard-link copies)
- `logs/plugin-fpp-failover.log` — event log
- The media folders, playlists and schedule chosen for content sync (backup and remote only)
- `MultiSyncMulticast` in FPP's settings is switched on if no MultiSync send method is enabled when the backup first takes over

## License

GPL v2, see `LICENSE`.
