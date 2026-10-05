<?php
// fpp-plugin-IlluminationToolbox-Failover: status dashboard and configuration.
// $settings, $pluginSettings and the PrintSetting* helpers are provided by
// FPP's plugin.php wrapper.

$pluginName = 'fpp-plugin-IlluminationToolbox-Failover';

$roleOptions = array(
    'Off (plugin idle)' => 'off',
    'Primary player' => 'primary',
    'Backup player' => 'backup',
    'Remote' => 'remote'
);
$failbackOptions = array(
    'Automatic - primary takes the show back at the next item' => 'auto',
    'Manual - stay on the backup until Fail Back is pressed' => 'manual'
);
$syncModeOptions = array(
    'Manual - report changes, promote by hand' => 'manual',
    'Automatic - promote after the soak time' => 'auto'
);
$resumeOptions = array(
    'Same item the primary was playing (or the next one if it has ended)' => 'item',
    'Always the next item' => 'next',
    'Start of the scheduled playlist' => 'playlist'
);

$currentRole = isset($pluginSettings['Role']) ? $pluginSettings['Role'] : 'off';
?>

<script type="text/javascript">
var failoverStatusTimer = null;

function fmtAgo(ms) {
    if (ms === undefined || ms === null || ms < 0) return 'never';
    if (ms < 1000) return 'just now';
    var s = Math.floor(ms / 1000);
    if (s < 60) return s + 's ago';
    var m = Math.floor(s / 60);
    if (m < 60) return m + 'm ' + (s % 60) + 's ago';
    var h = Math.floor(m / 60);
    return h + 'h ' + (m % 60) + 'm ago';
}

function stateBadge(state) {
    var cls = 'text-bg-secondary';
    var text = state;
    switch (state) {
        case 'active': cls = 'text-bg-success'; text = 'ACTIVE - running the show'; break;
        case 'takeover': cls = 'text-bg-success'; text = 'TAKING OVER - now the sync source, finishing the current item'; break;
        case 'hold': cls = 'text-bg-info'; text = 'STANDBY'; break;
        case 'handoff_out': cls = 'text-bg-warning'; text = 'HANDING OFF - finishing current item'; break;
        case 'handoff_in': cls = 'text-bg-warning'; text = 'TAKING OVER - waiting for peer item to end'; break;
        case 'remote': cls = 'text-bg-info'; text = 'REMOTE - following the active player'; break;
        case 'remote_fallback': cls = 'text-bg-danger'; text = 'REMOTE - no player alive, running fallback'; break;
    }
    return '<span class="badge ' + cls + ' fs-6">' + text + '</span>';
}

function escapeHtml(s) {
    return $('<div>').text(s === undefined || s === null ? '' : String(s)).html();
}

function renderFailoverStatus(d) {
    $('#foRole').text(d.role);
    $('#foState').html(stateBadge(d.state));
    $('#foEpoch').text(d.epoch);
    var h2 = d.hot || {};
    var modeText = d.fppMode;
    if (h2.actingPlayer) modeText += ' (in memory; plugin is the MultiSync source)';
    else if (d.multisync) modeText += ' (MultiSync on)';
    else if (h2.standby) modeText += ' (hot standby: mirroring the primary)';
    else modeText += ' (MultiSync off)';
    $('#foMode').text(modeText);
    $('#foManual').toggleClass('d-none', !d.manualStandby);
    if (d.role == 'backup') {
        $('#foStandbyKind').text(h2.capable ? 'hot (Remote mode, mirrors the primary, no restart on takeover)' : 'cold (Player mode with hold playlist; set FPP mode to Remote for hot standby)');
        $('#foStandbyRow').removeClass('d-none');
    } else {
        $('#foStandbyRow').addClass('d-none');
    }

    var p = d.player || {};
    var playerText = p.status_name || 'idle';
    if (p.playlist) {
        playerText += ' - ' + p.playlist;
        if (p.count > 0) playerText += ' (item ' + p.index + ' of ' + p.count + ')';
        if (p.sequence) playerText += ' - ' + p.sequence;
        if (p.seconds_remaining > 0) playerText += ', ' + Math.round(p.seconds_remaining) + 's left';
        if (p.holding) playerText = 'holding (' + p.playlist + ')';
    }
    if (p.running_sequence && (!p.sequence || p.holding)) {
        playerText += ' - mirroring ' + p.running_sequence;
        if (p.sequence_remaining_ms > 0) playerText += ', ' + Math.round(p.sequence_remaining_ms / 1000) + 's left';
    }
    $('#foPlayer').text(playerText);

    var peer = d.peer || {};
    if (d.role == 'primary' || d.role == 'backup') {
        $('#foPeerRow').removeClass('d-none');
        var peerHtml = '';
        if (!peer.address) {
            peerHtml = '<span class="text-danger">no peer address configured</span>';
        } else {
            peerHtml = escapeHtml(peer.address);
            if (peer.host) peerHtml += ' (' + escapeHtml(peer.host) + ')';
            peerHtml += ' - ' + (peer.alive ? '<span class="text-success">reachable</span>' : '<span class="text-danger">UNREACHABLE</span>');
            if (peer.state) peerHtml += ', state <b>' + escapeHtml(peer.state) + '</b>';
            if (peer.manualStandby) peerHtml += ' (manual standby)';
            peerHtml += ', last contact ' + fmtAgo(peer.lastContactMsAgo);
            if (peer.playlist) {
                peerHtml += '<br><small>Last seen playing ' + escapeHtml(peer.playlist) + ' item ' + peer.index + ' of ' + peer.count +
                    ', observed ' + fmtAgo(peer.observedMsAgo) + '</small>';
            }
        }
        $('#foPeer').html(peerHtml);

        var w = d.witness || {};
        var wText = w.enabled ? 'enabled' : 'disabled';
        if (w.enabled) {
            wText += ' - ' + ((w.addresses || []).length) + ' witness(es)';
            if (w.lastVouchMsAgo >= 0) wText += ', last vouched for the peer ' + fmtAgo(w.lastVouchMsAgo) + ' by ' + escapeHtml(w.vouchedBy);
        }
        $('#foWitness').html(wText);

        var h = d.handoff || {};
        if (d.state == 'handoff_in' && h.activateInMs >= 0) {
            $('#foHandoff').text('activating in ' + Math.ceil(h.activateInMs / 1000) + 's at item ' + (h.nextIndex + 1) + (h.playlist ? ' of ' + h.playlist : ''));
            $('#foHandoffRow').removeClass('d-none');
        } else {
            $('#foHandoffRow').addClass('d-none');
        }
    } else {
        $('#foPeerRow').addClass('d-none');
        $('#foHandoffRow').addClass('d-none');
    }

    var r = d.remote || {};
    if (d.role == 'remote') {
        $('#foRemoteRow').removeClass('d-none');
        var rt = 'sync packets: ' + fmtAgo(r.lastSyncMsAgo);
        if (r.syncMasterIP) rt += ' from ' + escapeHtml(r.syncMasterIP);
        rt += '<br>heartbeats: ' + fmtAgo(r.lastHeartbeatMsAgo);
        if (r.activeMaster && r.activeMaster.host) {
            rt += '<br>active player: <b>' + escapeHtml(r.activeMaster.host) + '</b> (' + escapeHtml(r.activeMaster.role) + ', epoch ' + r.activeMaster.epoch + ') ' + fmtAgo(r.activeMasterMsAgo);
        }
        if (r.fallbackSequence) rt += '<br>fallback sequence: ' + escapeHtml(r.fallbackSequence);
        $('#foRemote').html(rt);
    } else {
        $('#foRemoteRow').addClass('d-none');
    }

    var cs = d.contentSync || {};
    if (cs.available) {
        $('#foSyncRow, #foContentPanel').removeClass('d-none');
        var st = '';
        if (cs.running) {
            st = '<span class="text-warning">' + escapeHtml(cs.runningMode || 'job') + ' running</span> (' + Math.round(cs.runningMs / 1000) + 's)';
        } else if (cs.lastRunMsAgo >= 0) {
            var last = cs.last || {};
            st = 'last ' + escapeHtml(last.action || 'promote') + ' ' +
                (cs.lastOK ? '<span class="text-success">ok</span>' : '<span class="text-danger">FAILED</span>') +
                ' ' + fmtAgo(cs.lastRunMsAgo);
            if (cs.lastOK && last.nothingToDo) st += ': nothing new';
            else if (cs.lastOK) st += ': ' + (last.transferred || 0) + ' file(s) updated, ' + (last.deleted || 0) + ' deleted' + (last.snapshot ? ', rollback point ' + escapeHtml(last.snapshot) : '');
            else if (last.error) st += ': ' + escapeHtml(last.error);
        } else {
            st = 'nothing promoted yet';
        }
        st += '<br><small>mode: ' + (cs.enabled ? (cs.mode == 'auto' ? 'automatic promote' + (cs.soakMin > 0 ? ' after ' + cs.soakMin + ' min soak' : '') : 'manual promote, automatic check') + ' every ' + cs.intervalMin + ' min' : 'off (manual only)') + '</small>';
        $('#foSync').html(st);

        // Pending changes on the primary (cutting edge vs. stable here)
        var pend = cs.pending || {};
        var ph = '';
        if (pend.ok === false && pend.error) {
            ph = '<span class="text-danger">check failed: ' + escapeHtml(pend.error) + '</span>';
        } else if (pend.ok && (pend.count > 0 || pend.deletions > 0)) {
            ph = '<b class="text-warning">' + pend.count + ' file(s) new or changed on the primary</b>' +
                (pend.deletions > 0 ? ', ' + pend.deletions + ' would be deleted' : '') +
                ' (checked ' + escapeHtml(pend.checkedAt || '') + ', first seen ' + fmtAgo(cs.pendingSinceMsAgo) + ')';
            var files = pend.files || [];
            if (files.length) {
                ph += '<ul class="mb-0 mt-1 small">';
                for (var f = 0; f < files.length && f < 60; f++) {
                    ph += '<li>' + escapeHtml(files[f].dir) + '/' + escapeHtml(files[f].name) + ' <span class="text-muted">' + escapeHtml(files[f].change) + '</span></li>';
                }
                if (files.length > 60) ph += '<li>&hellip; and ' + (files.length - 60) + ' more</li>';
                ph += '</ul>';
            }
        } else if (pend.ok) {
            ph = '<span class="text-success">stable set here matches the primary</span> (checked ' + escapeHtml(pend.checkedAt || '') + ')';
        } else {
            ph = '<span class="text-muted">not checked yet</span>';
        }
        $('#foPending').html(ph);

        // Snapshots (rollback points)
        var snaps = cs.snapshots || [];
        var sh = '';
        if (!snaps.length) {
            sh = '<span class="text-muted">no snapshots yet; the first promote creates one</span>';
        } else {
            sh = '<div class="table-responsive"><table class="table table-sm mb-0"><thead><tr><th>Snapshot</th><th>Created</th><th>Why</th><th>Files</th><th></th></tr></thead><tbody>';
            for (var s = 0; s < snaps.length; s++) {
                var sn = snaps[s];
                sh += '<tr><td><code>' + escapeHtml(sn.id) + '</code></td><td>' + escapeHtml(sn.createdAt || '') + '</td><td>' + escapeHtml(sn.reason || '') + (sn.source == 'primary' ? ' <span class="badge text-bg-secondary">primary</span>' : '') + '</td><td>' + (sn.files || 0) + '</td>' +
                    '<td class="text-nowrap"><button class="buttons btn-sm" type="button" onclick="rollbackTo(\'' + escapeHtml(sn.id) + '\', \'primary\')">Roll back primary</button> ' +
                    '<button class="buttons btn-sm" type="button" onclick="rollbackTo(\'' + escapeHtml(sn.id) + '\', \'local\')">Roll back here</button> ' +
                    '<button class="buttons btn-sm btn-danger" type="button" onclick="rollbackTo(\'' + escapeHtml(sn.id) + '\', \'both\')">Both</button></td></tr>';
            }
            sh += '</tbody></table></div>';
        }
        $('#foSnapshots').html(sh);
        $('#btnSync, #btnCheck, #foSnapshots button').prop('disabled', !!cs.running);
    } else {
        $('#foSyncRow, #foContentPanel').addClass('d-none');
    }

    var heard = d.heard || {};
    var keys = Object.keys(heard).sort();
    var ht = '';
    for (var i = 0; i < keys.length; i++) {
        ht += '<span class="badge text-bg-light border me-1 mb-1">' + escapeHtml(keys[i]) + ' <span class="text-muted">' + fmtAgo(heard[keys[i]]) + '</span></span>';
    }
    $('#foHeard').html(ht || '<span class="text-muted">nothing heard yet</span>');

    var ev = d.events || [];
    var et = '';
    for (var j = ev.length - 1; j >= 0; j--) {
        et += escapeHtml(ev[j]) + '\n';
    }
    $('#foEvents').text(et || 'no events yet');

    // Buttons that make sense for the current role/state.
    var isPlayer = (d.role == 'primary' || d.role == 'backup');
    $('#foButtons').toggleClass('d-none', !isPlayer);
    $('#btnTakeover, #btnTakeoverNow').prop('disabled', d.state == 'active' || d.state == 'takeover');
    $('#btnYield').prop('disabled', d.state != 'active' && d.state != 'handoff_out' && d.state != 'takeover');
    $('#btnFailback').prop('disabled', !(d.role == 'primary' && d.state != 'active') && !(d.role == 'backup' && d.state == 'active'));
    $('#btnResume').prop('disabled', !d.manualStandby);
}

function refreshFailoverStatus() {
    $.ajax({
        url: 'api/plugin-apis/failover?action=status',
        dataType: 'json',
        timeout: 4000,
        success: function (d) {
            $('#foUnavailable').addClass('d-none');
            $('#foStatusBody').removeClass('d-none');
            renderFailoverStatus(d);
        },
        error: function () {
            $('#foUnavailable').removeClass('d-none');
            $('#foStatusBody').addClass('d-none');
        }
    });
}

function failoverAction(action, body, label) {
    $.ajax({
        url: 'api/plugin-apis/failover?action=' + encodeURIComponent(action),
        method: 'POST',
        data: JSON.stringify(body || {}),
        contentType: 'application/json',
        dataType: 'json',
        success: function () {
            $.jGrowl(label + ' requested', { themeState: 'success' });
            setTimeout(refreshFailoverStatus, 600);
        },
        error: function (xhr) {
            var msg = label + ' failed';
            try { msg += ': ' + JSON.parse(xhr.responseText).message; } catch (e) { }
            $.jGrowl(msg, { themeState: 'danger' });
        }
    });
}

function confirmFailoverAction(action, body, label, question) {
    if (!confirm(question)) return;
    failoverAction(action, body, label);
}

function rollbackTo(id, target) {
    var where = target == 'primary' ? 'the PRIMARY' : target == 'local' ? 'this node' : 'BOTH the primary and this node';
    var q = 'Roll back ' + where + ' to snapshot ' + id + '?\n\n' +
        (target != 'local' ? 'The primary\'s current content is saved as a pre-rollback snapshot first, so this can be undone. ' : '') +
        'Files are replaced while the show runs; a sequence that is playing finishes with its current copy.';
    if (!confirm(q)) return;
    failoverAction('rollbackContent', { snapshot: id, target: target }, 'Rollback to ' + id);
}

function RoleChangedCallback() {
    var role = $('#Role').val();
    $('.foPlayerSetting').toggleClass('d-none', !(role == 'primary' || role == 'backup'));
    $('.foPrimarySetting').toggleClass('d-none', role != 'primary');
    $('.foBackupSetting').toggleClass('d-none', role != 'backup');
    $('.foRemoteSetting').toggleClass('d-none', role != 'remote');
    $('.foSyncSetting').toggleClass('d-none', !(role == 'backup' || role == 'remote'));
    $('.foPeerSetting').toggleClass('d-none', role == 'off');
    setTimeout(refreshFailoverStatus, 800);
}

$(document).ready(function () {
    refreshFailoverStatus();
    failoverStatusTimer = setInterval(refreshFailoverStatus, 2000);
    $('.foPlayerSetting').toggleClass('d-none', !('<?php echo $currentRole; ?>' == 'primary' || '<?php echo $currentRole; ?>' == 'backup'));
    $('.foPrimarySetting').toggleClass('d-none', '<?php echo $currentRole; ?>' != 'primary');
    $('.foBackupSetting').toggleClass('d-none', '<?php echo $currentRole; ?>' != 'backup');
    $('.foRemoteSetting').toggleClass('d-none', '<?php echo $currentRole; ?>' != 'remote');
    $('.foSyncSetting').toggleClass('d-none', !('<?php echo $currentRole; ?>' == 'backup' || '<?php echo $currentRole; ?>' == 'remote'));
    $('.foPeerSetting').toggleClass('d-none', '<?php echo $currentRole; ?>' == 'off');
});
</script>

<div id="failover" class="settings">
    <h2>Player Failover</h2>

    <div id="foUnavailable" class="alert alert-warning d-none">
        The failover plugin is not responding inside fppd. If it was just installed, wait a few seconds; otherwise check that fppd is running and that the plugin built correctly (see the plugin log in the Log viewer).
    </div>

    <div id="foStatusBody" class="d-none">
        <div class="container-fluid settingsTable settingsGroupTable">
            <div class="row mb-2">
                <div class="col-md-3 fw-bold">This node</div>
                <div class="col-md-9"><span id="foRole"></span> &middot; <span id="foState"></span> &middot; epoch <span id="foEpoch"></span>
                    <span id="foManual" class="badge text-bg-warning d-none">manual standby</span></div>
            </div>
            <div class="row mb-2">
                <div class="col-md-3 fw-bold">FPP mode</div>
                <div class="col-md-9" id="foMode"></div>
            </div>
            <div class="row mb-2 d-none" id="foStandbyRow">
                <div class="col-md-3 fw-bold">Standby type</div>
                <div class="col-md-9" id="foStandbyKind"></div>
            </div>
            <div class="row mb-2">
                <div class="col-md-3 fw-bold">Player</div>
                <div class="col-md-9" id="foPlayer"></div>
            </div>
            <div class="row mb-2 d-none" id="foPeerRow">
                <div class="col-md-3 fw-bold">Peer</div>
                <div class="col-md-9"><div id="foPeer"></div><div class="mt-1"><small>Witness check: <span id="foWitness"></span></small></div></div>
            </div>
            <div class="row mb-2 d-none" id="foHandoffRow">
                <div class="col-md-3 fw-bold">Handoff</div>
                <div class="col-md-9" id="foHandoff"></div>
            </div>
            <div class="row mb-2 d-none" id="foRemoteRow">
                <div class="col-md-3 fw-bold">Players heard</div>
                <div class="col-md-9" id="foRemote"></div>
            </div>
            <div class="row mb-2 d-none" id="foSyncRow">
                <div class="col-md-3 fw-bold">Content sync</div>
                <div class="col-md-9" id="foSync"></div>
            </div>
            <div class="row mb-2">
                <div class="col-md-3 fw-bold">Heard on the network</div>
                <div class="col-md-9" id="foHeard"></div>
            </div>
        </div>

        <div id="foButtons" class="d-flex flex-wrap gap-2 mt-3 mb-3 d-none">
            <button id="btnTakeover" class="buttons btn-success" type="button"
                onclick="confirmFailoverAction('takeover', {graceful: true, manual: true}, 'Graceful takeover', 'Take over the show from the peer at the end of its current item?')">Take Over (graceful)</button>
            <button id="btnTakeoverNow" class="buttons btn-danger" type="button"
                onclick="confirmFailoverAction('takeover', {graceful: false, manual: true}, 'Immediate takeover', 'Take over the show NOW? The peer is stopped immediately and remotes will restart the current item.')">Take Over Now</button>
            <button id="btnYield" class="buttons" type="button"
                onclick="confirmFailoverAction('yield', {manual: true, reason: 'yield button'}, 'Yield', 'Stop running the show on this node and stand by?')">Yield</button>
            <button id="btnFailback" class="buttons" type="button"
                onclick="failoverAction('failback', {}, 'Fail back')">Fail Back to Primary</button>
            <button id="btnResume" class="buttons" type="button"
                onclick="failoverAction('resume', {}, 'Resume automatic operation')">Clear Manual Standby</button>
        </div>

        <div id="foContentPanel" class="d-none">
            <h3 class="mt-3">Content: primary is the cutting edge, this node is stable</h3>
            <div class="container-fluid settingsTable settingsGroupTable">
                <div class="row mb-2">
                    <div class="col-md-3 fw-bold">Not yet promoted</div>
                    <div class="col-md-9" id="foPending"></div>
                </div>
                <div class="row mb-2">
                    <div class="col-md-3 fw-bold">Rollback points</div>
                    <div class="col-md-9" id="foSnapshots"></div>
                </div>
            </div>
            <div class="d-flex flex-wrap gap-2 mt-2 mb-3">
                <button id="btnCheck" class="buttons" type="button"
                    onclick="failoverAction('checkContent', {}, 'Content check')">Check Primary For Changes</button>
                <button id="btnSync" class="buttons btn-success" type="button"
                    onclick="confirmFailoverAction('promoteContent', {}, 'Promote', 'Promote the primary\'s current content to this node? The current stable set is saved as a rollback point first.')">Promote Primary Content To Here</button>
            </div>
        </div>

        <h3 class="mt-3">Recent events</h3>
        <pre id="foEvents" class="border rounded p-2 bg-body-tertiary mw-100 overflow-auto" style="max-height: 16rem;"></pre>
    </div>

    <h3 class="mt-4">Configuration</h3>
    <div class="container-fluid settingsTable settingsGroupTable">
        <div class="row mb-2">
            <div class="col-md-3">Role</div>
            <div class="col-md-9">
                <?php PrintSettingSelect('Role', 'Role', 0, 0, 'off', $roleOptions, $pluginName, 'RoleChangedCallback'); ?>
                <div class="form-text">Primary: the preferred player, in FPP Player mode with MultiSync enabled. Backup: put it in FPP <b>Remote</b> mode for hot standby; it mirrors the primary like any remote and takes the show over seamlessly, mid-song, when the primary fails. Remote: an FPP remote that reports what it hears and can run a fallback. Both players carry the same playlists, sequences, media and schedule.</div>
            </div>
        </div>

        <div class="row mb-2 foPeerSetting">
            <div class="col-md-3">Peer address</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('PeerAddress', 0, 0, 64, 32, $pluginName, ''); ?>
                <div class="form-text">IP address (preferred) or hostname of the other player. On the primary enter the backup; on the backup enter the primary. On a remote, enter the primary to enable content sync.</div>
            </div>
        </div>

        <div class="row mb-2 foPlayerSetting">
            <div class="col-md-3">Poll interval (ms)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('PollIntervalMS', 0, 0, 6, 6, $pluginName, '1000'); ?>
                <div class="form-text">How often the peer is polled and heartbeats are sent. Minimum 250.</div>
            </div>
        </div>

        <div class="row mb-2 foPlayerSetting">
            <div class="col-md-3">Failure threshold</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('FailureThreshold', 0, 0, 4, 4, $pluginName, '3'); ?>
                <div class="form-text">Consecutive poll intervals without any sign of the peer before it is considered dead. With a 1000 ms poll interval, 3 means a takeover roughly 3.5 seconds after the peer stops responding.</div>
            </div>
        </div>

        <div class="row mb-2 foPrimarySetting">
            <div class="col-md-3">Recovery threshold</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('RecoveryThreshold', 0, 0, 4, 4, $pluginName, '5'); ?>
                <div class="form-text">Consecutive successful polls of the backup before the primary asks for the show back. Prevents flapping on a bad link.</div>
            </div>
        </div>

        <div class="row mb-2 foPrimarySetting">
            <div class="col-md-3">Fail-back</div>
            <div class="col-md-9">
                <?php PrintSettingSelect('Fail-back', 'FailbackMode', 0, 0, 'auto', $failbackOptions, $pluginName); ?>
            </div>
        </div>

        <div class="row mb-2 foBackupSetting">
            <div class="col-md-3">Hot detect (ms)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('HotDetectMS', 0, 0, 6, 6, $pluginName, '1500'); ?>
                <div class="form-text">Hot standby only. While a sequence is playing the primary sends sync packets several times a second. If they stop for this long and HTTP is quiet too, take over immediately instead of waiting for the failure threshold. 0 disables.</div>
            </div>
        </div>

        <div class="row mb-2 foBackupSetting">
            <div class="col-md-3">Takeover lead (ms)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('TakeoverLeadMS', 0, 0, 5, 5, $pluginName, '40'); ?>
                <div class="form-text">Hot standby only. How far before the end of the mirrored item the backup's own playlist is started, so the next item follows without a gap.</div>
            </div>
        </div>

        <div class="row mb-2 foPlayerSetting">
            <div class="col-md-3">Resume position</div>
            <div class="col-md-9">
                <?php PrintSettingSelect('Resume position', 'ResumeMode', 0, 0, 'item', $resumeOptions, $pluginName); ?>
                <div class="form-text">Cold standby only (a backup in Player mode, or the primary taking over from a dead backup). Where the show continues; items restart from their beginning. A hot backup does not need this: it keeps the current item running and continues with the next.</div>
            </div>
        </div>

        <div class="row mb-2 foPlayerSetting">
            <div class="col-md-3">Handoff margin (ms)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('HandoffMarginMS', 0, 0, 5, 5, $pluginName, '300'); ?>
                <div class="form-text">Extra delay after the peer's item ends before this node starts the next one during a graceful handoff. Too small risks the peer's stop packet cutting off the first frames; too large leaves a visible gap.</div>
            </div>
        </div>

        <div class="row mb-2 foBackupSetting">
            <div class="col-md-3">Witness check</div>
            <div class="col-md-9">
                <?php PrintSettingCheckbox('Witness check', 'WitnessCheck', 0, 0, '1', '0', $pluginName, '', 0); ?>
                <div class="form-text">Before taking over, ask the remotes whether they still hear the primary. If one does, the problem is between this backup and the primary, not the primary itself, and the takeover is held off. Remotes need this plugin installed (role Remote) to answer.</div>
            </div>
        </div>

        <div class="row mb-2 foBackupSetting">
            <div class="col-md-3">Witness addresses</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('WitnessAddresses', 0, 0, 255, 48, $pluginName, ''); ?>
                <div class="form-text">Comma-separated remote addresses to ask. Leave empty to ask every FPP remote discovered by MultiSync.</div>
            </div>
        </div>

        <div class="row mb-2 foBackupSetting">
            <div class="col-md-3">Witness grace (s)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('WitnessGraceSec', 0, 0, 4, 4, $pluginName, '5'); ?>
                <div class="form-text">A witness that heard the primary within this many seconds counts as proof it is alive. This is also how long a <i>genuine</i> primary failure is delayed, because a witness really did hear it moments before it died. The primary sends a heartbeat every poll interval, so a few seconds is plenty; raise it only if your network drops multicast for long stretches.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Content pipeline</div>
            <div class="col-md-9">
                <?php PrintSettingCheckbox('Content pipeline', 'ContentSync', 0, 0, '1', '0', $pluginName, '', 0); ?>
                <div class="form-text">Track the primary's content using FPP's rsync daemon. <b>Enable rsync</b> under Settings &rarr; Services on the <i>primary</i> first (and here too if you want to roll the primary back). Runs only while this node is standing by and the primary is reachable; the buttons above work any time.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Promotion</div>
            <div class="col-md-9">
                <?php PrintSettingSelect('Promotion', 'ContentSyncMode', 0, 0, 'manual', $syncModeOptions, $pluginName); ?>
                <div class="form-text">Manual: this node only reports what changed on the primary; you press Promote when the new sequences are proven. Automatic: promote once the primary's changes have sat unchanged for the soak time.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Soak time (min)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('ContentSyncSoakMin', 0, 0, 6, 6, $pluginName, '0'); ?>
                <div class="form-text">Automatic promotion only. How long the primary's change set must stay unchanged before it is promoted. 0 promotes as soon as a change is seen. Something like 1440 (a day) means yesterday's uploads become stable only after they have run a show.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Check every (min)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('ContentSyncIntervalMin', 0, 0, 5, 5, $pluginName, '30'); ?>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Rollback points to keep</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('ContentSnapshots', 0, 0, 3, 3, $pluginName, '5'); ?>
                <div class="form-text">Every promote saves the previous stable set as a snapshot; every rollback of the primary saves its current content too. Snapshots share unchanged files with the live folders (hard links), so each one only costs the space of what changed. 0 disables snapshots and rollback.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Folders</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('ContentSyncDirs', 0, 0, 128, 48, $pluginName, 'sequences,music,videos,effects,images,scripts'); ?>
                <div class="form-text">Comma-separated media folders to mirror. Allowed: sequences, music, videos, effects, images, scripts, events, upload.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting foBackupSetting">
            <div class="col-md-3">Playlists</div>
            <div class="col-md-9">
                <?php PrintSettingCheckbox('Playlists', 'ContentSyncPlaylists', 0, 0, '1', '0', $pluginName, '', 1); ?>
                <div class="form-text">Mirror the playlists folder (the plugin's own hold playlist is never touched). Backup only.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting foBackupSetting">
            <div class="col-md-3">Schedule</div>
            <div class="col-md-9">
                <?php PrintSettingCheckbox('Schedule', 'ContentSyncSchedule', 0, 0, '1', '0', $pluginName, '', 1); ?>
                <div class="form-text">Copy the primary's schedule and reload it here. Backup only.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Delete extras</div>
            <div class="col-md-9">
                <?php PrintSettingCheckbox('Delete extras', 'ContentSyncDelete', 0, 0, '1', '0', $pluginName, '', 0); ?>
                <div class="form-text">Also delete local files in the synced folders that the primary no longer has. Off by default.</div>
            </div>
        </div>

        <div class="row mb-2 foSyncSetting">
            <div class="col-md-3">Compress transfers</div>
            <div class="col-md-9">
                <?php PrintSettingCheckbox('Compress transfers', 'ContentSyncCompress', 0, 0, '1', '0', $pluginName, '', 0); ?>
                <div class="form-text">Compress data in flight. Helps on slow links, costs CPU; leave off on a Pi Zero.</div>
            </div>
        </div>

        <div class="row mb-2 foRemoteSetting">
            <div class="col-md-3">Fallback sequence</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('RemoteFallbackSequence', 0, 0, 128, 32, $pluginName, ''); ?>
                <div class="form-text">Name of an .fseq file in this remote's sequences folder to loop when no player has been heard for the delay below. Leave empty to stay dark. The moment a player sends sync packets again, its sequence takes over.</div>
            </div>
        </div>

        <div class="row mb-2 foRemoteSetting">
            <div class="col-md-3">Fallback delay (s)</div>
            <div class="col-md-9">
                <?php PrintSettingTextSaved('RemoteFallbackDelaySec', 0, 0, 4, 4, $pluginName, '15'); ?>
                <div class="form-text">Seconds without sync packets or heartbeats before the fallback sequence starts. Keep it longer than the players' failure threshold so a normal failover never triggers it.</div>
            </div>
        </div>
    </div>
</div>
