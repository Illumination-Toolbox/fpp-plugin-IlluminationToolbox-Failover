/*
 * fpp-failover - automatic player failover for the Falcon Player (FPP)
 *
 * Two FPP players, a "primary" and a "backup", carry identical content and
 * schedules.  Exactly one runs the show at a time.
 *
 * The primary runs in Player mode.  When it must stand by (the backup has the
 * show), it "holds": it plays a private, silent hold playlist that is
 * protected from schedule override, so its scheduler cannot start the show.
 *
 * The backup runs in FPP Remote mode ("hot standby").  As a remote it mirrors
 * the primary frame for frame and its own outputs stay live.  When the
 * primary dies mid-sequence the backup keeps that sequence running, opens
 * the MultiSync send socket and becomes the sync source itself - the remotes
 * never see a restart - then switches fppd to Player mode in memory so its
 * scheduler carries on with the next item.  When the primary returns the show
 * is handed back at an item boundary and the backup drops back to Remote mode
 * in memory.  No fppd restarts anywhere.
 *
 * A backup that is in Player mode instead falls back to "cold standby": the
 * same hold-playlist mechanism as the primary, with resume at item level.
 *
 * On remotes the plugin adds visibility, a witness endpoint (used to avoid a
 * false takeover when only the link between the players is down) and an
 * optional local fallback sequence to run when no player is alive.
 *
 * This file is covered by the GPL v2 (see LICENSE).
 */

#include <fpp-pch.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "Plugin.h"
#include "Plugins.h"
#include "Player.h"
#include "Scheduler.h"
#include "Sequence.h"
#include "MultiSync.h"
#include "CurlManager.h"
#include "Timers.h"
#include "Warnings.h"
#include "common.h"
#include "settings.h"
#include "log.h"
#include "fpphttp.h"
#include "commands/Commands.h"
#include "playlist/Playlist.h"
#include "mediaoutput/mediaoutput.h"
#include "mediaoutput/MediaOutputBase.h"

namespace {

const char* const PLUGIN_NAME = "fpp-failover";
const char* const PLUGIN_VERSION = "1.2.0";
const char* const HOLD_PLAYLIST = "fpp-failover-hold";
const char* const TIMER_NAME = "fpp-failover-tick";
constexpr long long TICK_MS = 50;

// Warning ids are arbitrary but must not collide with FPP's own small ids.
constexpr int WARN_ID_MODE = 93101;
constexpr int WARN_ID_MULTISYNC = 93102;
constexpr int WARN_ID_PEER = 93103;
constexpr int WARN_ID_SYNC = 93104;

enum class Role {
    Off,
    Primary,
    Backup,
    Remote
};

enum class NodeState {
    Hold,           // standing by (hold playlist, or Remote mode on a hot backup)
    Active,         // running the show normally
    Takeover,       // hot takeover in progress: current item finishes as shadow
    HandoffOut,     // active, finishing the current item, then will stand by
    HandoffIn,      // standing by, will activate when the peer's item ends
    RemoteIdle,     // remote role: following whichever player is active
    RemoteFallback  // remote role: no player alive, running the fallback
};

const char* roleName(Role r) {
    switch (r) {
    case Role::Primary: return "primary";
    case Role::Backup: return "backup";
    case Role::Remote: return "remote";
    default: return "off";
    }
}

const char* stateName(NodeState s) {
    switch (s) {
    case NodeState::Hold: return "hold";
    case NodeState::Active: return "active";
    case NodeState::Takeover: return "takeover";
    case NodeState::HandoffOut: return "handoff_out";
    case NodeState::HandoffIn: return "handoff_in";
    case NodeState::RemoteIdle: return "remote";
    case NodeState::RemoteFallback: return "remote_fallback";
    }
    return "unknown";
}

Role parseRole(const std::string& s) {
    if (s == "primary") return Role::Primary;
    if (s == "backup") return Role::Backup;
    if (s == "remote") return Role::Remote;
    return Role::Off;
}

bool peerStateIsActive(const std::string& s) {
    return s == "active" || s == "takeover" || s == "handoff_out";
}

int toInt(const std::string& s, int def) {
    if (s.empty()) return def;
    try {
        return std::stoi(s);
    } catch (...) {
        return def;
    }
}

// Playlist status fields come back as strings from GetCurrentStatus().
int jsonInt(const Json::Value& v, const char* key, int def = 0) {
    if (!v.isMember(key)) return def;
    const Json::Value& j = v[key];
    if (j.isInt()) return j.asInt();
    if (j.isString()) return toInt(j.asString(), def);
    if (j.isNumeric()) return (int)j.asDouble();
    return def;
}

double jsonDouble(const Json::Value& v, const char* key, double def = 0.0) {
    if (!v.isMember(key)) return def;
    const Json::Value& j = v[key];
    if (j.isNumeric()) return j.asDouble();
    if (j.isString()) {
        try {
            return std::stod(j.asString());
        } catch (...) {
            return def;
        }
    }
    return def;
}

std::string timestamp() {
    char buf[32];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// What this node knows about its peer player, from HTTP polls, heartbeats and
// (on a hot backup) the primary's own sync packets.
struct PeerInfo {
    bool valid = false;          // at least one successful status poll
    long long lastContactMS = 0; // any proof of life
    long long lastOkMS = 0;      // last successful status poll
    int okCount = 0;             // consecutive successful polls
    int failCount = 0;           // consecutive failed polls
    std::string role;
    std::string state;
    std::string host;
    int epoch = 0;
    bool manualStandby = false;
    // Snapshot of the peer's player, used to resume at the right item.
    std::string playlist;
    int index = 0; // 1-based, as reported by FPP
    int count = 0;
    double secondsPlayed = 0;
    double secondsRemaining = 0;
    long long observedMS = 0;
};

} // namespace

class FPPFailoverPlugin : public FPPPlugins::Plugin,
                          public FPPPlugins::APIProviderPlugin,
                          public FPPPlugins::ChannelDataPlugin,
                          public MultiSyncPlugin {
public:
    FPPFailoverPlugin() :
        FPPPlugins::Plugin(PLUGIN_NAME, true),
        FPPPlugins::APIProviderPlugin(),
        FPPPlugins::ChannelDataPlugin() {
        m_logFile = FPP_DIR_MEDIA("/logs/plugin-fpp-failover.log");
        m_dataDir = FPP_DIR_MEDIA("/plugindata/fpp-failover");
        m_stateFile = m_dataDir + "/state.json";
        std::error_code ec;
        std::filesystem::create_directories(m_dataDir, ec);

        m_hostName = getSetting("HostName", "FPP");
        // fppd started in Remote mode means MultiSync::Init() left native sync
        // sending off for the life of this process.  That is exactly what a hot
        // backup needs: it can become the sync source itself without fppd also
        // sending, and can flip to Player mode in memory without a restart.
        m_bootedRemote = (getFPPmode() == REMOTE_MODE);

        loadConfig();
        loadState();
        registerCommands();
        multiSync->addMultiSyncPlugin(this);

        long long now = GetTimeMS();
        m_startMS = now;
        // Give the peer a full detection window before it can be declared dead.
        m_peer.lastContactMS = now;
        m_lastPollSentMS = 0;

        initialiseForRole();

        Timers::INSTANCE.addPeriodicTimer(TIMER_NAME, TICK_MS, [this]() { tick(); });
        logEvent("Plugin loaded, role=" + std::string(roleName(m_role)) +
                 ", state=" + stateName(m_state) + ", epoch=" + std::to_string(m_epoch) +
                 (m_bootedRemote ? " (hot standby capable)" : ""));
    }

    virtual ~FPPFailoverPlugin() {}

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------
    virtual std::function<bool()> shutdown() override {
        std::unique_lock<std::recursive_mutex> lock(m_lock);
        Timers::INSTANCE.stopPeriodicTimer(TIMER_NAME);
        m_syncSendActive = false;
        if (m_syncPid > 0) {
            kill(m_syncPid, SIGTERM);
            m_syncPid = 0;
        }
        CurlManager::INSTANCE.cancelRequests(PLUGIN_NAME);
        multiSync->removeMultiSyncPlugin(this);
        removeCommands();
        clearWarnings();
        if (m_forcePlayerMode) {
            // We are acting as the player in memory, on a box the user
            // configured as a Remote-mode hot standby.  Deliberately do NOT
            // persist that: writing fppMode=player would outlive this session
            // and the box would come back from its next reboot as a second
            // full player, running the schedule against the primary and
            // fighting it for the remotes - with no plugin left to resolve it.
            // The on-disk setting stays "remote" (what the user asked for) and
            // the restart flag tells them the box needs a restart, after which
            // it is a proper remote again.  Only the running process is left in
            // player mode, because the show is mid-flight and cannot be moved.
            setSetting("restartFlag", "1");
            WarningHolder::AddWarningTimeout(3600, WARN_ID_SYNC,
                                             "fpp-failover was unloaded while acting as the player; "
                                             "restart fppd to return this node to Remote mode",
                                             {}, PLUGIN_NAME);
            logEvent("Plugin unloading while acting as player - on-disk mode left as configured, fppd restart needed");
        }
        // Never leave the hold playlist behind: without the plugin it would
        // block this node's schedule forever.
        if (isHoldPlaying()) {
            Player::INSTANCE.StopNow(1);
            logEvent("Plugin unloading, hold released");
        }
        // Do not report "done" yet.  This plugin is a ChannelDataPlugin: the
        // channel output thread calls modifyChannelData() every frame.  FPP
        // detaches us from that list right after this returns, then destroys us
        // and - because we declare FPP_PLUGIN_SUPPORTS_UNLOAD - dlclose()s the
        // library as soon as this predicate is true.
        //
        // FPP 10.2 holds a lock across the per-frame walk, so the detach waits
        // for any in-flight call.  FPP 10.0 to 10.1.x do not: the walk is
        // unlocked, so a frame that started before the detach can still be
        // inside modifyChannelData() when the library is unmapped, which is a
        // jump into unmapped memory.  Returning nullptr (what this used to do)
        // made FPP destroy and unmap immediately, inside that window.
        //
        // Waiting a second lets any such frame finish against a still-live
        // object - it only reads m_syncSendActive, cleared above, and returns.
        // A single call takes microseconds and FPP calls a frame slow past
        // 150 ms, so a second is a wide margin.  It costs nothing on 10.2 but a
        // slightly later teardown.  The lambda captures no pointer into us, so
        // it is safe for FPP to run it after we are detached.
        const long long settleUntil = GetTimeMS() + 1000;
        return [settleUntil]() { return GetTimeMS() >= settleUntil; };
    }

    virtual void registerApis() override {
        FPPPlugins::registerPluginApi(
            "/failover",
            [this](const HttpRequestPtr& req, HttpCallback&& callback) {
                handleRequest(req, std::move(callback));
            },
            { drogon::Get, drogon::Post }, true);
    }

    virtual void unregisterApis() override {
        FPPPlugins::unregisterPluginApi("/failover");
    }

    virtual void settingChanged(const std::string& key, const std::string& value) override {
        std::unique_lock<std::recursive_mutex> lock(m_lock);
        Role oldRole = m_role;
        std::string oldPeer = m_peerAddress;
        loadConfig();
        if (m_role != oldRole) {
            logEvent("Role changed from " + std::string(roleName(oldRole)) + " to " + roleName(m_role));
            leaveRole(oldRole);
            initialiseForRole();
        } else if (m_peerAddress != oldPeer) {
            logEvent("Peer address changed to " + m_peerAddress);
            m_peer = PeerInfo();
            m_peer.lastContactMS = GetTimeMS();
        }
    }

    // ------------------------------------------------------------------
    // MultiSync receive hooks - a player is alive when these fire
    // ------------------------------------------------------------------
    virtual void ReceivedSeqOpenPacket(const std::string&) override { noteSync(); }
    virtual void ReceivedSeqSyncStartPacket(const std::string&) override { noteSync(); }
    virtual void ReceivedSeqSyncPacket(const std::string&, int, float) override { noteSync(); }
    virtual void ReceivedSeqSyncStopPacket(const std::string&) override { noteSync(); }
    virtual void ReceivedMediaOpenPacket(const std::string&) override { noteSync(); }
    virtual void ReceivedMediaSyncStartPacket(const std::string&) override { noteSync(); }
    virtual void ReceivedMediaSyncPacket(const std::string&, float) override { noteSync(); }
    virtual void ReceivedMediaSyncStopPacket(const std::string&) override { noteSync(); }

    // Heartbeat packets from other fpp-failover nodes arrive here.
    virtual void multiSyncData(const uint8_t* data, int len) override {
        if (!data || len <= 0) return;
        std::string s((const char*)data, strnlen((const char*)data, len));
        Json::Value hb;
        if (!LoadJsonFromString(s, hb) || !hb.isObject()) return;
        if (hb["plugin"].asString() != PLUGIN_NAME) return;

        std::unique_lock<std::recursive_mutex> lock(m_lock);
        long long now = GetTimeMS();
        std::vector<std::string> ids;
        if (hb.isMember("addresses") && hb["addresses"].isArray()) {
            for (const auto& a : hb["addresses"]) ids.push_back(a.asString());
        }
        std::string host = hb["host"].asString();
        if (!host.empty()) ids.push_back(host);
        // Ignore our own broadcasts.
        for (auto& id : ids) {
            if (id == m_hostName) return;
            for (auto& mine : m_localAddresses) {
                if (id == mine) return;
            }
        }
        bool fromPeer = false;
        for (auto& id : ids) {
            m_heard[id] = now;
            if (!m_peerAddress.empty() && id == m_peerAddress) fromPeer = true;
        }
        m_lastHeartbeatMS = now;
        if (peerStateIsActive(hb["state"].asString())) {
            m_lastActiveHeartbeatMS = now;
            m_activeMaster = hb;
        }
        if (fromPeer) {
            m_peer.lastContactMS = now;
            if (hb.isMember("state")) m_peer.state = hb["state"].asString();
            if (hb.isMember("role")) m_peer.role = hb["role"].asString();
            if (hb.isMember("epoch")) m_peer.epoch = hb["epoch"].asInt();
            if (hb.isMember("manualStandby")) m_peer.manualStandby = hb["manualStandby"].asBool();
        }
    }

    // ------------------------------------------------------------------
    // Sync sender: runs on the channel output thread, once per frame.
    //
    // While this node acts as the player without fppd's native sync sending
    // (hot backup after takeover), announce every sequence the playlist runs
    // and keep the remotes' position updated - the same packets fppd's own
    // output thread would send.
    // ------------------------------------------------------------------
    virtual void modifyChannelData(int ms, uint8_t* seqData) override {
        if (!m_syncSendActive.load(std::memory_order_relaxed)) return;
        if (!sequence->IsSequenceRunning()) return;
        std::string name = sequence->GetSeqFilenameCopy();
        if (name.empty()) return;
        int step = sequence->GetSeqStepTime();
        if (step <= 0) return;
        int frame = ms / step;

        m_shadowRemainingMS = sequence->m_seqMSDuration - ms;
        m_shadowLastFrameMS = GetTimeMS();

        if (m_suppressAnnounce.load(std::memory_order_relaxed)) return;

        std::lock_guard<std::mutex> l(m_sendLock);
        if (name != m_sendSeqName) {
            // Wait for a second frame of the same file before announcing it,
            // so a sequence the playlist opened and immediately replaced
            // (see completeActivation) never reaches the remotes.
            if (name == m_candidateName) {
                m_candidateCount++;
            } else {
                m_candidateName = name;
                m_candidateCount = 1;
            }
            if (m_candidateCount < 2) return;
            multiSync->SendSeqOpenPacket(name);
            multiSync->SendSeqSyncStartPacket(name);
            m_sendSeqName = name;
            m_candidateName.clear();
            m_candidateCount = 0;
        }
        // MultiSync rate-limits this to every 10 frames itself.
        multiSync->SendSeqSyncPacket(name, frame, (float)frame * step / 1000.0f);
    }

private:
    // ------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------
    void loadConfig() {
        m_role = parseRole(settings["Role"]);
        m_peerAddress = settings["PeerAddress"];
        m_pollIntervalMS = std::max(250, toInt(settings["PollIntervalMS"], 1000));
        m_failureThreshold = std::max(1, toInt(settings["FailureThreshold"], 3));
        m_recoveryThreshold = std::max(1, toInt(settings["RecoveryThreshold"], 5));
        m_failbackMode = settings["FailbackMode"].empty() ? "auto" : settings["FailbackMode"];
        m_witnessCheck = settings["WitnessCheck"] == "1";
        m_witnessAddresses = settings["WitnessAddresses"];
        m_witnessGraceSec = std::max(2, toInt(settings["WitnessGraceSec"], 5));
        m_handoffMarginMS = std::max(0, toInt(settings["HandoffMarginMS"], 300));
        m_resumeMode = settings["ResumeMode"].empty() ? "item" : settings["ResumeMode"];
        m_hotDetectMS = std::max(0, toInt(settings["HotDetectMS"], 1500));
        m_takeoverLeadMS = std::max(0, toInt(settings["TakeoverLeadMS"], 40));
        m_remoteFallbackSequence = settings["RemoteFallbackSequence"];
        m_remoteFallbackDelaySec = std::max(2, toInt(settings["RemoteFallbackDelaySec"], 15));
        m_contentSync = settings["ContentSync"] == "1";
        m_contentSyncIntervalMin = std::max(1, toInt(settings["ContentSyncIntervalMin"], 30));
        m_contentSyncDirs = settings["ContentSyncDirs"].empty() ? "sequences,music,videos,effects,images,scripts"
                                                                : settings["ContentSyncDirs"];
        m_contentSyncPlaylists = settings["ContentSyncPlaylists"].empty() ? true : settings["ContentSyncPlaylists"] == "1";
        m_contentSyncSchedule = settings["ContentSyncSchedule"].empty() ? true : settings["ContentSyncSchedule"] == "1";
        m_contentSyncDelete = settings["ContentSyncDelete"] == "1";
        m_contentSyncCompress = settings["ContentSyncCompress"] == "1";
        m_contentSyncMode = settings["ContentSyncMode"].empty() ? "manual" : settings["ContentSyncMode"];
        m_contentSyncSoakMin = std::max(0, toInt(settings["ContentSyncSoakMin"], 0));
        m_contentSnapshots = std::max(0, toInt(settings["ContentSnapshots"], 5));
        refreshLocalAddresses();
    }

    // ------------------------------------------------------------------
    // Content pipeline: the primary is the cutting edge, this node keeps the
    // last promoted ("stable") set plus rollback snapshots.
    //
    // The work happens in scripts/sync_content.sh as a child process with a
    // mode: "check" (dry run: what would a promote change), "promote"
    // (snapshot stable, pull the primary's content), "rollback <id> <target>".
    // fppd reaps children automatically (SA_NOCLDWAIT), so completion is
    // detected by the pid disappearing and the result read from a file.
    // ------------------------------------------------------------------
    std::string syncStatusFile() { return m_dataDir + "/sync-status.json"; }
    std::string syncPendingFile() { return m_dataDir + "/sync-pending.json"; }
    std::string snapshotDir() { return m_dataDir + "/snapshots"; }

    bool contentSyncRoleOK() { return m_role == Role::Backup || m_role == Role::Remote; }

    bool contentSyncStateOK() {
        // Only while standing by: an active player is running the show from
        // these files, and the primary is unreachable then anyway.
        return m_state == NodeState::Hold || m_state == NodeState::HandoffIn ||
               m_state == NodeState::RemoteIdle || m_state == NodeState::RemoteFallback;
    }

    // Launch the script.  mode: check | promote | rollback.
    bool startContentJob(const std::string& mode, const std::string& reason,
                         const std::string& arg1 = "", const std::string& arg2 = "") {
        if (m_syncPid > 0) return false;
        if (m_peerAddress.empty()) {
            logEvent("Content " + mode + " skipped: no peer address configured");
            return false;
        }
        std::string script = FPP_DIR_PLUGIN(std::string("/") + PLUGIN_NAME + "/scripts/sync_content.sh");
        if (!FileExists(script)) {
            logEvent("Content sync script missing: " + script);
            return false;
        }
        std::string fppDir = FPP_DIR;
        std::string mediaDir = FPP_DIR_MEDIA("");
        std::string statusFile = syncStatusFile();
        std::string pendingFile = syncPendingFile();
        std::string snapDir = snapshotDir();
        std::string dirs = m_contentSyncDirs;
        std::string snaps = std::to_string(m_contentSnapshots);
        // Playlists and the schedule only matter on a backup player.
        bool playlists = m_contentSyncPlaylists && m_role == Role::Backup;
        bool schedule = m_contentSyncSchedule && m_role == Role::Backup;

        pid_t pid = fork();
        if (pid < 0) {
            logEvent("Content " + mode + ": fork failed");
            return false;
        }
        if (pid == 0) {
            CloseOpenFiles(getSettingInt("daemonize"));
            setenv("FPPDIR", fppDir.c_str(), 1);
            setenv("MEDIADIR", mediaDir.c_str(), 1);
            setenv("SYNC_DIRS", dirs.c_str(), 1);
            setenv("SYNC_PLAYLISTS", playlists ? "1" : "0", 1);
            setenv("SYNC_SCHEDULE", schedule ? "1" : "0", 1);
            setenv("SYNC_DELETE", m_contentSyncDelete ? "1" : "0", 1);
            setenv("SYNC_COMPRESS", m_contentSyncCompress ? "1" : "0", 1);
            setenv("SYNC_SNAPSHOTS", snaps.c_str(), 1);
            setenv("SYNC_STATUS_FILE", statusFile.c_str(), 1);
            setenv("SYNC_PENDING_FILE", pendingFile.c_str(), 1);
            setenv("SYNC_SNAPSHOT_DIR", snapDir.c_str(), 1);
            if (!arg1.empty()) {
                execl("/bin/bash", "bash", script.c_str(), m_peerAddress.c_str(), mode.c_str(),
                      arg1.c_str(), arg2.c_str(), (char*)nullptr);
            } else {
                execl("/bin/bash", "bash", script.c_str(), m_peerAddress.c_str(), mode.c_str(), (char*)nullptr);
            }
            _exit(127);
        }
        m_syncPid = pid;
        m_syncMode = mode;
        m_syncStartedMS = GetTimeMS();
        if (mode != "check") {
            logEvent("Content " + mode + (arg1.empty() ? "" : " " + arg1 + " " + arg2) +
                     " started (" + reason + ")");
        }
        return true;
    }

    void finishContentJob() {
        std::string mode = m_syncMode;
        m_syncPid = 0;
        m_syncMode.clear();
        long long now = GetTimeMS();
        Json::Value st;
        if (mode == "check") {
            m_lastCheckMS = now;
            if (FileExists(syncPendingFile()) && LoadJsonFromFile(syncPendingFile(), st) && st.isObject()) {
                m_pending = st;
                std::string fp = st.get("fingerprint", "").asString();
                int count = st.get("count", 0).asInt() + st.get("deletions", 0).asInt();
                if (!st.get("ok", false).asBool()) {
                    m_pendingFingerprint.clear();
                    m_pendingSinceMS = 0;
                } else if (count == 0) {
                    m_pendingFingerprint.clear();
                    m_pendingSinceMS = 0;
                } else if (fp != m_pendingFingerprint) {
                    m_pendingFingerprint = fp;
                    m_pendingSinceMS = now;
                    logEvent("Primary has " + std::to_string(count) + " file change(s) not yet promoted");
                }
            }
            return;
        }
        m_lastSyncMS = now;
        if (FileExists(syncStatusFile()) && LoadJsonFromFile(syncStatusFile(), st) && st.isObject()) {
            m_lastSyncStatus = st;
            m_lastSyncOK = st.get("ok", false).asBool();
            if (m_lastSyncOK) {
                if (st.get("nothingToDo", false).asBool()) {
                    logEvent("Content promote: nothing new on the primary");
                } else {
                    logEvent("Content " + mode + " finished: " + std::to_string(st.get("transferred", 0).asInt()) +
                             " file(s) updated, " + std::to_string(st.get("deleted", 0).asInt()) + " deleted" +
                             (st.isMember("snapshot") ? " (snapshot " + st["snapshot"].asString() + ")" : ""));
                }
                if (mode == "promote") {
                    m_pending = Json::Value();
                    m_pendingFingerprint.clear();
                    m_pendingSinceMS = 0;
                }
            } else {
                logEvent("Content " + mode + " FAILED: " + st.get("error", "unknown error").asString());
            }
        } else {
            m_lastSyncOK = false;
            logEvent("Content " + mode + " ended without a result file");
        }
    }

    void contentSyncTick(long long now) {
        if (m_syncPid > 0) {
            if (kill(m_syncPid, 0) != 0) {
                finishContentJob();
            } else if ((now - m_syncStartedMS) > 4LL * 3600 * 1000) {
                logEvent("Content job running for over 4 hours, stopping it");
                kill(m_syncPid, SIGTERM);
            }
            return;
        }
        if (!contentSyncRoleOK()) return;
        // Manual requests first.
        if (!m_syncRequest.empty()) {
            std::string req = m_syncRequest;
            m_syncRequest.clear();
            if (req == "promote") {
                startContentJob("promote", "requested");
            } else if (req == "check") {
                startContentJob("check", "requested");
            } else if (req == "rollback") {
                startContentJob("rollback", "requested", m_syncRequestSnapshot.empty() ? "latest" : m_syncRequestSnapshot,
                                m_syncRequestTarget.empty() ? "both" : m_syncRequestTarget);
            }
            return;
        }
        if (!m_contentSync || !contentSyncStateOK() || !peerFresh(now)) return;
        if (m_nextSyncMS == 0) m_nextSyncMS = now + 60000; // settle after load
        if (now < m_nextSyncMS) return;
        m_nextSyncMS = now + (long long)m_contentSyncIntervalMin * 60000;

        // Automatic promotion: only after the primary's change set has sat
        // unchanged for the soak time (0 = promote as soon as seen).
        bool havePending = m_pending.isObject() && m_pending.get("ok", false).asBool() &&
                           (m_pending.get("count", 0).asInt() + m_pending.get("deletions", 0).asInt()) > 0;
        if (m_contentSyncMode == "auto" && havePending) {
            long long soakMS = (long long)m_contentSyncSoakMin * 60000;
            if (soakMS == 0 || (m_pendingSinceMS && (now - m_pendingSinceMS) >= soakMS)) {
                startContentJob("promote", soakMS ? "automatic, changes stable for " +
                                                        std::to_string(m_contentSyncSoakMin) + " min"
                                                  : "automatic");
                return;
            }
        }
        startContentJob("check", "scheduled");
    }

    Json::Value snapshotList() {
        Json::Value list(Json::arrayValue);
        std::error_code ec;
        std::vector<std::string> ids;
        for (const auto& e : std::filesystem::directory_iterator(snapshotDir(), ec)) {
            if (e.is_directory(ec)) ids.push_back(e.path().filename().string());
        }
        std::sort(ids.rbegin(), ids.rend());
        for (auto& id : ids) {
            Json::Value m;
            std::string mf = snapshotDir() + "/" + id + "/manifest.json";
            if (FileExists(mf) && LoadJsonFromFile(mf, m) && m.isObject()) {
                m["id"] = id;
                list.append(m);
            }
        }
        return list;
    }

    void refreshLocalAddresses() {
        m_localAddresses.clear();
        Json::Value sys = multiSync->GetSystems(true, false);
        if (sys.isMember("systems")) {
            for (const auto& s : sys["systems"]) {
                std::string a = s["address"].asString();
                if (!a.empty()) m_localAddresses.push_back(a);
            }
        }
    }

    void loadState() {
        Json::Value st;
        if (FileExists(m_stateFile) && LoadJsonFromFile(m_stateFile, st) && st.isObject()) {
            m_epoch = st.get("epoch", 0).asInt();
            m_manualStandby = st.get("manualStandby", false).asBool();
        }
    }

    void saveState() {
        Json::Value st;
        st["epoch"] = m_epoch;
        st["manualStandby"] = m_manualStandby;
        st["role"] = roleName(m_role);
        st["state"] = stateName(m_state);
        st["savedAt"] = timestamp();
        SaveJsonToFile(st, m_stateFile);
    }

    // Hot standby is available when this is a backup and fppd was started in
    // Remote mode.
    bool hotCapable() const { return m_role == Role::Backup && m_bootedRemote; }

    // ------------------------------------------------------------------
    // Logging
    // ------------------------------------------------------------------
    void logEvent(const std::string& msg) {
        LogInfo(VB_PLUGIN, "fpp-failover: %s\n", msg.c_str());
        std::string line = timestamp() + " " + msg;
        {
            std::unique_lock<std::mutex> l(m_eventsLock);
            m_events.push_back(line);
            while (m_events.size() > 50) m_events.pop_front();
        }
        std::ofstream f(m_logFile, std::ios::app);
        if (f) f << line << "\n";
    }

    // ------------------------------------------------------------------
    // Role setup
    // ------------------------------------------------------------------
    void initialiseForRole() {
        m_handoffActivateAtMS = 0;
        m_pendingAction.clear();
        if (m_role == Role::Primary || m_role == Role::Backup) {
            if (Player::INSTANCE.GetStatus() != FPP_STATUS_IDLE && Player::INSTANCE.WasScheduled()) {
                // A scheduled show is already running (plugin loaded or
                // enabled mid-show): we are the active player.
                m_state = NodeState::Active;
                m_activatedMS = GetTimeMS();
                // fppd's scheduler starts the show before this plugin loads, so
                // "a show is running" does not yet mean we are the right node to
                // run it.  If the first peer poll says the peer is already
                // running it (we rebooted mid-failover), hand it back to them
                // rather than cutting the show to our item 1 - see evaluate().
                m_bootAssumedActive = true;
                logEvent("Scheduled show already running, assuming active");
            } else {
                m_state = NodeState::Hold;
                startHold();
                if (hotCapable()) {
                    logEvent("Hot standby: mirroring the primary as an FPP remote");
                }
            }
        } else if (m_role == Role::Remote) {
            m_state = NodeState::RemoteIdle;
        } else {
            m_state = NodeState::Hold; // meaningless while off
        }
        saveState();
    }

    void leaveRole(Role old) {
        if (old == Role::Primary || old == Role::Backup) {
            if (m_forcePlayerMode) {
                if (isShowPlaying()) Player::INSTANCE.StopNow(1);
                leavePlayerMode();
            }
            if (isHoldPlaying()) {
                Player::INSTANCE.StopNow(1);
                logEvent("Hold released (role change)");
            }
        }
        if (old == Role::Remote && m_state == NodeState::RemoteFallback) {
            stopFallback();
        }
        clearWarnings();
    }

    // ------------------------------------------------------------------
    // Hold playlist (cold standby, and the guard during a hot takeover)
    // ------------------------------------------------------------------
    void ensureHoldPlaylistFile() {
        std::string file = FPP_DIR_PLAYLIST(std::string("/") + HOLD_PLAYLIST + ".json");
        if (FileExists(file)) return;
        Json::Value pl;
        pl["name"] = HOLD_PLAYLIST;
        pl["desc"] = "Standby hold playlist created by the fpp-failover plugin. Do not edit or delete.";
        pl["version"] = 4;
        pl["repeat"] = 1;
        pl["loopCount"] = 0;
        pl["empty"] = false;
        pl["random"] = 0;
        pl["leadIn"] = Json::Value(Json::arrayValue);
        pl["leadOut"] = Json::Value(Json::arrayValue);
        Json::Value entry;
        entry["type"] = "pause";
        entry["enabled"] = 1;
        entry["playOnce"] = 0;
        entry["duration"] = 3600;
        entry["note"] = "fpp-failover standby hold";
        Json::Value main(Json::arrayValue);
        main.append(entry);
        pl["mainPlaylist"] = main;
        Json::Value info;
        info["total_duration"] = 3600;
        info["total_items"] = 1;
        info["leadIn_duration"] = 0;
        info["leadIn_items"] = 0;
        info["mainPlaylist_duration"] = 3600;
        info["mainPlaylist_items"] = 1;
        info["leadOut_duration"] = 0;
        info["leadOut_items"] = 0;
        pl["playlistInfo"] = info;
        SaveJsonToFile(pl, file);
        logEvent("Created hold playlist " + file);
    }

    bool isHoldPlaying() {
        return Player::INSTANCE.GetStatus() != FPP_STATUS_IDLE &&
               Player::INSTANCE.GetPlaylistName() == HOLD_PLAYLIST;
    }

    bool isShowPlaying() {
        return Player::INSTANCE.GetStatus() != FPP_STATUS_IDLE &&
               Player::INSTANCE.GetPlaylistName() != HOLD_PLAYLIST;
    }

    // Start the hold playlist.  It is a manual playlist protected from
    // schedule override, which is what keeps the scheduler from starting the
    // show on this node.  A hot backup in Remote mode needs no hold: remotes
    // never run schedules.
    void startHold() {
        if (getFPPmode() != PLAYER_MODE) return;
        if (isHoldPlaying()) return;
        ensureHoldPlaylistFile();
        Player::INSTANCE.StartPlaylist(HOLD_PLAYLIST, 1, -1, -1, 1000, false);
        m_holdStartedMS = GetTimeMS();
        LogDebug(VB_PLUGIN, "fpp-failover: hold playlist started\n");
    }

    void stopHold() {
        if (isHoldPlaying()) {
            Player::INSTANCE.StopNow(1);
        }
    }

    // ------------------------------------------------------------------
    // Hot standby: acting as the player from a Remote-mode fppd
    // ------------------------------------------------------------------
    // fppd with MultiSync off falls back to multicast only when the setting
    // is on; our sender needs at least one real send method configured.
    void ensureSendMethod() {
        if (getSettingInt("MultiSyncMulticast") == 1 || getSettingInt("MultiSyncBroadcast") == 1) return;
        setSetting("MultiSyncMulticast", "1", true);
        logEvent("Enabled MultiSync multicast so remotes can hear this player");
    }

    void enterPlayerMode() {
        if (m_forcePlayerMode) return;
        ensureSendMethod();
        if (!multiSync->OpenControlSockets()) {
            logEvent("ERROR: could not open the MultiSync send socket");
        }
        {
            std::lock_guard<std::mutex> l(m_sendLock);
            // Whatever is running right now was announced by the primary and
            // is already playing on the remotes: never re-announce it.
            m_sendSeqName = sequence->IsSequenceRunning() ? sequence->GetSeqFilenameCopy() : "";
            m_candidateName.clear();
            m_candidateCount = 0;
            std::unique_lock<std::mutex> ml(mediaOutputLock);
            if (mediaOutput) {
                m_sendMediaName = mediaOutput->m_mediaFilename;
                m_sendMediaStarted = (mediaOutputStatus.status == MEDIAOUTPUTSTATUS_PLAYING);
            } else {
                m_sendMediaName.clear();
                m_sendMediaStarted = false;
            }
        }
        m_syncSendActive = true;
        setSetting("fppMode", "player", false);
        m_forcePlayerMode = true;
        // Block the scheduler until the current item has finished, then
        // rebuild its item list under Player mode.
        startHold();
        scheduler->ReloadScheduleFile();
        m_schedReloadRequestedMS = GetTimeMS();
        logEvent("Now the MultiSync source; fppd switched to Player mode in memory");
    }

    void leavePlayerMode() {
        if (!m_forcePlayerMode) return;
        m_syncSendActive = false;
        {
            std::lock_guard<std::mutex> l(m_sendLock);
            // If we were cut off mid-item, tell the remotes so they do not
            // free-run something nobody is driving any more.
            if (!m_sendSeqName.empty() && !sequence->IsSequenceRunning(m_sendSeqName)) {
                multiSync->SendSeqSyncStopPacket(m_sendSeqName);
            }
            if (!m_sendMediaName.empty()) {
                multiSync->SendMediaSyncStopPacket(m_sendMediaName);
            }
            m_sendSeqName.clear();
            m_candidateName.clear();
            m_candidateCount = 0;
            m_sendMediaName.clear();
            m_sendMediaStarted = false;
        }
        stopHold();
        setSetting("fppMode", "remote", false);
        m_forcePlayerMode = false;
        scheduler->ReloadScheduleFile();
        logEvent("Back to Remote mode: mirroring the primary again");
    }

    // Main-loop half of the sync sender: stop packets, and media sync.
    void syncSenderTick(long long now) {
        if (!m_syncSendActive) return;
        std::lock_guard<std::mutex> l(m_sendLock);
        if (!m_sendSeqName.empty() && !sequence->IsSequenceRunning(m_sendSeqName)) {
            if (!sequence->IsSequenceRunning()) {
                multiSync->SendSeqSyncStopPacket(m_sendSeqName);
            }
            m_sendSeqName.clear();
            m_candidateName.clear();
            m_candidateCount = 0;
        }

        std::string mname;
        bool playing = false;
        float secs = 0;
        {
            std::unique_lock<std::mutex> ml(mediaOutputLock);
            if (mediaOutput) {
                mname = mediaOutput->m_mediaFilename;
                playing = (mediaOutputStatus.status == MEDIAOUTPUTSTATUS_PLAYING);
                secs = mediaOutputStatus.mediaSeconds;
            }
        }
        if (mname != m_sendMediaName) {
            if (!m_sendMediaName.empty()) multiSync->SendMediaSyncStopPacket(m_sendMediaName);
            m_sendMediaName = mname;
            m_sendMediaStarted = false;
            if (!mname.empty()) multiSync->SendMediaOpenPacket(mname);
        }
        if (!mname.empty() && playing) {
            if (!m_sendMediaStarted) {
                multiSync->SendMediaSyncStartPacket(mname);
                m_sendMediaStarted = true;
                m_lastMediaSyncMS = now;
            } else if ((now - m_lastMediaSyncMS) >= 200) {
                m_lastMediaSyncMS = now;
                multiSync->SendMediaSyncPacket(mname, secs);
            }
        }
    }

    // ------------------------------------------------------------------
    // Player snapshot helpers
    // ------------------------------------------------------------------
    Json::Value playerSummary() {
        Json::Value ps;
        Player::INSTANCE.GetCurrentStatus(ps);
        Json::Value out;
        PlaylistStatus st = Player::INSTANCE.GetStatus();
        out["status"] = (int)st;
        out["status_name"] = st == FPP_STATUS_IDLE ? "idle" : st == FPP_STATUS_PLAYLIST_PLAYING ? "playing"
                                                            : st == FPP_STATUS_PLAYLIST_PAUSED  ? "paused"
                                                                                                : "stopping";
        out["playlist"] = ps.isMember("current_playlist") ? ps["current_playlist"]["playlist"].asString() : "";
        out["index"] = ps.isMember("current_playlist") ? jsonInt(ps["current_playlist"], "index") : 0;
        out["count"] = ps.isMember("current_playlist") ? jsonInt(ps["current_playlist"], "count") : 0;
        out["sequence"] = ps.get("current_sequence", "").asString();
        out["seconds_played"] = jsonDouble(ps, "seconds_played");
        out["seconds_remaining"] = jsonDouble(ps, "seconds_remaining");
        out["repeat"] = Player::INSTANCE.GetRepeat();
        out["scheduled"] = Player::INSTANCE.WasScheduled() ? 1 : 0;
        out["holding"] = isHoldPlaying();
        // What the sequence engine itself is doing - on a hot backup this is
        // the mirrored show even though the playlist is idle.
        out["running_sequence"] = sequence->IsSequenceRunning() ? sequence->GetSeqFilenameCopy() : "";
        out["sequence_remaining_ms"] = sequence->IsSequenceRunning() ? sequence->m_seqMSRemaining : 0;
        return out;
    }

    // ------------------------------------------------------------------
    // Activation / yielding
    // ------------------------------------------------------------------
    // Item to start at (0-based) when taking over from the peer, or -1 for
    // "wherever the scheduler starts".
    int computeResumeIndex(long long now) {
        if (m_resumeMode == "playlist") return -1;
        if (!m_peer.valid || m_peer.playlist.empty() || m_peer.count <= 0) return -1;
        if (m_peer.playlist == HOLD_PLAYLIST) return -1;
        int idx0 = std::max(0, m_peer.index - 1);
        double downtime = (now - m_peer.observedMS) / 1000.0;
        int target = idx0;
        if (m_resumeMode == "next") {
            target = idx0 + 1;
        } else if (downtime >= m_peer.secondsRemaining) {
            target = idx0 + 1;
        }
        if (target >= m_peer.count) target = 0;
        return target;
    }

    void jumpToItem(int idx0) {
        std::string name = Player::INSTANCE.GetPlaylistName();
        if (name.empty() || idx0 < 0) return;
        if (Player::INSTANCE.GetPosition() - 1 == idx0) return;
        Player::INSTANCE.StartScheduledPlaylist(name, idx0,
                                                Player::INSTANCE.GetRepeat(),
                                                Player::INSTANCE.GetScheduleEntry(),
                                                Player::INSTANCE.GetPriority(),
                                                Player::INSTANCE.GetOrigStartTime(),
                                                Player::INSTANCE.GetOrigStopTime(),
                                                Player::INSTANCE.GetStopMethod());
        logEvent("Resumed playlist '" + name + "' at item " + std::to_string(idx0 + 1));
    }

    // Become the active player.  forceIndex (0-based) overrides the resume
    // calculation; -2 means "compute from the peer's last known position".
    //
    // Cold path (Player mode): release the hold and start the schedule now.
    // Hot path (Remote mode): become the sync source at once, keep whatever
    // is running, and start the schedule at the next item when it ends.
    void activate(const std::string& reason, int forceIndex = -2) {
        long long now = GetTimeMS();
        m_epoch++;
        m_manualStandby = false;
        m_handoffActivateAtMS = 0;

        if (hotCapable() && getFPPmode() == REMOTE_MODE) {
            enterPlayerMode();
            m_takeoverReason = reason;
            m_takeoverIndex = forceIndex;
            m_takeoverStartedMS = now;
            m_state = NodeState::Takeover;
            saveState();
            std::string running = sequence->IsSequenceRunning() ? sequence->GetSeqFilenameCopy() : "";
            m_takeoverSeq = running;
            if (!running.empty()) {
                logEvent("TAKING OVER (" + reason + "): keeping '" + running + "' running, epoch " +
                         std::to_string(m_epoch));
                // The remotes will finish this item on our sync; the next
                // one comes from our own playlist, at index (peer index).
                if (m_takeoverIndex == -2 && m_peer.valid && !m_peer.playlist.empty() && m_peer.count > 0) {
                    m_takeoverIndex = m_peer.index; // 1-based current == 0-based next
                    if (m_takeoverIndex >= m_peer.count) m_takeoverIndex = 0;
                }
            } else {
                logEvent("TAKING OVER (" + reason + "): nothing running, starting the schedule");
            }
            return;
        }
        completeActivation(reason, forceIndex);
    }

    // Release the hold, let the scheduler start the show, jump to the
    // right item.  All in one go, with our own announcements suppressed so a
    // first item the playlist opens and immediately replaces never reaches
    // the remotes.
    void completeActivation(const std::string& reason, int forceIndex) {
        long long now = GetTimeMS();
        m_suppressAnnounce = true;
        stopHold();
        Player::INSTANCE.ClearForceStopped();
        scheduler->CheckIfShouldBePlayingNow(1);
        if (Player::INSTANCE.GetStatus() == FPP_STATUS_PLAYLIST_PLAYING && Player::INSTANCE.WasScheduled()) {
            int target = forceIndex >= 0 ? forceIndex : (forceIndex == -2 ? computeResumeIndex(now) : -1);
            if (target >= 0) {
                int count = playerSummary()["count"].asInt();
                if (target < count) jumpToItem(target);
            }
        } else {
            logEvent("Nothing scheduled right now; scheduler will start the show at its next scheduled time");
        }
        m_suppressAnnounce = false;
        m_state = NodeState::Active;
        m_activatedMS = now;
        saveState();
        logEvent("ACTIVE (" + reason + "), epoch " + std::to_string(m_epoch));
    }

    // Hot takeover in progress: finish the mirrored item, then hand the
    // playlist engine the next one.
    void takeoverTick(long long now) {
        bool seqRunning = sequence->IsSequenceRunning();
        bool mediaPlaying = false;
        {
            std::unique_lock<std::mutex> ml(mediaOutputLock);
            mediaPlaying = mediaOutput && mediaOutputStatus.status == MEDIAOUTPUTSTATUS_PLAYING;
        }
        bool ready = false;
        if (seqRunning) {
            if (m_shadowLastFrameMS >= m_takeoverStartedMS) {
                long long rem = m_shadowRemainingMS - (now - m_shadowLastFrameMS);
                ready = rem <= m_takeoverLeadMS;
            } else {
                // No frames have reached the sender yet; do not wait forever.
                ready = (now - m_takeoverStartedMS) > 5000;
            }
        } else if (mediaPlaying) {
            ready = (now - m_takeoverStartedMS) > 30 * 60 * 1000;
        } else {
            ready = true;
        }
        if (!ready) return;

        // The schedule was reloaded under Player mode when the takeover
        // began; give ScheduleProc (once a second) time to pick it up.
        int repeat = 0;
        std::string scheduled = scheduler->GetPlaylistThatShouldBePlaying(repeat);
        if (scheduled.empty() && (now - m_schedReloadRequestedMS) < 2500) {
            return;
        }
        int idx = m_takeoverIndex;
        if (!scheduled.empty() && !m_takeoverSeq.empty()) {
            // The item we just finished is authoritative for where to go next.
            int found = indexOfSequenceInPlaylist(scheduled, m_takeoverSeq);
            if (found >= 0) idx = found + 1;
        }
        completeActivation(m_takeoverReason, idx);
    }

    // 0-based position of the first item in the playlist that plays the given
    // sequence, counting lead-in, main and lead-out the way FPP does; -1 if
    // not found.
    int indexOfSequenceInPlaylist(const std::string& playlist, const std::string& seq) {
        Json::Value pl;
        std::string file = FPP_DIR_PLAYLIST("/" + playlist + ".json");
        if (!FileExists(file) || !LoadJsonFromFile(file, pl) || !pl.isObject()) return -1;
        int pos = 0;
        for (const char* section : { "leadIn", "mainPlaylist", "leadOut" }) {
            if (!pl.isMember(section) || !pl[section].isArray()) continue;
            for (const auto& e : pl[section]) {
                if (e.get("sequenceName", "").asString() == seq) return pos;
                pos++;
            }
        }
        return -1;
    }

    // Stop running the show immediately and stand by.
    void yieldNow(const std::string& reason) {
        if (isShowPlaying()) {
            Player::INSTANCE.StopNow(1);
        }
        m_handoffActivateAtMS = 0;
        m_state = NodeState::Hold;
        if (m_forcePlayerMode) {
            leavePlayerMode();
        } else {
            startHold();
        }
        saveState();
        logEvent("YIELDED (" + reason + ")");
    }

    // The plan a peer needs to pick the show up at our next item.  Read-only,
    // so it is safe to build from an HTTP thread.
    Json::Value handoffPlan() {
        Json::Value plan;
        plan["accepted"] = true;
        plan["playlist"] = "";
        plan["nextIndex"] = -1;
        plan["endsInMs"] = 0;
        if (isShowPlaying() && Player::INSTANCE.GetStatus() != FPP_STATUS_IDLE) {
            Json::Value ps = playerSummary();
            plan["playlist"] = ps["playlist"];
            int idx1 = ps["index"].asInt();
            int count = ps["count"].asInt();
            int next0 = idx1; // 1-based current == 0-based next
            if (count > 0 && next0 >= count) next0 = 0;
            plan["nextIndex"] = next0;
            plan["endsInMs"] = (int)(ps["seconds_remaining"].asDouble() * 1000.0);
        }
        return plan;
    }

    // Finish the current item, then stand by.  Runs on the main loop.
    void beginHandoffOut(const std::string& reason) {
        if (isShowPlaying() && Player::INSTANCE.GetStatus() == FPP_STATUS_PLAYLIST_PLAYING) {
            Json::Value plan = handoffPlan();
            Player::INSTANCE.StopGracefully(1);
            m_state = NodeState::HandoffOut;
            m_handoffStartedMS = GetTimeMS();
            logEvent("HANDOFF OUT (" + reason + "): finishing item " +
                     std::to_string(plan["nextIndex"].asInt()) + " of '" + plan["playlist"].asString() +
                     "', " + std::to_string(plan["endsInMs"].asInt()) + " ms left");
            saveState();
        } else {
            yieldNow(reason);
        }
    }

    void scheduleHandoffIn(const Json::Value& plan, const std::string& reason) {
        long long now = GetTimeMS();
        int endsIn = plan.get("endsInMs", 0).asInt();
        m_handoffNextIndex = plan.get("nextIndex", -1).asInt();
        m_handoffPlaylist = plan.get("playlist", "").asString();
        m_handoffActivateAtMS = now + endsIn + m_handoffMarginMS;
        m_state = NodeState::HandoffIn;
        saveState();
        logEvent("HANDOFF IN (" + reason + "): activating in " + std::to_string(endsIn + m_handoffMarginMS) +
                 " ms at item " + std::to_string(m_handoffNextIndex + 1));
    }

    // ------------------------------------------------------------------
    // Peer communication
    // ------------------------------------------------------------------
    std::string peerUrl(const std::string& path) {
        // Query form, not "/failover/<action>": FPP 10.0 does not dispatch the
        // family subpath route, so the subpath form 404s and peer discovery
        // (and therefore failover itself) would never work.  See handleRequest.
        return "http://" + m_peerAddress + "/api/plugin-apis/failover?action=" + path;
    }

    void pollPeer(long long now) {
        if (m_peerAddress.empty()) return;
        if (m_pollInFlight && (now - m_lastPollSentMS) < 12000) return;
        m_pollInFlight = true;
        m_lastPollSentMS = now;
        CurlManager::INSTANCE.addGet(peerUrl("status"), [this](int rc, const std::string& resp) {
            std::unique_lock<std::recursive_mutex> lock(m_lock);
            m_pollInFlight = false;
            long long t = GetTimeMS();
            Json::Value v;
            if (rc == 200 && LoadJsonFromString(resp, v) && v.isObject() &&
                v["plugin"].asString() == PLUGIN_NAME) {
                m_peer.valid = true;
                m_peer.lastContactMS = t;
                m_peer.lastOkMS = t;
                m_peer.okCount++;
                m_peer.failCount = 0;
                m_peer.role = v["role"].asString();
                m_peer.state = v["state"].asString();
                m_peer.host = v["host"].asString();
                m_peer.epoch = v["epoch"].asInt();
                m_peer.manualStandby = v["manualStandby"].asBool();
                if (v.isMember("player")) {
                    const Json::Value& p = v["player"];
                    std::string pl = p["playlist"].asString();
                    if (!pl.empty() && pl != HOLD_PLAYLIST && p["status_name"].asString() == "playing") {
                        m_peer.playlist = pl;
                        m_peer.index = p["index"].asInt();
                        m_peer.count = p["count"].asInt();
                        m_peer.secondsPlayed = p["seconds_played"].asDouble();
                        m_peer.secondsRemaining = p["seconds_remaining"].asDouble();
                        m_peer.observedMS = t;
                    }
                }
                if (m_peerWarned) {
                    m_peerWarned = false;
                    clearWarning(WARN_ID_PEER);
                    logEvent("Peer " + m_peerAddress + " is reachable again");
                }
            } else {
                m_peer.failCount++;
                m_peer.okCount = 0;
                LogDebug(VB_PLUGIN, "fpp-failover: peer poll failed rc=%d\n", rc);
            }
        }, PLUGIN_NAME);
    }

    long long deadAfterMS() {
        return (long long)m_failureThreshold * m_pollIntervalMS + m_pollIntervalMS / 2;
    }

    bool peerAlive(long long now) {
        return (now - m_peer.lastContactMS) < deadAfterMS();
    }

    bool peerFresh(long long now) {
        return m_peer.valid && (now - m_peer.lastOkMS) < (2LL * m_pollIntervalMS + 500);
    }

    void postToPeer(const std::string& path, const Json::Value& body,
                    std::function<void(int, const std::string&)> cb) {
        if (m_peerAddress.empty()) return;
        std::string data = SaveJsonToString(body);
        CurlManager::INSTANCE.addPost(peerUrl(path), data, "application/json",
                                      [this, cb](int rc, const std::string& resp) {
                                          std::unique_lock<std::recursive_mutex> lock(m_lock);
                                          if (cb) cb(rc, resp);
                                      },
                                      PLUGIN_NAME);
    }

    // Ask the active peer to finish its item and hand the show to us.
    void requestHandoff(const std::string& reason) {
        long long now = GetTimeMS();
        if (m_handoffRequestedMS && (now - m_handoffRequestedMS) < 10000) return;
        m_handoffRequestedMS = now;
        Json::Value body;
        body["from"] = m_hostName;
        body["epoch"] = m_epoch;
        logEvent("Requesting handoff from peer (" + reason + ")");
        postToPeer("requestHandoff", body, [this, reason](int rc, const std::string& resp) {
            Json::Value v;
            if (rc == 200 && LoadJsonFromString(resp, v) && v.isObject()) {
                if (v.get("accepted", false).asBool()) {
                    if (m_state == NodeState::Hold || m_state == NodeState::HandoffIn) {
                        scheduleHandoffIn(v, "peer accepted");
                    }
                } else {
                    // Keep the 10 s backoff: a peer mid-takeover will accept later.
                    logEvent("Peer declined handoff (state " + v.get("state", "?").asString() + ")");
                }
            } else {
                logEvent("Handoff request failed (rc " + std::to_string(rc) + ")");
            }
        });
    }

    bool canSendPackets() {
        return multiSync->isMultiSyncEnabled() || m_syncSendActive;
    }

    void sendHeartbeat(long long now) {
        if (!canSendPackets()) return;
        if ((now - m_lastHeartbeatSentMS) < m_pollIntervalMS) return;
        m_lastHeartbeatSentMS = now;
        Json::Value hb;
        hb["plugin"] = PLUGIN_NAME;
        hb["v"] = 1;
        hb["role"] = roleName(m_role);
        hb["state"] = stateName(m_state);
        hb["epoch"] = m_epoch;
        hb["host"] = m_hostName;
        hb["manualStandby"] = m_manualStandby;
        Json::Value addrs(Json::arrayValue);
        for (auto& a : m_localAddresses) addrs.append(a);
        hb["addresses"] = addrs;
        std::string s = SaveJsonToString(hb);
        multiSync->SendPluginData(PLUGIN_NAME, (const uint8_t*)s.c_str(), (int)s.size() + 1);
    }

    // ------------------------------------------------------------------
    // Witnesses: remotes that can vouch the primary is still alive
    // ------------------------------------------------------------------
    std::vector<std::string> witnessList() {
        std::vector<std::string> out;
        if (!m_witnessAddresses.empty()) {
            for (auto& w : split(m_witnessAddresses, ',')) {
                std::string t = w;
                while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
                while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
                if (!t.empty()) out.push_back(t);
            }
            return out;
        }
        // Every full FPP instance on the network except us and the peer.  Only a
        // node running this plugin answers with a "heard" map, so anything
        // else just fails one request harmlessly; the filter exists to avoid
        // polling things that are not FPP at all.
        //
        // The test is the hardware type: below kSysTypeFalconController (0x80)
        // is a full FPP instance, at or above it is an external controller or
        // light (Falcon, WLED, ESPixelStick, and from FPP 10.2 Twinkly).  This
        // is the same boundary FPP itself uses to decide what counts as an FPP
        // box.  Two tempting alternatives are both wrong:
        //  - fppMode: MultiSync caches it from discovery and the cache goes
        //    stale for a long time after a node changes mode, so a real remote
        //    can still read as "player" and would be dropped.
        //  - multiSyncCapable: only becomes true for a system heard via the UDP
        //    MultiSync ping.  Anything found by HTTP discovery - including FPP
        //    remotes - is recorded with it false.  HTTP discovery is exactly
        //    what FPP falls back to when multicast is blocked, so filtering on
        //    it would leave no witnesses on precisely the networks that most
        //    need them.
        // Type 0 (unknown) is excluded: it could be any device that answered.
        constexpr int kFirstExternalControllerType = 0x80; // kSysTypeFalconController
        Json::Value sys = multiSync->GetSystems(false, false);
        if (sys.isMember("systems")) {
            for (const auto& s : sys["systems"]) {
                if (s.get("local", 0).asInt()) continue;
                int type = s.get("typeId", 0).asInt();
                if (type <= 0 || type >= kFirstExternalControllerType) continue;
                std::string a = s["address"].asString();
                if (!a.empty() && a != m_peerAddress) out.push_back(a);
            }
        }
        return out;
    }

    void pollWitnesses(long long now) {
        if (!m_witnessCheck) return;
        if ((now - m_lastWitnessPollMS) < 2LL * m_pollIntervalMS) return;
        if (m_witnessInFlight > 0) return;
        m_lastWitnessPollMS = now;
        for (auto& w : witnessList()) {
            m_witnessInFlight++;
            std::string url = "http://" + w + "/api/plugin-apis/failover?action=status";
            CurlManager::INSTANCE.addGet(url, [this, w](int rc, const std::string& resp) {
                std::unique_lock<std::recursive_mutex> lock(m_lock);
                m_witnessInFlight = std::max(0, m_witnessInFlight - 1);
                Json::Value v;
                if (rc != 200 || !LoadJsonFromString(resp, v) || !v.isObject() || !v.isMember("heard")) {
                    return;
                }
                long long t = GetTimeMS();
                const Json::Value& heard = v["heard"];
                long long best = -1;
                for (const auto& key : heard.getMemberNames()) {
                    if (key == m_peerAddress || (!m_peer.host.empty() && key == m_peer.host)) {
                        long long ago = heard[key].asInt64();
                        if (best < 0 || ago < best) best = ago;
                    }
                }
                if (best >= 0 && best < (long long)m_witnessGraceSec * 1000) {
                    m_witnessAliveMS = t - best;
                    m_witnessVouchedBy = w;
                }
            }, PLUGIN_NAME);
        }
    }

    bool witnessAllowsTakeover(long long now) {
        if (!m_witnessCheck) return true;
        return (now - m_witnessAliveMS) >= (long long)m_witnessGraceSec * 1000;
    }

    // ------------------------------------------------------------------
    // Remote-side helpers (also used by a hot backup, which is a remote)
    // ------------------------------------------------------------------
    void noteSync() {
        m_lastSyncPacketMS = GetTimeMS();
        m_syncSeen = true;
    }

    void refreshHeardFromSyncStats(long long now) {
        if (!m_syncSeen.exchange(false)) return;
        Json::Value stats = multiSync->GetSyncStats();
        std::string master = stats.get("masterIP", "").asString();
        if (!master.empty()) {
            m_heard[master] = m_lastSyncPacketMS;
            m_syncMasterIP = master;
            if (!m_peerAddress.empty() && master == m_peerAddress) {
                m_peer.lastContactMS = std::max(m_peer.lastContactMS, (long long)m_lastSyncPacketMS);
            }
        }
    }

    void startFallback() {
        if (m_remoteFallbackSequence.empty()) return;
        std::string f = m_remoteFallbackSequence;
        if (!endsWith(f, ".fseq")) f += ".fseq";
        if (!FileExists(FPP_DIR_SEQUENCE("/" + f))) {
            if (!m_fallbackMissingWarned) {
                m_fallbackMissingWarned = true;
                logEvent("Fallback sequence '" + f + "' does not exist");
            }
            return;
        }
        sequence->StartSequence(f, 0);
        m_state = NodeState::RemoteFallback;
        m_fallbackStartedMS = GetTimeMS();
        if (!m_fallbackLogged) {
            m_fallbackLogged = true;
            logEvent("No player alive - running fallback sequence '" + f + "'");
        }
    }

    void stopFallback() {
        m_state = NodeState::RemoteIdle;
        m_fallbackLogged = false;
    }

    void remoteTick(long long now) {
        if (getFPPmode() != REMOTE_MODE) {
            setWarning(WARN_ID_MODE, "fpp-failover: role 'remote' requires FPP Remote mode");
            return;
        }
        clearWarning(WARN_ID_MODE);
        long long lastMaster = std::max((long long)m_lastSyncPacketMS, m_lastHeartbeatMS);
        bool masterQuiet = (now - lastMaster) > (long long)m_remoteFallbackDelaySec * 1000;

        if (m_state == NodeState::RemoteFallback) {
            if (!masterQuiet) {
                // A player is back.  Its sync packets replace our sequence on
                // their own; just stop treating this as a fallback.
                logEvent("Player is back, fallback ends");
                stopFallback();
            } else if (!sequence->IsSequenceRunning()) {
                startFallback(); // loop it
            }
            return;
        }
        if (!m_remoteFallbackSequence.empty() && masterQuiet && !sequence->IsSequenceRunning() &&
            (now - m_startMS) > (long long)m_remoteFallbackDelaySec * 1000) {
            startFallback();
        }
    }

    // ------------------------------------------------------------------
    // Warnings
    // ------------------------------------------------------------------
    void setWarning(int id, const std::string& msg) {
        if (m_warnings[id] == msg) return;
        if (!m_warnings[id].empty()) WarningHolder::RemoveWarning(id, m_warnings[id], PLUGIN_NAME);
        m_warnings[id] = msg;
        WarningHolder::AddWarningTimeout(3600 * 24 * 365, id, msg, {}, PLUGIN_NAME);
    }
    void clearWarning(int id) {
        auto it = m_warnings.find(id);
        if (it == m_warnings.end() || it->second.empty()) return;
        WarningHolder::RemoveWarning(id, it->second, PLUGIN_NAME);
        it->second.clear();
    }
    void clearWarnings() {
        for (auto& w : m_warnings) {
            if (!w.second.empty()) WarningHolder::RemoveWarning(w.first, w.second, PLUGIN_NAME);
            w.second.clear();
        }
    }

    void checkPlayerRoleWarnings() {
        if (m_role == Role::Primary) {
            if (getFPPmode() != PLAYER_MODE) {
                setWarning(WARN_ID_MODE, "fpp-failover: role 'primary' requires FPP Player mode");
            } else {
                clearWarning(WARN_ID_MODE);
            }
        } else if (m_role == Role::Backup) {
            if (!m_bootedRemote) {
                setWarning(WARN_ID_MODE, "fpp-failover: backup is in Player mode, so standby is cold (hold playlist). "
                                         "Set FPP mode to Remote and restart fppd for hot standby");
            } else {
                clearWarning(WARN_ID_MODE);
            }
        }
        if (getFPPmode() == PLAYER_MODE && !m_forcePlayerMode && !multiSync->isMultiSyncEnabled()) {
            setWarning(WARN_ID_MULTISYNC, "fpp-failover: MultiSync is disabled on this player; remotes will not follow it");
        } else {
            clearWarning(WARN_ID_MULTISYNC);
        }
        if (m_peerAddress.empty()) {
            setWarning(WARN_ID_PEER, "fpp-failover: no peer address configured");
        } else if (m_warnings[WARN_ID_PEER].find("no peer") != std::string::npos) {
            clearWarning(WARN_ID_PEER);
        }
    }

    // ------------------------------------------------------------------
    // The state machine, run every TICK_MS on fppd's main loop
    // ------------------------------------------------------------------
    void tick() {
        std::unique_lock<std::recursive_mutex> lock(m_lock);
        long long now = GetTimeMS();
        if (m_role == Role::Off) return;
        refreshHeardFromSyncStats(now);
        if (m_role == Role::Remote) {
            remoteTick(now);
            if (!m_peerAddress.empty() && (now - m_lastPollSentMS) >= 5LL * m_pollIntervalMS) pollPeer(now);
            contentSyncTick(now);
            return;
        }
        if ((now - m_lastAddrRefreshMS) > 60000) {
            m_lastAddrRefreshMS = now;
            refreshLocalAddresses();
        }
        if ((now - m_lastWarnCheckMS) > 1000) {
            m_lastWarnCheckMS = now;
            checkPlayerRoleWarnings();
        }
        if (m_role == Role::Primary && getFPPmode() != PLAYER_MODE) return;

        // A settings-file reload can put the on-disk mode back while we act
        // as the player in memory.  Re-assert it.
        if (m_forcePlayerMode && getFPPmode() != PLAYER_MODE) {
            setSetting("fppMode", "player", false);
            scheduler->ReloadScheduleFile();
            logEvent("Re-asserted in-memory Player mode after a settings reload");
        }

        sendHeartbeat(now);
        if ((now - m_lastPollSentMS) >= m_pollIntervalMS) pollPeer(now);
        if (m_role == Role::Backup && m_state == NodeState::Hold) pollWitnesses(now);

        syncSenderTick(now);
        contentSyncTick(now);
        processPendingAction(now);
        evaluate(now);
        maintain(now);
    }

    void evaluate(long long now) {
        bool alive = peerAlive(now);
        bool fresh = peerFresh(now);
        bool noPeerConfigured = m_peerAddress.empty();

        if (!alive && !m_peerWarned && !noPeerConfigured) {
            m_peerWarned = true;
            setWarning(WARN_ID_PEER, "fpp-failover: peer " + m_peerAddress + " is unreachable");
        }

        switch (m_state) {
        case NodeState::Hold:
            if (m_role == Role::Primary) {
                if (m_manualStandby) break;
                if (noPeerConfigured) {
                    activate("no peer configured");
                } else if (fresh) {
                    if (peerStateIsActive(m_peer.state)) {
                        if (m_failbackMode == "auto" && m_peer.okCount >= m_recoveryThreshold) {
                            requestHandoff("primary back, automatic fail-back");
                        }
                    } else if (m_peer.state == "handoff_in") {
                        // Both waiting on each other - primary wins.
                        activate("peer was waiting for a handoff");
                    } else {
                        activate("peer is standing by");
                    }
                } else if (!alive) {
                    activate("peer unreachable");
                }
            } else { // Backup
                if (noPeerConfigured) break;
                if (fresh && m_peer.state == "hold" && m_peer.manualStandby) {
                    activate("primary is in manual standby");
                } else if (!alive) {
                    if (witnessAllowsTakeover(now)) {
                        activate("primary unreachable");
                    } else if (!m_witnessBlockedLogged) {
                        m_witnessBlockedLogged = true;
                        logEvent("Primary unreachable from here, but witness " + m_witnessVouchedBy +
                                 " still hears it - not taking over");
                    }
                } else if (hotCapable() && m_hotDetectMS > 0 && sequence->IsSequenceRunning() &&
                           m_lastSyncPacketMS > 0 &&
                           (now - m_lastSyncPacketMS) > m_hotDetectMS &&
                           (now - m_peer.lastOkMS) > m_hotDetectMS &&
                           witnessAllowsTakeover(now)) {
                    // Mid-sequence the primary syncs several times a second.
                    // Silence that long, with HTTP quiet too, is a dead
                    // player; every second we wait is a second the remotes
                    // free-run toward the end of the item.
                    activate("primary sync stopped mid-sequence");
                } else {
                    m_witnessBlockedLogged = false;
                }
            }
            break;

        case NodeState::Takeover:
            // Only a poll that SUCCEEDED AFTER the takeover began says anything
            // about the peer: we took over precisely because it went silent, and
            // its last good poll is normally a second or two old at that moment.
            // Trusting that stale "active" made the backup take over, yield, and
            // take over again inside one second, churning the sync source under
            // the remotes mid-song.
            if (m_peer.lastOkMS > m_takeoverStartedMS && peerStateIsActive(m_peer.state) &&
                m_role == Role::Backup) {
                yieldNow("primary is active again");
            } else {
                takeoverTick(now);
            }
            break;

        case NodeState::HandoffIn:
            if (m_handoffActivateAtMS && now >= m_handoffActivateAtMS) {
                activate("handoff complete", m_handoffNextIndex);
            } else if (!alive) {
                activate("peer died during handoff");
            } else if (!m_handoffActivateAtMS && (now - m_handoffRequestedMS) > 15000) {
                m_state = NodeState::Hold; // request went nowhere, retry from Hold
                m_handoffRequestedMS = 0;
            }
            break;

        case NodeState::Active:
            // A peer reading taken before we activated still says "active".
            // Acting on it right after a handoff makes the two nodes bounce
            // the show back and forth, so ignore the peer until a poll that
            // started after we became active has had time to land.
            if (fresh && m_bootAssumedActive && peerStateIsActive(m_peer.state)) {
                // We only assumed active because fppd's scheduler had already
                // started something at boot.  The peer was really running the
                // show, so stand by; the normal Hold logic then asks them for a
                // graceful handoff at the next item boundary instead of
                // yanking the show back to our item 1.
                m_bootAssumedActive = false;
                yieldNow("peer was already running the show when we started");
                break;
            }
            m_bootAssumedActive = false;
            if (fresh && peerStateIsActive(m_peer.state) &&
                m_peer.lastOkMS > m_activatedMS &&
                (now - m_activatedMS) > (2LL * m_pollIntervalMS)) {
                if (m_role == Role::Backup) {
                    yieldNow("primary is active");
                } else if ((now - m_lastNudgeMS) > 5000) {
                    // Split brain: the backup should yield on its own once it
                    // sees us, but nudge it in case its polling is broken.
                    m_lastNudgeMS = now;
                    Json::Value body;
                    body["from"] = m_hostName;
                    body["reason"] = "primary is active";
                    postToPeer("yield", body, nullptr);
                    logEvent("Both players active - asked backup to yield");
                }
            }
            break;

        case NodeState::HandoffOut:
            if (Player::INSTANCE.GetStatus() == FPP_STATUS_IDLE) {
                m_state = NodeState::Hold;
                if (m_forcePlayerMode) {
                    leavePlayerMode();
                } else {
                    startHold();
                }
                saveState();
                logEvent("Handoff complete, now standing by");
            } else if ((now - m_handoffStartedMS) > 30 * 60 * 1000) {
                yieldNow("handoff took too long");
            }
            break;

        default:
            break;
        }
    }

    // Keep the hold playlist running while holding (cold), and notice if
    // something else is running the show on this node.
    void maintain(long long now) {
        if (getFPPmode() != PLAYER_MODE) return;
        if (m_state == NodeState::Hold || m_state == NodeState::HandoffIn) {
            if (Player::INSTANCE.GetStatus() == FPP_STATUS_IDLE) {
                if ((now - m_holdStartedMS) > 2000) startHold();
            } else if (isShowPlaying() && Player::INSTANCE.WasScheduled()) {
                // The scheduler got past us (hold failed to start in time).
                if (m_role == Role::Primary && !m_manualStandby) {
                    m_state = NodeState::Active;
                    m_activatedMS = now;
                    m_epoch++;
                    saveState();
                    logEvent("Scheduled show started while holding - assuming active");
                } else {
                    yieldNow("scheduled show started while holding");
                }
            }
        } else if (m_state == NodeState::Active) {
            if (isHoldPlaying()) {
                // Someone started the hold playlist by hand: honour it.
                m_state = NodeState::Hold;
                saveState();
                logEvent("Hold playlist started manually - standing by");
            }
        }
    }

    // Actions requested over HTTP or via FPP commands run on the main loop.
    void processPendingAction(long long now) {
        if (m_pendingAction.empty()) return;
        std::string a = m_pendingAction;
        Json::Value arg = m_pendingArg;
        m_pendingAction.clear();
        m_pendingArg = Json::Value();

        if (a == "handoffOut") {
            if (m_state == NodeState::Active) {
                beginHandoffOut(arg.get("reason", "peer requested it").asString());
            }
        } else if (a == "yield") {
            // Manual standby only ever pins the primary; a backup that is
            // told to yield must still be free to take over again later.
            m_manualStandby = arg.get("manual", false).asBool() && m_role == Role::Primary;
            if (m_state == NodeState::Active || m_state == NodeState::HandoffOut ||
                m_state == NodeState::Takeover) {
                yieldNow(arg.get("reason", "requested").asString());
            } else {
                m_state = NodeState::Hold;
                startHold();
                saveState();
            }
        } else if (a == "takeover") {
            bool graceful = arg.get("graceful", true).asBool();
            if (m_state == NodeState::Active || m_state == NodeState::Takeover) return;
            if (graceful && peerFresh(now) && peerStateIsActive(m_peer.state)) {
                m_manualStandby = false;
                m_handoffRequestedMS = 0;
                Json::Value body;
                body["from"] = m_hostName;
                body["epoch"] = m_epoch;
                body["manual"] = true;
                logEvent("Manual takeover: requesting graceful handoff");
                postToPeer("requestHandoff", body, [this](int rc, const std::string& resp) {
                    Json::Value v;
                    if (rc == 200 && LoadJsonFromString(resp, v) && v.get("accepted", false).asBool()) {
                        scheduleHandoffIn(v, "manual takeover");
                    } else {
                        activate("manual takeover (peer did not accept handoff)");
                    }
                });
            } else {
                Json::Value body;
                body["from"] = m_hostName;
                body["manual"] = true;
                body["reason"] = "manual takeover";
                postToPeer("yield", body, nullptr);
                activate("manual takeover");
            }
        } else if (a == "failback") {
            m_manualStandby = false;
            saveState();
            if (m_role == Role::Primary) {
                if (m_state == NodeState::Hold) {
                    m_handoffRequestedMS = 0;
                    if (peerFresh(now) && peerStateIsActive(m_peer.state)) {
                        requestHandoff("manual fail-back");
                    }
                }
            } else if (m_role == Role::Backup && m_state == NodeState::Active) {
                // Tell the primary to come back; it will request a handoff.
                Json::Value body;
                body["from"] = m_hostName;
                postToPeer("failback", body, nullptr);
                logEvent("Asked primary to take the show back");
            }
        }
    }

    // ------------------------------------------------------------------
    // HTTP API:  /api/plugin-apis/failover?action=<action>
    // ------------------------------------------------------------------
    Json::Value statusJson() {
        long long now = GetTimeMS();
        Json::Value r;
        r["plugin"] = PLUGIN_NAME;
        r["version"] = PLUGIN_VERSION;
        r["role"] = roleName(m_role);
        r["state"] = stateName(m_state);
        r["epoch"] = m_epoch;
        r["manualStandby"] = m_manualStandby;
        r["fppMode"] = getFPPmodeStr();
        r["multisync"] = multiSync->isMultiSyncEnabled();
        r["host"] = m_hostName;
        r["uptimeMs"] = (Json::Int64)(now - m_startMS);
        Json::Value addrs(Json::arrayValue);
        for (auto& a : m_localAddresses) addrs.append(a);
        r["addresses"] = addrs;

        Json::Value hot;
        hot["capable"] = hotCapable();
        hot["standby"] = hotCapable() && !m_forcePlayerMode;
        hot["actingPlayer"] = m_forcePlayerMode;
        hot["syncSender"] = (bool)m_syncSendActive;
        hot["lastSyncMsAgo"] = m_lastSyncPacketMS ? (Json::Int64)(now - m_lastSyncPacketMS) : -1;
        hot["syncMasterIP"] = m_syncMasterIP;
        {
            std::lock_guard<std::mutex> l(m_sendLock);
            hot["announcedSequence"] = m_sendSeqName;
            hot["announcedMedia"] = m_sendMediaName;
        }
        r["hot"] = hot;

        Json::Value peer;
        peer["address"] = m_peerAddress;
        peer["alive"] = !m_peerAddress.empty() && peerAlive(now);
        peer["valid"] = m_peer.valid;
        peer["lastContactMsAgo"] = (Json::Int64)(now - m_peer.lastContactMS);
        peer["lastOkMsAgo"] = m_peer.lastOkMS ? (Json::Int64)(now - m_peer.lastOkMS) : -1;
        peer["state"] = m_peer.state;
        peer["role"] = m_peer.role;
        peer["host"] = m_peer.host;
        peer["epoch"] = m_peer.epoch;
        peer["manualStandby"] = m_peer.manualStandby;
        peer["okCount"] = m_peer.okCount;
        peer["failCount"] = m_peer.failCount;
        peer["playlist"] = m_peer.playlist;
        peer["index"] = m_peer.index;
        peer["count"] = m_peer.count;
        peer["secondsRemaining"] = m_peer.secondsRemaining;
        peer["observedMsAgo"] = m_peer.observedMS ? (Json::Int64)(now - m_peer.observedMS) : -1;
        r["peer"] = peer;

        Json::Value wit;
        wit["enabled"] = m_witnessCheck;
        wit["lastVouchMsAgo"] = m_witnessAliveMS ? (Json::Int64)(now - m_witnessAliveMS) : -1;
        wit["vouchedBy"] = m_witnessVouchedBy;
        Json::Value wl(Json::arrayValue);
        for (auto& w : witnessList()) wl.append(w);
        wit["addresses"] = wl;
        r["witness"] = wit;

        Json::Value handoff;
        handoff["activateInMs"] = m_handoffActivateAtMS ? (Json::Int64)(m_handoffActivateAtMS - now) : -1;
        handoff["nextIndex"] = m_handoffNextIndex;
        handoff["playlist"] = m_handoffPlaylist;
        r["handoff"] = handoff;

        r["player"] = playerSummary();

        Json::Value cs;
        cs["enabled"] = m_contentSync;
        cs["mode"] = m_contentSyncMode;
        cs["soakMin"] = m_contentSyncSoakMin;
        cs["snapshotsKept"] = m_contentSnapshots;
        cs["available"] = contentSyncRoleOK() && !m_peerAddress.empty();
        cs["running"] = m_syncPid > 0;
        cs["runningMode"] = m_syncMode;
        cs["runningMs"] = m_syncPid > 0 ? (Json::Int64)(now - m_syncStartedMS) : 0;
        cs["lastRunMsAgo"] = m_lastSyncMS ? (Json::Int64)(now - m_lastSyncMS) : -1;
        cs["lastOK"] = m_lastSyncOK;
        cs["lastCheckMsAgo"] = m_lastCheckMS ? (Json::Int64)(now - m_lastCheckMS) : -1;
        cs["nextRunInMs"] = (m_contentSync && m_nextSyncMS) ? (Json::Int64)(m_nextSyncMS - now) : -1;
        cs["intervalMin"] = m_contentSyncIntervalMin;
        cs["last"] = m_lastSyncStatus;
        cs["pending"] = m_pending;
        cs["pendingSinceMsAgo"] = m_pendingSinceMS ? (Json::Int64)(now - m_pendingSinceMS) : -1;
        cs["snapshots"] = snapshotList();
        r["contentSync"] = cs;

        Json::Value heard;
        for (auto& h : m_heard) heard[h.first] = (Json::Int64)(now - h.second);
        r["heard"] = heard;

        Json::Value rem;
        rem["lastSyncMsAgo"] = m_lastSyncPacketMS ? (Json::Int64)(now - m_lastSyncPacketMS) : -1;
        rem["lastHeartbeatMsAgo"] = m_lastHeartbeatMS ? (Json::Int64)(now - m_lastHeartbeatMS) : -1;
        rem["syncMasterIP"] = m_syncMasterIP;
        rem["activeMaster"] = m_activeMaster;
        rem["activeMasterMsAgo"] = m_lastActiveHeartbeatMS ? (Json::Int64)(now - m_lastActiveHeartbeatMS) : -1;
        rem["fallbackSequence"] = m_remoteFallbackSequence;
        r["remote"] = rem;

        Json::Value ev(Json::arrayValue);
        {
            std::unique_lock<std::mutex> l(m_eventsLock);
            for (auto& e : m_events) ev.append(e);
        }
        r["events"] = ev;
        return r;
    }

    void handleRequest(const HttpRequestPtr& req, HttpCallback&& callback) {
        Json::Value body;
        std::string b(req->getBody());
        if (!b.empty()) LoadJsonFromString(b, body);
        if (!body.isObject()) body = Json::Value(Json::objectValue);

        // The action can arrive three ways, in this order of preference:
        //   /failover/<action>        subpath
        //   /failover?action=<action> query parameter
        //   body {"action": "..."}    JSON body
        //
        // The subpath form depends on FPP registering the family route
        // (registerPluginApi(..., family=true)) with drogon.  On FPP 10.0
        // that registration does not match - fpp-brightness, the reference
        // plugin, has the same problem - so every subpath 404s and only the
        // base path is reachable.  The query form is what the UI and the
        // documented API use, because it works on every build.
        std::string path = req->path();
        std::string action;
        size_t p = path.find("/failover");
        if (p != std::string::npos) {
            action = path.substr(p + 9);
            while (!action.empty() && action.front() == '/') action.erase(action.begin());
            while (!action.empty() && action.back() == '/') action.pop_back();
        }
        if (action.empty()) action = req->getParameter("action");
        if (action.empty() && body.isMember("action")) action = body["action"].asString();

        std::unique_lock<std::recursive_mutex> lock(m_lock);
        Json::Value resp;
        int code = 200;

        if (action.empty() || action == "status") {
            resp = statusJson();
        } else if (req->getMethod() != drogon::Post) {
            code = 405;
            resp["status"] = "error";
            resp["message"] = "POST required";
        } else if (action == "syncContent" || action == "promoteContent" || action == "checkContent" ||
                   action == "rollbackContent") {
            if (!contentSyncRoleOK()) {
                code = 409;
                resp["status"] = "error";
                resp["message"] = "Content sync runs on a backup or remote, pulling from the primary";
            } else if (m_syncPid > 0) {
                code = 409;
                resp["status"] = "error";
                resp["message"] = "A content job (" + m_syncMode + ") is already running";
            } else {
                if (action == "checkContent") {
                    m_syncRequest = "check";
                } else if (action == "rollbackContent") {
                    m_syncRequest = "rollback";
                    m_syncRequestSnapshot = body.get("snapshot", "latest").asString();
                    m_syncRequestTarget = body.get("target", "both").asString();
                    if (m_syncRequestTarget != "local" && m_syncRequestTarget != "primary") m_syncRequestTarget = "both";
                } else {
                    m_syncRequest = "promote";
                }
                resp["status"] = "OK";
            }
        } else if (m_role != Role::Primary && m_role != Role::Backup) {
            code = 409;
            resp["status"] = "error";
            resp["message"] = "This node is not configured as a primary or backup player";
        } else if (action == "requestHandoff") {
            // Peer wants the show.  Answer with the plan now; the graceful
            // stop itself runs on the main loop at the next tick.
            if (m_state == NodeState::Active || m_state == NodeState::HandoffOut) {
                resp = handoffPlan();
                if (m_state == NodeState::Active) {
                    m_pendingAction = "handoffOut";
                    m_pendingArg = Json::Value(Json::objectValue);
                    m_pendingArg["reason"] = "peer " + body.get("from", "?").asString() + " requested it";
                }
                // A manual takeover from the peer pins a primary in standby
                // until someone asks for the show back.
                if (body.get("manual", false).asBool() && m_role == Role::Primary) {
                    m_manualStandby = true;
                    saveState();
                }
            } else {
                // Takeover in progress or standing by: nothing to hand off.
                resp["accepted"] = false;
                resp["state"] = stateName(m_state);
            }
        } else if (action == "yield") {
            m_pendingAction = "yield";
            m_pendingArg = body;
            if (!body.isMember("reason")) m_pendingArg["reason"] = "yield requested by " + body.get("from", "peer").asString();
            resp["status"] = "OK";
        } else if (action == "takeover") {
            m_pendingAction = "takeover";
            m_pendingArg = body;
            resp["status"] = "OK";
        } else if (action == "failback") {
            m_pendingAction = "failback";
            m_pendingArg = body;
            resp["status"] = "OK";
        } else if (action == "resume") {
            // Clear manual standby without forcing anything else.
            m_manualStandby = false;
            saveState();
            resp["status"] = "OK";
        } else {
            code = 404;
            resp["status"] = "error";
            resp["message"] = "Unknown action '" + action + "'";
        }
        callback(makeStringResponse(SaveJsonToString(resp), code, "application/json"));
    }

    // ------------------------------------------------------------------
    // FPP commands
    // ------------------------------------------------------------------
    class FailoverCommand : public Command {
    public:
        FailoverCommand(FPPFailoverPlugin* p, const std::string& name, const std::string& desc,
                        const std::string& action, bool manual) :
            Command(name, desc), plugin(p), m_action(action), m_manual(manual) {}
        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string>& args) override {
            std::unique_lock<std::recursive_mutex> lock(plugin->m_lock);
            if (m_action == "promote" || m_action == "check") {
                plugin->m_syncRequest = m_action;
                return std::make_unique<Command::Result>("Failover: content " + m_action + " requested");
            }
            if (m_action == "rollback") {
                plugin->m_syncRequest = "rollback";
                plugin->m_syncRequestSnapshot = args.size() > 0 && !args[0].empty() ? args[0] : "latest";
                plugin->m_syncRequestTarget = args.size() > 1 && !args[1].empty() ? args[1] : "both";
                return std::make_unique<Command::Result>("Failover: content rollback requested");
            }
            plugin->m_pendingAction = m_action;
            plugin->m_pendingArg = Json::Value(Json::objectValue);
            plugin->m_pendingArg["manual"] = m_manual;
            plugin->m_pendingArg["reason"] = "FPP command";
            return std::make_unique<Command::Result>("Failover: " + m_action + " requested");
        }
        FPPFailoverPlugin* plugin;
        std::string m_action;
        bool m_manual;
    };

    void registerCommands() {
        addOwnedCommand(new FailoverCommand(this, "Failover Take Over",
                                            "Take over as the active player (graceful, at the end of the peer's current item)",
                                            "takeover", true));
        addOwnedCommand(new FailoverCommand(this, "Failover Yield",
                                            "Stop running the show and stand by until told otherwise",
                                            "yield", true));
        addOwnedCommand(new FailoverCommand(this, "Failover Fail Back",
                                            "Return the show to the primary player",
                                            "failback", false));
        addOwnedCommand(new FailoverCommand(this, "Failover Promote Content",
                                            "Snapshot the current stable content, then pull the primary's sequences, media, playlists and schedule",
                                            "promote", false));
        addOwnedCommand(new FailoverCommand(this, "Failover Check Content",
                                            "Dry run: list what a promote would change",
                                            "check", false));
        FailoverCommand* rb = new FailoverCommand(this, "Failover Rollback Content",
                                                  "Restore a content snapshot onto this node and/or the primary",
                                                  "rollback", false);
        rb->args.push_back(Command::CommandArg("snapshot", "string", "Snapshot id").setDefaultValue("latest"));
        rb->args.push_back(Command::CommandArg("target", "string", "Target")
                               .setContentList({ "both", "primary", "local" })
                               .setDefaultValue("both"));
        addOwnedCommand(rb);
    }
    void addOwnedCommand(Command* c) {
        m_commands.push_back(c);
        CommandManager::INSTANCE.addCommand(c);
    }
    void removeCommands() {
        for (Command* c : m_commands) {
            CommandManager::INSTANCE.removeCommand(c);
            delete c;
        }
        m_commands.clear();
    }

    // ------------------------------------------------------------------
    // Data
    // ------------------------------------------------------------------
    std::recursive_mutex m_lock;
    std::string m_logFile, m_dataDir, m_stateFile, m_hostName;
    std::vector<std::string> m_localAddresses;
    long long m_lastAddrRefreshMS = 0;
    long long m_lastWarnCheckMS = 0;

    Role m_role = Role::Off;
    NodeState m_state = NodeState::Hold;
    int m_epoch = 0;
    bool m_manualStandby = false;
    bool m_bootedRemote = false;

    std::string m_peerAddress;
    int m_pollIntervalMS = 1000;
    int m_failureThreshold = 3;
    int m_recoveryThreshold = 5;
    std::string m_failbackMode = "auto";
    bool m_witnessCheck = false;
    std::string m_witnessAddresses;
    int m_witnessGraceSec = 15;
    int m_handoffMarginMS = 300;
    std::string m_resumeMode = "item";
    int m_hotDetectMS = 1500;
    int m_takeoverLeadMS = 40;
    std::string m_remoteFallbackSequence;
    int m_remoteFallbackDelaySec = 15;

    // Content sync
    bool m_contentSync = false;
    int m_contentSyncIntervalMin = 30;
    std::string m_contentSyncDirs;
    bool m_contentSyncPlaylists = true;
    bool m_contentSyncSchedule = true;
    bool m_contentSyncDelete = false;
    bool m_contentSyncCompress = false;
    std::string m_contentSyncMode = "manual";
    int m_contentSyncSoakMin = 0;
    int m_contentSnapshots = 5;
    pid_t m_syncPid = 0;
    std::string m_syncMode;
    long long m_syncStartedMS = 0;
    long long m_lastSyncMS = 0;
    long long m_lastCheckMS = 0;
    long long m_nextSyncMS = 0;
    std::string m_syncRequest, m_syncRequestSnapshot, m_syncRequestTarget;
    bool m_lastSyncOK = false;
    Json::Value m_lastSyncStatus;
    Json::Value m_pending;
    std::string m_pendingFingerprint;
    long long m_pendingSinceMS = 0;

    PeerInfo m_peer;
    bool m_pollInFlight = false;
    long long m_lastPollSentMS = 0;
    long long m_lastHeartbeatSentMS = 0;
    bool m_peerWarned = false;
    long long m_lastNudgeMS = 0;

    long long m_startMS = 0;
    long long m_activatedMS = 0;
    bool m_bootAssumedActive = false;
    long long m_holdStartedMS = 0;
    long long m_handoffActivateAtMS = 0;
    long long m_handoffRequestedMS = 0;
    long long m_handoffStartedMS = 0;
    int m_handoffNextIndex = -1;
    std::string m_handoffPlaylist;

    // Hot takeover
    bool m_forcePlayerMode = false;
    std::atomic<bool> m_syncSendActive{ false };
    std::atomic<bool> m_suppressAnnounce{ false };
    std::atomic<long long> m_shadowRemainingMS{ 0 };
    std::atomic<long long> m_shadowLastFrameMS{ 0 };
    std::mutex m_sendLock;
    std::string m_sendSeqName, m_candidateName, m_sendMediaName;
    int m_candidateCount = 0;
    bool m_sendMediaStarted = false;
    long long m_lastMediaSyncMS = 0;
    long long m_schedReloadRequestedMS = 0;
    long long m_takeoverStartedMS = 0;
    int m_takeoverIndex = -2;
    std::string m_takeoverReason;
    std::string m_takeoverSeq;

    long long m_witnessAliveMS = 0;
    long long m_lastWitnessPollMS = 0;
    int m_witnessInFlight = 0;
    std::string m_witnessVouchedBy;
    bool m_witnessBlockedLogged = false;

    std::map<std::string, long long> m_heard;
    std::atomic<long long> m_lastSyncPacketMS{ 0 };
    std::atomic<bool> m_syncSeen{ false };
    std::string m_syncMasterIP;
    long long m_lastHeartbeatMS = 0;
    long long m_lastActiveHeartbeatMS = 0;
    Json::Value m_activeMaster;
    long long m_fallbackStartedMS = 0;
    bool m_fallbackLogged = false;
    bool m_fallbackMissingWarned = false;

    std::string m_pendingAction;
    Json::Value m_pendingArg;

    std::map<int, std::string> m_warnings;
    std::vector<Command*> m_commands;

    std::mutex m_eventsLock;
    std::deque<std::string> m_events;
};

// Routes go through registerPluginApi(), commands are withdrawn in shutdown(),
// the timer is stopped and outstanding curl requests are cancelled by owner,
// the MultiSync hook is removed and the per-frame sender is switched off
// before the object goes - nothing outside this library holds a pointer into
// it once shutdown() returns.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
FPPPlugins::Plugin* createPlugin() {
    return new FPPFailoverPlugin();
}
}
