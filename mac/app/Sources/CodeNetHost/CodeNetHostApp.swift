// Code Net menu-bar app: runs beside Ableton Live on every show Mac.
import AppKit
import SwiftUI

@main
struct CodeNetHostApp: App {
    @StateObject private var engine = Engine()

    var body: some Scene {
        MenuBarExtra {
            DashboardView(engine: engine)
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
                Text(engine.timecode)
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
            Text("Receivers").font(.headline)
            if engine.receivers.isEmpty {
                Text("No receivers have reported yet. Receivers report to hosts they hear from.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            ForEach(engine.receivers) { r in
                ReceiverRow(entry: r, hostName: engine.settings.hostName, hostChoices: hostChoices,
                            assigning: engine.assigning.contains(r.id), assignFailed: engine.assignFailed.contains(r.id),
                            onAssign: { main, backup in engine.assign(receiver: r.id, main: main, backup: backup) })
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
    let onAssign: (String, String?) -> Void

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
                    Text(s.output ?? "stopped").font(.system(.body, design: .monospaced))
                }
                Text(detail(s)).font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                assignment
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
