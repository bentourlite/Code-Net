// Receiving on this Mac: the same job as a Raspberry Pi receiver (follow a main and a
// backup host, generate fresh LTC or MTC), run by the C++ engine in cpp/core.
import CodeNetCore
import Foundation

struct ReceiverSettings: Equatable {
    var enabled = false
    var name = "mac-rx"
    var main = ""
    var backup = ""
    var outputKind = OutputDevice.Kind.audio
    var deviceId = ""
    var channel = 0  // 0-based audio output, -1 for all
    var level: Float = 0.5
    var multicast = true
    var wifi = false
    var advanceMs = 0.0

    static func load() -> ReceiverSettings {
        let d = UserDefaults.standard
        var s = ReceiverSettings()
        s.enabled = d.bool(forKey: "rx.enabled")
        s.name = d.string(forKey: "rx.name") ?? s.name
        s.main = d.string(forKey: "rx.main") ?? ""
        s.backup = d.string(forKey: "rx.backup") ?? ""
        s.outputKind = OutputDevice.Kind(rawValue: d.integer(forKey: "rx.outputKind")) ?? .audio
        s.deviceId = d.string(forKey: "rx.deviceId") ?? ""
        s.channel = d.object(forKey: "rx.channel") as? Int ?? 0
        s.level = d.object(forKey: "rx.level") as? Float ?? 0.5
        s.multicast = d.object(forKey: "rx.multicast") as? Bool ?? true
        s.wifi = d.bool(forKey: "rx.wifi")
        s.advanceMs = d.double(forKey: "rx.advanceMs")
        return s
    }

    func save() {
        let d = UserDefaults.standard
        d.set(enabled, forKey: "rx.enabled")
        d.set(name, forKey: "rx.name")
        d.set(main, forKey: "rx.main")
        d.set(backup, forKey: "rx.backup")
        d.set(outputKind.rawValue, forKey: "rx.outputKind")
        d.set(deviceId, forKey: "rx.deviceId")
        d.set(channel, forKey: "rx.channel")
        d.set(level, forKey: "rx.level")
        d.set(multicast, forKey: "rx.multicast")
        d.set(wifi, forKey: "rx.wifi")
        d.set(advanceMs, forKey: "rx.advanceMs")
    }

    /// The settings that need the network side restarting when they change.
    var networkKey: String { "\(name)|\(multicast)|\(wifi)" }
    var outputKey: String { "\(outputKind.rawValue)|\(deviceId)|\(channel)" }
}

struct OutputDevice: Identifiable, Hashable {
    enum Kind: Int { case audio = 0, midi = 1 }
    var kind: Kind
    var deviceId: String
    var name: String
    var channels: Int

    var id: String { "\(kind.rawValue):\(deviceId)" }
    var label: String { kind == .midi ? "MIDI (MTC): \(name)" : "Audio (LTC): \(name)" }
}

final class ReceiverEngine: ObservableObject {
    @Published var settings: ReceiverSettings {
        didSet {
            guard settings != oldValue else { return }
            settings.save()
            apply()
        }
    }
    @Published private(set) var status: ReceiverStatus?
    @Published private(set) var error: String?
    @Published private(set) var devices: [OutputDevice] = []

    private var handle: OpaquePointer?
    private var networkKey = ""
    private var outputKey = ""
    private var appliedSources = ("", "")
    private var timer: Timer?

    init() {
        settings = ReceiverSettings.load()
        refreshDevices()
        apply()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.poll() }
    }

    deinit {
        timer?.invalidate()
        stop()
    }

    func refreshDevices() {
        var list = [cn_device](repeating: cn_device(), count: 64)
        let count = Int(cn_list_devices(&list, Int32(list.count)))
        devices = list.prefix(min(count, list.count)).map { d in
            OutputDevice(kind: OutputDevice.Kind(rawValue: Int(d.kind)) ?? .audio, deviceId: Self.string(d.id),
                         name: Self.string(d.name), channels: Int(d.channels))
        }
    }

    func returnToMain() {
        if let h = handle { cn_receiver_return_to_main(h) }
    }

    // MARK: engine

    private func apply() {
        var s = settings
        if s.backup == s.main { s.backup = "" }
        guard s.enabled else {
            stop()
            error = nil
            status = nil
            return
        }
        if handle == nil || networkKey != s.networkKey {
            stop()
            var message = [CChar](repeating: 0, count: 256)
            let group = s.multicast ? CodeNet.multicastGroup : "none"
            let created: OpaquePointer? = s.name.withCString { name -> OpaquePointer? in
                s.main.withCString { main -> OpaquePointer? in
                    s.backup.withCString { backup -> OpaquePointer? in
                        group.withCString { group -> OpaquePointer? in
                            var c = cn_receiver_config(
                                name: name, main: main, backup: backup, group: group,
                                port: Int32(CodeNet.framePort), loss_timeout_frames: 2, max_predict_frames: 2,
                                wifi: s.wifi ? 1 : 0, auto_return: 1, advance_ms: s.advanceMs)
                            return cn_receiver_create(&c, &message, Int32(message.count))
                        }
                    }
                }
            }
            guard let h = created else {
                error = String(cString: message)
                return
            }
            handle = h
            networkKey = s.networkKey
            outputKey = ""
            appliedSources = (s.main, s.backup)
            cn_receiver_set_assign_callback(h, { context, main, backup in
                guard let context, let main, let backup else { return }
                let engine = Unmanaged<ReceiverEngine>.fromOpaque(context).takeUnretainedValue()
                let m = String(cString: main), b = String(cString: backup)
                DispatchQueue.main.async { engine.assigned(main: m, backup: b) }
            }, Unmanaged.passUnretained(self).toOpaque())
            cn_receiver_set_advance_callback(h, { context, ms in
                guard let context else { return }
                let engine = Unmanaged<ReceiverEngine>.fromOpaque(context).takeUnretainedValue()
                DispatchQueue.main.async { engine.settings.advanceMs = ms }
            }, Unmanaged.passUnretained(self).toOpaque())
        }
        guard let h = handle else { return }
        if appliedSources != (s.main, s.backup) {
            cn_receiver_set_sources(h, s.main, s.backup)
            appliedSources = (s.main, s.backup)
        }
        if outputKey != s.outputKey {
            outputKey = s.outputKey
            if s.deviceId.isEmpty {
                cn_receiver_stop_output(h)
                error = nil
            } else {
                var message = [CChar](repeating: 0, count: 256)
                let ok = cn_receiver_start_output(h, Int32(s.outputKind.rawValue), s.deviceId, Int32(s.channel),
                                                  s.level, &message, Int32(message.count))
                error = ok == 0 ? nil : String(cString: message)
            }
        }
        cn_receiver_set_level(h, s.level)
        cn_receiver_set_advance(h, s.advanceMs)
    }

    /// The timecode on the output right now, for a display that counts every frame.
    func timecodeNow() -> String? {
        guard let h = handle else { return nil }
        var buffer = [CChar](repeating: 0, count: 16)
        return cn_receiver_timecode_now(h, &buffer, Int32(buffer.count)) > 0 ? String(cString: buffer) : nil
    }

    private func stop() {
        if let h = handle {
            cn_receiver_set_assign_callback(h, nil, nil)
            cn_receiver_set_advance_callback(h, nil, nil)
            cn_receiver_destroy(h)
        }
        handle = nil
        networkKey = ""
        outputKey = ""
    }

    /// A host app changed this receiver's main or backup; keep it for next time.
    private func assigned(main: String, backup: String) {
        appliedSources = (main, backup)
        settings.main = main
        settings.backup = backup
    }

    private func poll() {
        guard let h = handle else { return }
        var buffer = [CChar](repeating: 0, count: 4096)
        cn_receiver_status(h, &buffer, Int32(buffer.count))
        let data = Data(String(cString: buffer).utf8)
        if let decoded = try? JSONDecoder().decode(ReceiverStatus.self, from: data), decoded != status {
            status = decoded
        }
    }

    private static func string<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self) }
    }
}
