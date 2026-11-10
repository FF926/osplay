import Foundation
import Observation

/// User settings (DEVPLAN: UserDefaults on macOS). Every change is saved at once.
@MainActor
@Observable
final class AppSettings {
    var qualityPreset: QualityPreset {
        didSet { defaults.set(qualityPreset.rawValue, forKey: Key.qualityPreset) }
    }

    /// The largest size a shared display is streamed at (1080p by default, user decision 2026-10-25).
    var streamSize: StreamSizePreset {
        didSet { defaults.set(streamSize.rawValue, forKey: Key.streamSize) }
    }

    /// Hides and shows the viewer.
    var viewerHotkey: Hotkey {
        didSet { save(viewerHotkey, forKey: Key.viewerHotkey) }
    }

    /// Reverses the roles of a running session.
    var swapHotkey: Hotkey {
        didSet { save(swapHotkey, forKey: Key.swapHotkey) }
    }

    /// Shows and hides the stats overlay while viewing.
    var statsHotkey: Hotkey {
        didSet { save(statsHotkey, forKey: Key.statsHotkey) }
    }

    /// A small box with fps, bitrate, round trip, loss and latency over the watched display.
    var showsStatsOverlay: Bool {
        didSet { defaults.set(showsStatsOverlay, forKey: Key.statsOverlay) }
    }

    /// Looks for a newer release once a day (on by default, like the Windows shell).
    var checksForUpdates: Bool {
        didSet { defaults.set(checksForUpdates, forKey: Key.checksForUpdates) }
    }

    /// When the last update check got an answer.
    var lastUpdateCheck: Date? {
        didSet { defaults.set(lastUpdateCheck, forKey: Key.lastUpdateCheck) }
    }

    /// The permissions window was finished once; afterwards it opens only when one is missing.
    var hasCompletedOnboarding: Bool {
        didSet { defaults.set(hasCompletedOnboarding, forKey: Key.onboarding) }
    }

    /// This Mac's displays not offered to hosts, by `LocalDisplay.key`.
    var excludedDisplays: Set<String> {
        didSet { save(excludedDisplays, forKey: Key.excludedDisplays) }
    }

    /// Per paired device (by device ID), how its monitors' virtual displays start.
    private(set) var hiDPIChoices: [String: HiDPIChoice] {
        didSet { save(hiDPIChoices, forKey: Key.hiDPIChoices) }
    }

    /// Per paired device (by device ID), the resolution each of its monitors ended at (fork): the
    /// user may have picked another mode in System Settings, and the next session starts there.
    private(set) var savedResolutions: [String: [UInt8: DisplayResolution]] {
        didSet { save(savedResolutions, forKey: Key.savedResolutions) }
    }

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        qualityPreset = defaults.string(forKey: Key.qualityPreset).flatMap(QualityPreset.init) ?? .high
        streamSize = defaults.string(forKey: Key.streamSize).flatMap(StreamSizePreset.init) ?? .hd1080
        viewerHotkey = Self.load(Hotkey.self, from: defaults, forKey: Key.viewerHotkey) ?? Tunables.Viewer.toggleHotkey
        swapHotkey = Self.load(Hotkey.self, from: defaults, forKey: Key.swapHotkey) ?? Tunables.Viewer.swapHotkey
        statsHotkey = Self.load(Hotkey.self, from: defaults, forKey: Key.statsHotkey) ?? Tunables.Viewer.statsHotkey
        showsStatsOverlay = defaults.bool(forKey: Key.statsOverlay)
        checksForUpdates = defaults.object(forKey: Key.checksForUpdates) as? Bool ?? true
        lastUpdateCheck = defaults.object(forKey: Key.lastUpdateCheck) as? Date
        hasCompletedOnboarding = defaults.bool(forKey: Key.onboarding)
        excludedDisplays = Self.load(Set<String>.self, from: defaults, forKey: Key.excludedDisplays) ?? []
        hiDPIChoices = Self.load([String: HiDPIChoice].self, from: defaults, forKey: Key.hiDPIChoices) ?? [:]
        arrangements = Self.load([String: [ArrangedMonitor]].self, from: defaults, forKey: Key.arrangements) ?? [:]
        savedResolutions =
            Self.load(
                [String: [UInt8: DisplayResolution]].self,
                from: defaults,
                forKey: Key.savedResolutions
            ) ?? [:]
    }

    /// Per paired device (by device ID), where each of its monitors sits next to this Mac.
    private(set) var arrangements: [String: [ArrangedMonitor]] {
        didSet { save(arrangements, forKey: Key.arrangements) }
    }

    func arrangement(for deviceID: String) -> [ArrangedMonitor] {
        arrangements[deviceID] ?? []
    }

    func setArrangement(_ arrangement: [ArrangedMonitor], for deviceID: String) {
        arrangements[deviceID] = arrangement.isEmpty ? nil : arrangement
    }

    func savedResolutions(for deviceID: String) -> [UInt8: DisplayResolution] {
        savedResolutions[deviceID] ?? [:]
    }

    func setSavedResolutions(_ resolutions: [UInt8: DisplayResolution], for deviceID: String) {
        savedResolutions[deviceID] = resolutions.isEmpty ? nil : resolutions
    }

    func hiDPIChoice(for deviceID: String) -> HiDPIChoice {
        hiDPIChoices[deviceID] ?? .automatic
    }

    func setHiDPIChoice(_ choice: HiDPIChoice, for deviceID: String) {
        hiDPIChoices[deviceID] = choice == .automatic ? nil : choice
    }

    private func save(_ value: some Encodable, forKey key: String) {
        // Encoding plain value types cannot fail.
        if let data = try? JSONEncoder().encode(value) {
            defaults.set(data, forKey: key)
        }
    }

    private static func load<Value: Decodable>(
        _ type: Value.Type, from defaults: UserDefaults, forKey key: String
    )
        -> Value?
    {
        // A value that no longer decodes (an older format) falls back to the default.
        defaults.data(forKey: key).flatMap { try? JSONDecoder().decode(type, from: $0) }
    }

    private enum Key {
        static let qualityPreset = "qualityPreset"
        static let streamSize = "streamSize"
        static let viewerHotkey = "viewerHotkey"
        static let swapHotkey = "swapHotkey"
        static let statsHotkey = "statsHotkey"
        static let statsOverlay = "showsStatsOverlay"
        static let hiDPIChoices = "hiDPIChoices"
        static let savedResolutions = "savedResolutions"
        static let onboarding = "hasCompletedOnboarding"
        static let arrangements = "displayArrangements"
        static let excludedDisplays = "excludedViewerDisplays"
        static let checksForUpdates = "checksForUpdates"
        static let lastUpdateCheck = "lastUpdateCheck"
    }
}
