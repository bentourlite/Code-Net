// The host side of Code Net: takes frames from the Ableton plugin, sends them to the
// receivers, answers their clock sync requests and collects their status reports.
import Foundation

struct HostSettings: Equatable {
    var hostName: String
    var multicast: Bool
    var unicastTargets: [String]

    static let defaultsKey = "settings"

    static func load() -> HostSettings {
        let d = UserDefaults.standard
        let fallback = Host.current().localizedName.map { String($0.prefix(16)) } ?? "mac"
        return HostSettings(
            hostName: d.string(forKey: "hostName") ?? fallback,
            multicast: d.object(forKey: "multicast") as? Bool ?? true,
            unicastTargets: d.stringArray(forKey: "unicastTargets") ?? [])
    }

    func save() {
        let d = UserDefaults.standard
        d.set(hostName, forKey: "hostName")
        d.set(multicast, forKey: "multicast")
        d.set(unicastTargets, forKey: "unicastTargets")
    }
}

struct ReceiverEntry: Identifiable, Equatable {
    var id: String
    var address: String
    var status: ReceiverStatus
    var lastSeen: Date
}

enum PluginState: String {
    case notSeen = "Plugin not found"
    case playing = "Playing"
    case stopped = "Stopped (no LTC on the track)"
    case lost = "Plugin lost"
}

final class Engine: ObservableObject {
    // Published to the UI on the main queue.
    @Published private(set) var timecode = "--:--:--:--"
    @Published private(set) var rate: UInt8 = 0
    @Published private(set) var pluginState = PluginState.notSeen
    @Published private(set) var framesSent: UInt64 = 0
    @Published private(set) var receivers: [ReceiverEntry] = []
    @Published private(set) var problems: [String] = []
    /// Receivers with an assignment on its way, and ones that didn't confirm one.
    @Published private(set) var assigning: Set<String> = []
    @Published private(set) var assignFailed: Set<String> = []
    @Published var settings: HostSettings {
        didSet {
            settings.save()
            let s = settings
            queue.async { self.liveSettings = s }
        }
    }

    private let queue = DispatchQueue(label: "codenet.engine", qos: .userInteractive)
    private var liveSettings: HostSettings
    private var pluginSocket: UDPSocket?
    private var syncSocket: UDPSocket?
    private var sendSocket: UDPSocket?
    private var heartbeat: DispatchSourceTimer?
    private var uiTimer: DispatchSourceTimer?

    // Engine state, only touched on `queue`.
    private var sequence: UInt32 = 0
    private var last = FramePacket()
    private var lastPluginPacketNs: Int64 = 0
    private var lastPlayingNs: Int64 = 0
    private var sent: UInt64 = 0
    private var receiverTable: [String: ReceiverEntry] = [:]
    private var sendError: String?
    private var setupErrors: [String] = []
    private var pendingAssigns: [String: (assign: Assign, address: String, deadline: Int64)] = [:]
    private var failedAssigns: Set<String> = []
    private var assignSequence = UInt32(truncatingIfNeeded: Int(Date().timeIntervalSince1970))

    static let pluginAliveNs: Int64 = 500_000_000

    init() {
        let s = HostSettings.load()
        settings = s
        liveSettings = s
        queue.async { self.start() }
    }

    private func start() {
        do {
            let plugin = try UDPSocket()
            try plugin.bind(host: "127.0.0.1", port: CodeNet.pluginPort)
            plugin.startReceiving(on: queue) { [weak self] data, _, _ in self?.onPluginPacket(data) }
            pluginSocket = plugin
        } catch {
            setupErrors.append("Can't listen for the plugin: \(error). Is another copy of Code Net running?")
        }
        do {
            let sync = try UDPSocket()
            try sync.bind(host: "0.0.0.0", port: CodeNet.syncPort)
            sync.startReceiving(on: queue) { [weak self] data, host, port in
                self?.onReceiverPacket(data, from: host, port: port)
            }
            syncSocket = sync
        } catch {
            setupErrors.append("Can't listen for receivers: \(error)")
        }
        sendSocket = try? UDPSocket()

        // Heartbeats at 30 Hz keep receivers' loss timers fed while there is no LTC.
        let hb = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
        hb.schedule(deadline: .now(), repeating: .milliseconds(33), leeway: .milliseconds(2))
        hb.setEventHandler { [weak self] in self?.sendHeartbeatIfIdle() }
        hb.resume()
        heartbeat = hb

        let ui = DispatchSource.makeTimerSource(queue: queue)
        ui.schedule(deadline: .now(), repeating: .milliseconds(200))
        ui.setEventHandler { [weak self] in self?.publish() }
        ui.resume()
        uiTimer = ui
    }

    // MARK: packets

    private func onPluginPacket(_ data: Data) {
        guard var packet = FramePacket.decode(data) else { return }
        let now = CodeNet.nowNs()
        lastPluginPacketNs = now
        last = packet
        if packet.playing {
            lastPlayingNs = now
            packet.source = CodeNet.sourceId(liveSettings.hostName)
            forward(&packet)
        }
    }

    private func sendHeartbeatIfIdle() {
        let now = CodeNet.nowNs()
        let frameNs = Int64(1_000_000_000) / Int64(max(last.rate, 25))
        guard now - lastPlayingNs > 2 * frameNs else { return }
        var packet = last
        packet.source = CodeNet.sourceId(liveSettings.hostName)
        packet.frameStartNs = now
        packet.flags = now - lastPluginPacketNs < Self.pluginAliveNs ? 0 : CodeNet.flagSourceLost
        forward(&packet)
    }

    private func forward(_ packet: inout FramePacket) {
        sequence &+= 1
        packet.sequence = sequence
        let data = packet.encode()
        guard let socket = sendSocket else { return }
        var error: String?
        if liveSettings.multicast {
            error = socket.send(data, to: CodeNet.multicastGroup, port: CodeNet.framePort) ?? error
        }
        for target in liveSettings.unicastTargets {
            if let e = socket.send(data, to: target, port: CodeNet.framePort) { error = "\(target): \(e)" }
        }
        sendError = error
        if packet.playing { sent += 1 }
    }

    private func onReceiverPacket(_ data: Data, from host: String, port: UInt16) {
        let t2 = CodeNet.nowNs()
        if let request = SyncRequest.decode(data) {
            let reply = request.response(host: CodeNet.sourceId(liveSettings.hostName), t2: t2, t3: CodeNet.nowNs())
            syncSocket?.send(reply, to: host, port: port)
        } else if let report = ReceiverStatus.decode(data) {
            receiverTable[report.id] = ReceiverEntry(id: report.id, address: host, status: report.status,
                                                     lastSeen: Date())
        }
    }

    // MARK: assignments

    /// Sets a receiver's main and backup hosts. Repeated until its status report shows it.
    func assign(receiver: String, main: String, backup: String?) {
        queue.async {
            guard let entry = self.receiverTable[receiver], !main.isEmpty, main != backup else { return }
            self.assignSequence &+= 1
            let assign = Assign(receiver: receiver, main: main, backup: backup?.isEmpty == true ? nil : backup,
                                sequence: self.assignSequence)
            self.pendingAssigns[receiver] = (assign, entry.address, CodeNet.nowNs() + 5_000_000_000)
            self.failedAssigns.remove(receiver)
            self.sendPendingAssigns()
        }
    }

    private func sendPendingAssigns() {
        let now = CodeNet.nowNs()
        for (id, pending) in pendingAssigns {
            let status = receiverTable[id]?.status
            if status?.main == pending.assign.main && status?.backup == pending.assign.backup {
                pendingAssigns[id] = nil
            } else if now > pending.deadline {
                pendingAssigns[id] = nil
                failedAssigns.insert(id)
            } else {
                sendSocket?.send(pending.assign.encode(), to: pending.address, port: CodeNet.framePort)
            }
        }
    }

    // MARK: UI

    private func publish() {
        sendPendingAssigns()
        let now = CodeNet.nowNs()
        let state: PluginState
        if lastPluginPacketNs == 0 { state = .notSeen }
        else if now - lastPluginPacketNs > Self.pluginAliveNs { state = .lost }
        else if now - lastPlayingNs < 200_000_000 { state = .playing }
        else { state = .stopped }
        receiverTable = receiverTable.filter { Date().timeIntervalSince($0.value.lastSeen) < 60 }
        let list = receiverTable.values.sorted { $0.id < $1.id }
        var problems = setupErrors
        if let e = sendError {
            problems.append(e.contains("No route") || e.contains("Host is down")
                ? "Can't send: \(e). Allow Code Net under System Settings > Privacy & Security > Local Network."
                : "Can't send: \(e)")
        }
        let tc = last.rate == 0 || lastPluginPacketNs == 0 ? "--:--:--:--" : last.timecode
        let snapshot = (tc, last.rate, state, sent, list, problems)
        let assigning = Set(pendingAssigns.keys), assignFailed = failedAssigns
        DispatchQueue.main.async {
            self.timecode = snapshot.0
            self.rate = snapshot.1
            self.pluginState = snapshot.2
            self.framesSent = snapshot.3
            if self.receivers != snapshot.4 { self.receivers = snapshot.4 }
            if self.problems != snapshot.5 { self.problems = snapshot.5 }
            if self.assigning != assigning { self.assigning = assigning }
            if self.assignFailed != assignFailed { self.assignFailed = assignFailed }
        }
    }
}
