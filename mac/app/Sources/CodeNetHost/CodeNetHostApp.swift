// Code Net menu-bar app: runs beside Ableton Live on every show Mac.
import AppKit
import SwiftUI

@main
struct CodeNetHostApp: App {
    @StateObject private var engine = Engine()
    @StateObject private var receiver = ReceiverEngine()

    var body: some Scene {
        MenuBarExtra {
            DashboardView(engine: engine, receiver: receiver)
        } label: {
            Image(systemName: engine.menuBarSymbol)
        }
        .menuBarExtraStyle(.window)
    }
}

extension Engine {
    var menuBarSymbol: String {
        if !problems.isEmpty { return "exclamationmark.triangle" }
        switch pluginState {
        case .playing: return "dot.radiowaves.left.and.right"
        case .stopped: return "pause.circle"
        case .lost, .notSeen: return "antenna.radiowaves.left.and.right.slash"
        }
    }
}

struct DashboardView: View {
    @ObservedObject var engine: Engine
    @ObservedObject var receiver: ReceiverEngine
    @State private var targetsText = ""
    @State private var nameText = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            if !engine.problems.isEmpty {
                ForEach(engine.problems, id: \.self) { problem in
                    Label(problem, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .font(.callout)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Divider()
            receiversSection
            Divider()
            ReceiveSection(receiver: receiver, hostChoices: hostChoices)
            Divider()
            settingsSection
            HStack {
                Text("Code Net \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev")")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
            }
        }
        .padding(16)
        .frame(width: 520)
        .onAppear {
            targetsText = engine.settings.unicastTargets.joined(separator: ", ")
            nameText = engine.settings.hostName
        }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 2) {
                TimelineView(.animation(minimumInterval: 1.0 / 60, paused: !engine.anchor.playing)) { _ in
                    Text(engine.anchor.timecode(at: CodeNet.nowNs()))
                }
                    .font(.system(size: 34, weight: .medium, design: .monospaced))
                    .foregroundStyle(engine.pluginState == .playing ? Color.green : Color.secondary)
                Text(engine.pluginState.rawValue + (engine.rate > 0 ? "  ·  \(engine.rate) fps" : ""))
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text(engine.settings.hostName).font(.headline)
                Text("\(engine.framesSent) frames sent").font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var receiversSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Receivers").font(.headline)
                Spacer()
                if engine.receivers.contains(where: { $0.status.readback != nil }) {
                    Button("Measure all offsets") {
                        for r in engine.receivers where r.status.readback != nil {
                            engine.calibrate(receiver: r.id)
                        }
                    }
                    .font(.callout)
                    .help("Each receiver with a readback loop times its own output and sets its offset so all "
                          + "of them put out the same frame at the same moment. Play timecode first.")
                }
            }
            if engine.receivers.isEmpty {
                Text("No receivers have reported yet. Receivers report to hosts they hear from.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            ForEach(engine.receivers) { r in
                ReceiverRow(entry: r, hostName: engine.settings.hostName, hostChoices: hostChoices,
                            assigning: engine.assigning.contains(r.id), assignFailed: engine.assignFailed.contains(r.id),
                            calibrating: engine.calibrating.contains(r.id),
                            calibrateFailed: engine.calibrateFailed.contains(r.id),
                            onAssign: { main, backup in engine.assign(receiver: r.id, main: main, backup: backup) },
                            onCalibrate: { ms in engine.calibrate(receiver: r.id, advanceMs: ms) })
            }
        }
    }

    /// Every host name this Mac knows of: itself and whatever the receivers hear.
    private var hostChoices: [String] {
        var names: Set<String> = [engine.settings.hostName]
        for r in engine.receivers {
            names.formUnion(r.status.heard ?? [])
            names.formUnion((r.status.sources ?? []).map(\.name))
        }
        names.formUnion(receiver.status?.heard ?? [])
        names.formUnion((receiver.status?.sources ?? []).map(\.name))
        return names.filter { !$0.isEmpty }.sorted()
    }

    private var settingsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("This host").font(.headline)
            HStack {
                Text("Host name").frame(width: 110, alignment: .leading)
                // Applied on Return, so receivers don't see every partial name while typing.
                TextField("mac-a", text: $nameText, onCommit: applyName)
                    .textFieldStyle(.roundedBorder)
                Button("Apply", action: applyName)
            }
            Toggle("Send by multicast (\(CodeNet.multicastGroup))", isOn: Binding(
                get: { engine.settings.multicast }, set: { engine.settings.multicast = $0 }))
            HStack {
                Text("Also send to").frame(width: 110, alignment: .leading)
                TextField("receiver IPs, comma separated", text: $targetsText, onCommit: applyTargets)
                    .textFieldStyle(.roundedBorder)
                Button("Apply", action: applyTargets)
            }
            Text("Receivers name this host as their main or backup using the host name above.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    /// Cuts a name to whole characters that fit the 16-byte source id.
    static func fitSourceId(_ name: String) -> String {
        var out = ""
        for c in name {
            if out.utf8.count + String(c).utf8.count > 16 { break }
            out.append(c)
        }
        return out
    }

    private func applyName() {
        let name = Self.fitSourceId(nameText.trimmingCharacters(in: .whitespaces))
        nameText = name
        if !name.isEmpty { engine.settings.hostName = name }
    }

    private func applyTargets() {
        engine.settings.unicastTargets = targetsText
            .split(whereSeparator: { $0 == "," || $0 == " " })
            .map(String.init)
            .filter { !$0.isEmpty }
    }
}

struct ReceiverRow: View {
    let entry: ReceiverEntry
    let hostName: String
    let hostChoices: [String]
    let assigning: Bool
    let assignFailed: Bool
    let calibrating: Bool
    let calibrateFailed: Bool
    let onAssign: (String, String?) -> Void
    let onCalibrate: (Double?) -> Void
    @State private var offsetText = ""
    @FocusState private var editingOffset: Bool

    private var currentMain: String {
        entry.status.main ?? entry.status.sources?.first { $0.role == "main" }?.name ?? ""
    }

    private var currentBackup: String {
        entry.status.backup ?? entry.status.sources?.first { $0.role == "backup" }?.name ?? ""
    }

    private var assignment: some View {
        HStack(spacing: 8) {
            Picker("Main", selection: Binding(
                get: { currentMain },
                set: { main in onAssign(main, main == currentBackup ? orNil(currentMain) : orNil(currentBackup)) }
            )) {
                ForEach(choices(including: currentMain), id: \.self) { Text($0).tag($0) }
            }
            .frame(width: 180)
            Picker("Backup", selection: Binding(
                get: { currentBackup },
                set: { backup in
                    if backup == currentMain { return }
                    onAssign(currentMain, backup.isEmpty ? nil : backup)
                }
            )) {
                Text("None").tag("")
                ForEach(choices(including: currentBackup).filter { !$0.isEmpty && $0 != currentMain }, id: \.self) {
                    Text($0).tag($0)
                }
            }
            .frame(width: 190)
            if assigning {
                ProgressView().controlSize(.small)
            } else if assignFailed {
                Text("not confirmed").font(.caption).foregroundStyle(.orange)
            }
        }
        .font(.caption)
        .disabled(stale)
    }

    /// Output offset: how much earlier this receiver starts each frame to cancel its device's
    /// delay. Measured from its readback loop, or typed in.
    private var offset: some View {
        let s = entry.status
        return HStack(spacing: 6) {
            Text("Output offset")
            TextField("0", text: $offsetText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 64)
                .focused($editingOffset)
                .onSubmit(setOffset)
            Text("ms earlier")
            Button("Set", action: setOffset)
            Button("Measure") { onCalibrate(nil) }
                .disabled(s.readback == nil)
                .help(s.readback == nil
                      ? "Needs the output looped back into an input (readback_device on a Pi)."
                      : "Time the output through its readback loop and set the offset to match. Play timecode first.")
            if calibrating || s.calibration?.state == "measuring" {
                ProgressView().controlSize(.small)
            } else if calibrateFailed {
                Text("not confirmed").foregroundStyle(.orange)
            } else if let c = s.calibration, c.state == "failed" {
                Text(c.message ?? "failed").foregroundStyle(.orange).lineLimit(2)
            } else if let c = s.calibration, let measured = c.measured_ms {
                Text(String(format: "measured %+.2f ms", measured)).foregroundStyle(.secondary)
            }
        }
        .font(.caption)
        .disabled(stale)
        .onAppear(perform: syncOffsetText)
        .onChange(of: s.advance_ms) { _ in syncOffsetText() }
    }

    private func syncOffsetText() {
        if !editingOffset { offsetText = String(format: "%.2f", entry.status.advance_ms ?? 0) }
    }

    private func setOffset() {
        guard let ms = Double(offsetText.replacingOccurrences(of: ",", with: ".")), abs(ms) < 1000 else {
            syncOffsetText()
            return
        }
        editingOffset = false
        onCalibrate(ms)
    }

    private func orNil(_ name: String) -> String? { name.isEmpty ? nil : name }

    private func choices(including current: String) -> [String] {
        var names = hostChoices
        if !current.isEmpty && !names.contains(current) { names.append(current) }
        return names
    }

    private var stale: Bool { Date().timeIntervalSince(entry.lastSeen) > 3 }

    var body: some View {
        let s = entry.status
        HStack(alignment: .top, spacing: 10) {
            Circle()
                .fill(stale ? Color.gray : s.output != nil ? Color.green : Color.red)
                .frame(width: 10, height: 10)
                .padding(.top, 5)
            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Text(entry.id).font(.body.weight(.semibold))
                    Text(entry.address).font(.caption).foregroundStyle(.secondary)
                    if s.wifi == true { Text("Wi-Fi mode").font(.caption).foregroundStyle(.orange) }
                    Spacer()
                    let anchor = entry.outputAnchor
                    TimelineView(.animation(minimumInterval: 1.0 / 60, paused: anchor == nil || stale)) { _ in
                        Text(stale ? s.output ?? "stopped" : anchor?.timecode(at: CodeNet.nowNs()) ?? "stopped")
                    }
                    .font(.system(.body, design: .monospaced))
                }
                Text(detail(s)).font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                assignment
                if s.advance_ms != nil { offset }
            }
        }
        .opacity(stale ? 0.5 : 1)
    }

    private func detail(_ s: ReceiverStatus) -> String {
        var parts: [String] = []
        if stale { parts.append("not heard for \(Int(Date().timeIntervalSince(entry.lastSeen))) s") }
        if let following = s.following {
            let host = s.sources?.first { $0.role == following }?.name ?? following
            parts.append("following \(following) (\(host))")
        } else if let idle = s.idle {
            parts.append(idle)
        }
        if let me = s.sources?.first(where: { $0.name == hostName }) {
            parts.append("this Mac is its \(me.role)")
            if let gap = me.gap_ms { parts.append(String(format: "gap %.0f ms", gap)) }
        }
        if let device = s.device { parts.append(device) }
        if let rb = s.readback {
            let offset = rb.offset_ms.map { String(format: " %+.2f ms", $0) } ?? ""
            parts.append("output \(rb.state)\(offset)")
        }
        return parts.joined(separator: "  ·  ")
    }
}

/// Receiving timecode on this Mac, like a Pi: follow a main and backup host and output
/// fresh LTC on an audio interface or MTC to a MIF4.
struct ReceiveSection: View {
    @ObservedObject var receiver: ReceiverEngine
    let hostChoices: [String]
    @State private var nameText = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle(isOn: $receiver.settings.enabled) {
                Text("Receive timecode on this Mac").font(.headline)
            }
            if receiver.settings.enabled {
                statusLine
                if let error = receiver.error {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange).font(.callout)
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack {
                    Text("Receiver name").frame(width: 110, alignment: .leading)
                    TextField("mac-rx", text: $nameText, onCommit: applyName)
                        .textFieldStyle(.roundedBorder)
                    Button("Apply", action: applyName)
                }
                HStack(spacing: 8) {
                    Picker("Main", selection: $receiver.settings.main) {
                        Text("None").tag("")
                        ForEach(choices(including: receiver.settings.main), id: \.self) { Text($0).tag($0) }
                    }
                    Picker("Backup", selection: $receiver.settings.backup) {
                        Text("None").tag("")
                        ForEach(choices(including: receiver.settings.backup).filter { $0 != receiver.settings.main },
                                id: \.self) { Text($0).tag($0) }
                    }
                }
                HStack {
                    Picker("Output", selection: outputSelection) {
                        Text("None").tag("")
                        ForEach(receiver.devices) { Text($0.label).tag($0.id) }
                    }
                    Button {
                        receiver.refreshDevices()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .help("Look for new audio and MIDI devices")
                }
                if receiver.settings.outputKind == .audio, let device = selectedDevice {
                    HStack {
                        Picker("Channel", selection: $receiver.settings.channel) {
                            Text("All").tag(-1)
                            ForEach(0..<max(device.channels, 1), id: \.self) { Text("\($0 + 1)").tag($0) }
                        }
                        .frame(width: 150)
                        Text("Level").padding(.leading, 8)
                        Slider(value: $receiver.settings.level, in: 0.05...1)
                    }
                }
                HStack {
                    Text("Output offset").frame(width: 110, alignment: .leading)
                    TextField("0", value: $receiver.settings.advanceMs, format: .number.precision(.fractionLength(0...2)))
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 70)
                    Text("ms earlier").foregroundStyle(.secondary)
                }
                .help("Start each frame this much earlier to cancel the output device's delay, so this Mac "
                      + "lines up with the other receivers. Hosts can also set it from their receiver list.")
                HStack {
                    Toggle("Multicast", isOn: $receiver.settings.multicast)
                    Toggle("Wi-Fi mode", isOn: $receiver.settings.wifi)
                    Spacer()
                    Button("Return to main", action: receiver.returnToMain)
                }
                .font(.callout)
            }
        }
        .onAppear { nameText = receiver.settings.name }
    }

    private var statusLine: some View {
        let s = receiver.status
        let running = s?.output != nil
        return VStack(alignment: .leading, spacing: 2) {
            HStack {
                Circle().fill(running ? Color.green : Color.secondary).frame(width: 10, height: 10)
                TimelineView(.animation(minimumInterval: 1.0 / 60, paused: !running)) { _ in
                    Text(receiver.timecodeNow() ?? s?.output ?? "--:--:--:--")
                }
                .font(.system(.title3, design: .monospaced))
                if let following = s?.following { Text("following \(following)").foregroundStyle(.secondary) }
            }
            if let idle = s?.idle {
                Text(idle).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            } else if let device = s?.device {
                Text(device).font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var selectedDevice: OutputDevice? {
        receiver.devices.first { $0.kind == receiver.settings.outputKind && $0.deviceId == receiver.settings.deviceId }
    }

    private var outputSelection: Binding<String> {
        Binding(
            get: { selectedDevice?.id ?? "" },
            set: { id in
                var s = receiver.settings
                if let device = receiver.devices.first(where: { $0.id == id }) {
                    s.outputKind = device.kind
                    s.deviceId = device.deviceId
                    if device.kind == .audio && s.channel >= device.channels { s.channel = 0 }
                } else {
                    s.deviceId = ""
                }
                receiver.settings = s
            })
    }

    private func choices(including current: String) -> [String] {
        var names = hostChoices
        if !current.isEmpty && !names.contains(current) { names.append(current) }
        return names
    }

    private func applyName() {
        let name = DashboardView.fitSourceId(nameText.trimmingCharacters(in: .whitespaces))
        nameText = name
        if !name.isEmpty { receiver.settings.name = name }
    }
}
