# Tests

## `fpp_matrix.sh` - the plugin inside a real fppd, per FPP release

Builds and runs the plugin on an official FPP image, as three containers on a
private Docker network - a primary, a backup and a remote - and checks it.

```bash
bash tests/fpp_matrix.sh 10.2 "/path/to/a/real/show.fseq"
bash tests/fpp_matrix.sh 10.1.2 "/path/to/a/real/show.fseq"
bash tests/fpp_matrix.sh 10.0 "/path/to/a/real/show.fseq"
```

The tag is any `falconchristmas/fpp` image tag. The `.fseq` is optional; with
one, the hot-standby test runs: the backup boots in Remote mode and mirrors the
show, the primary is killed mid-song, and an independent remote is sampled once
a second to prove the song never restarts. Use a real rendered sequence at
least a minute long.

It prints PASS / FAIL / INFO per check and exits non-zero on any FAIL. Set
`KEEP=1` to leave the containers running afterwards for inspection. A run takes
a few minutes. Needs Docker.

Run it against each new FPP release before raising `maxFPPVersion` in
`pluginInfo.json`, and against the oldest supported release after changing the
plugin.

What the script works around, none of which applies to a real FPP box:

- `docker restart` does not work on this image: a stale Apache pid file in the
  media volume makes the entrypoint exit, so containers are recreated instead.
- There is no systemd, so FPP's "Enable rsync" toggle cannot start rsyncd; the
  script starts it by hand.
- `fppMode` and `MultiSyncEnabled` are read only when fppd starts, so the script
  writes them to each node's settings file and recreates the containers before
  testing. On a real primary that means: enable MultiSync, then restart fppd.

## `sync_content_test.sh` - content pipeline, no FPP needed

Runs `scripts/sync_content.sh` against a throwaway local rsync daemon and
checks the full lifecycle: check, promote, no-op promote, snapshot contents,
hard-linking, rollback of the backup, rollback of the primary with its
pre-rollback snapshot, undo, pruning, and the daemon-down error path.

Needs a real rsync 3.x. On macOS the system `rsync` is openrsync and cannot run
as a daemon, so install GNU rsync first:

```bash
brew install rsync
bash tests/sync_content_test.sh
```

32 checks; exits non-zero if any fail.
