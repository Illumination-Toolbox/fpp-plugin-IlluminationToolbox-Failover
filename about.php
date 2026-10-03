<div class="mt-2">
    <fieldset class="border rounded p-2">
        <legend>Player Failover - How it works</legend>
        <div class="p-2">
            <p>Two FPP players carry identical playlists, sequences, media and schedule. The <b>primary</b> runs in FPP <b>Player</b> mode with MultiSync enabled. The <b>backup</b> runs in FPP <b>Remote</b> mode: it mirrors the primary frame for frame like any remote, and its own outputs stay live. Remotes stay in normal FPP Remote mode; they follow whichever player is sending sync packets.</p>

            <p><b>Hot failover.</b> The backup polls the primary once a second, listens for heartbeats, and, while a sequence is playing, hears the primary's own sync packets several times a second. When those stop (hot detect) or nothing has been heard for the failure threshold, the backup keeps the sequence it is already mirroring running, opens the MultiSync send socket and becomes the sync source itself. The remotes see an unbroken stream of sync packets and never restart the song. In the background the plugin switches fppd to Player mode in memory and rebuilds the schedule; when the mirrored item ends, the backup's own playlist starts the next item. No fppd restart, no mode change on disk.</p>

            <p><b>Fail-back.</b> When the primary is back and healthy for the recovery threshold, it asks the backup for a graceful handoff. The backup finishes its current item, drops back to Remote mode in memory and mirrors again; the primary starts the next item a few hundred milliseconds after the backup's item ends. Set Fail-back to Manual to stay on the backup until you press <i>Fail Back</i>.</p>

            <p><b>Cold standby.</b> A backup left in Player mode, and the primary whenever it must stand by, use a different mechanism: they play a private, silent playlist named <code>fpp-failover-hold</code> that is protected from schedule override, which keeps their scheduler from starting the show. A cold takeover restarts the show at the item the peer was last seen playing.</p>

            <p><b>Witness check.</b> If the backup cannot reach the primary but a remote (with this plugin installed as role Remote) still hears it, the fault is the link between the players and the backup does not take over.</p>

            <p><b>Remote fallback.</b> A remote can loop a local .fseq when no player has been heard for a while, so the display is not dark if both players are down. The moment a player sends sync packets again, that sequence replaces the fallback.</p>

            <p><b>Content pipeline.</b> The primary is the cutting edge: new sequences from xLights land there and run there. A backup (or remote) keeps the last <i>promoted</i> set, so a failover always plays a proven show. On a timer the backup checks what changed on the primary and lists it on the status page. <i>Promote</i> saves the current stable set as a rollback point, then pulls the primary's content over it. Promotion can be manual, or automatic once the primary's change set has sat unchanged for a soak time (say a day, so last night's uploads become stable only after they have run a show). <i>Roll back</i> restores any snapshot onto this node, the primary, or both; the primary's current content is snapshotted first so a rollback is itself reversible. Snapshots are hard-link copies, so each costs only the space of what changed.</p>

            <p>Transport is FPP's own rsync daemon, the same one the MultiSync page uses: turn on <i>Enable rsync</i> under Settings &rarr; Services on the primary (and on the backup if you want to roll the primary back from it). Files are written to a temporary name and renamed into place, so a sequence that is playing keeps its old copy until it closes. Host-specific settings (network, outputs, channel config) are never copied.</p>

            <p><b>Manual control.</b> The buttons on the status page, the FPP commands <i>Failover Take Over</i>, <i>Failover Yield</i>, <i>Failover Fail Back</i>, <i>Failover Promote Content</i>, <i>Failover Check Content</i> and <i>Failover Rollback Content</i>, and the HTTP endpoints under <code>/api/plugin-apis/failover?action=</code> all do the same things. A manual takeover or yield pins the primary in standby until you press <i>Fail Back</i> or <i>Clear Manual Standby</i>.</p>

            <p><b>Things to know.</b></p>
            <ul>
                <li>Content sync mirrors files from the primary. It never copies settings, channel outputs or network config, and it never deletes anything unless "Delete extras" is on.</li>
                <li>A holding node still runs scheduled FPP <i>commands</i> from its schedule; only scheduled playlists are blocked.</li>
                <li>A hot backup in standby shows FPP's usual "playlist was scheduled but system is running in Remote mode" warning. That is expected.</li>
                <li>Turn on <b>Send MultiSync via Multicast</b> (or Broadcast) on the primary. FPP's unicast default only reaches nodes its discovery cache currently believes are remotes, and that cache goes stale after a node changes mode - while it is stale, heartbeats stop arriving and the witness check cannot vouch for anything.</li>
                <li>The HTTP API takes its action as a query parameter (<code>?action=status</code>), which works on every FPP 10 release. On FPP 10.0 to 10.1.x a plugin loaded without restarting fppd - as the Plugin Manager does - cannot serve subpath routes, so <code>/failover/status</code> returns 404 there until fppd restarts; FPP 10.2 fixed this. The buttons on this page use the form that works everywhere.</li>
                <li>Enabling MultiSync on the primary needs an fppd restart: FPP reads that setting only at startup, and until the restart the primary sends nothing for the backup and remotes to follow.</li>
                <li>If the backup's outputs go to the same controllers as the primary's, both send while the backup mirrors. Give the backup's E1.31 universes a lower priority, or leave its outputs off, unless the backup drives its own lights.</li>
                <li>Cold takeovers resume at item granularity: the current song restarts on the remotes.</li>
                <li>Do not delete the <code>fpp-failover-hold</code> playlist; the plugin recreates it if it is missing and removes it on uninstall.</li>
                <li>Everything the plugin does is written to <code>plugin-fpp-failover.log</code> in the FPP log viewer.</li>
            </ul>
        </div>
    </fieldset>

    <fieldset class="border rounded p-2 mt-2">
        <legend>Player Failover - Info</legend>
        <div class="p-2">
            <div id='credits'>
                <b>Developed by:</b><br />
                <br />
                James Arrowood<br />
                <br />
                <a href='https://github.com/jamesarrowood/fpp-failover'>Git Repository</a><br>
                <a href='https://github.com/jamesarrowood/fpp-failover/issues'>Bug Reporter</a><br>
                <br />
            </div>
        </div>
    </fieldset>
</div>
