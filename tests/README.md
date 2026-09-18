# Tests

## `sync_content_test.sh` — content pipeline, no FPP needed

Runs `scripts/sync_content.sh` against a throwaway local rsync daemon and
checks the full lifecycle: check, promote, no-op promote, snapshot contents,
hard-linking, rollback of the backup, rollback of the primary with its
pre-rollback snapshot, undo, pruning, and the daemon-down error path.

Needs a real rsync 3.x. On macOS the system `rsync` is openrsync and cannot
run as a daemon, so install GNU rsync first:

```bash
brew install rsync
bash tests/sync_content_test.sh
```

32 checks; exits non-zero if any fail.

## Container lab — the plugin inside a real fppd

The C++ side was verified against FPP 10.0 using the official image, three
containers on one Docker network (primary, backup, remote):

```bash
docker network create fpplab
docker run -d --name fpp-primary --hostname fpp-primary --network fpplab \
  --cap-add=SYS_NICE -p 8081:80 -v "$PWD/lab/primary/media:/home/fpp/media" \
  falconchristmas/fpp:latest
# ...repeat for fpp-backup (8082) and fpp-remote (8083)
```

Then, in each container, copy this repo to
`/home/fpp/media/plugins/fpp-failover`, run `scripts/fpp_install.sh` to build
it, and load it without restarting fppd:

```bash
curl -s -X POST http://127.0.0.1:32322/fppd/plugin/fpp-failover/load
```

Two things the container image needs that a real FPP box does not:

- **rsyncd**: there is no systemd, so FPP's "Enable rsync" toggle cannot start
  it. Generate the config the way `FPP_Install.sh` does and start it by hand:
  `sed -e 's#FPPDIR#/opt/fpp#g' -e 's#FPPHOME#/home/fpp#g' -e 's#FPPUSER#fpp#g' \
  < /opt/fpp/etc/rsyncd.conf > /etc/rsyncd.conf && rsync --daemon --config=/etc/rsyncd.conf`
- **`docker restart` does not work** on this image: a stale Apache pid file in
  the mounted media volume makes the entrypoint exit immediately. Remove the
  container and run a fresh one instead.

Set the primary to multicast (`MultiSyncMulticast=1`); FPP's unicast default
will not reach a node whose cached mode is stale.
