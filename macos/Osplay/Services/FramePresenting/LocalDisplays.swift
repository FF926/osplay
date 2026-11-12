import AppKit
import CoreBridge
import CoreGraphics

/// One of this Mac's displays, as offered to a host when this Mac is used as a display.
struct LocalDisplay: Equatable, Sendable, Identifiable {
    /// What the host is told. Its ID is the display's place from left to right.
    let description: MonitorDescription
    let displayID: CGDirectDisplayID
    /// The display's identity across launches, for leaving it out (`AppSettings.excludedDisplays`):
    /// its name and place, like the Windows app's, because the ID follows the arrangement.
    let key: String
    /// Where it sits among the displays (top-left origin, points); the picker draws it there.
    let frame: CGRect

    var id: UInt8 { description.id }

    /// Whether a host still shows this display right: same size, refresh rate and scale.
    func hasSameMode(as other: LocalDisplay) -> Bool {
        description.widthPixels == other.description.widthPixels
            && description.heightPixels == other.description.heightPixels
            && description.refreshMillihertz == other.description.refreshMillihertz
            && description.scalePercent == other.description.scalePercent
    }
}

/// The displays this Mac offers when it is used as a viewer (DEVPLAN Phase 13b: every display).
enum LocalDisplays {
    /// A display as the window server reports it, before it gets an ID.
    struct Candidate: Equatable, Sendable {
        let displayID: CGDirectDisplayID
        let name: String
        /// Top-left origin, points.
        let frame: CGRect
        let widthPixels: Int
        let heightPixels: Int
        /// 0 when the display reports none.
        let refreshRate: Double
        let physicalMillimeters: CGSize
        let backingScale: CGFloat
    }

    /// Every attached display that can be offered.
    static func all() -> [LocalDisplay] {
        offered(from: NSScreen.screens.compactMap(candidate).filter { !isOwnVirtualDisplay($0.displayID) })
    }

    /// (fork) This Mac's own virtual displays never count as screens a host can take over. One
    /// left over from a hosting session that ended only moments ago would otherwise be offered to
    /// the next host: its aspect matches no real screen, and a viewer window of it fullscreened
    /// shows black bars.
    private static func isOwnVirtualDisplay(_ displayID: CGDirectDisplayID) -> Bool {
        CGDisplayVendorNumber(displayID) == VirtualDisplayIdentity.osplayVendorID
    }

    /// From left to right (then top to bottom), so the IDs follow the arrangement as on Windows; at
    /// most `Tunables.Viewer.maximumDisplays`. A display the protocol cannot describe is left out.
    static func offered(from candidates: [Candidate]) -> [LocalDisplay] {
        let usable = candidates.filter {
            (Limit.minimumWidth...Limit.maximumSide).contains($0.widthPixels)
                && (Limit.minimumHeight...Limit.maximumSide).contains($0.heightPixels)
        }
        let ordered = usable.sorted { ($0.frame.minX, $0.frame.minY) < ($1.frame.minX, $1.frame.minY) }
        return ordered.prefix(Tunables.Viewer.maximumDisplays).enumerated().map { index, candidate in
            let refresh = candidate.refreshRate > 0 ? candidate.refreshRate : Tunables.Viewer.defaultRefreshRate
            let millihertz = (refresh * 1000).rounded()
            let description = MonitorDescription(
                id: UInt8(index), widthPixels: candidate.widthPixels, heightPixels: candidate.heightPixels,
                refreshMillihertz: UInt32(min(max(millihertz, Limit.minimumMillihertz), Limit.maximumMillihertz)),
                physicalWidthMillimeters: millimeters(candidate.physicalMillimeters.width),
                physicalHeightMillimeters: millimeters(candidate.physicalMillimeters.height),
                scalePercent: min(max(Int((candidate.backingScale * 100).rounded()), 100), 400),
                name: ProtocolText.name(candidate.name))
            return LocalDisplay(
                description: description, displayID: candidate.displayID,
                key: "\(candidate.name)|\(Int(candidate.frame.minX))|\(Int(candidate.frame.minY))",
                frame: candidate.frame)
        }
    }

    private static func millimeters(_ value: CGFloat) -> Int {
        min(max(Int(value.rounded()), 0), Limit.maximumMillimeters)
    }

    private static func candidate(_ screen: NSScreen) -> Candidate? {
        guard let displayID = screen.displayID, let mode = CGDisplayCopyDisplayMode(displayID) else { return nil }
        return Candidate(
            displayID: displayID, name: screen.localizedName, frame: CGDisplayBounds(displayID),
            widthPixels: mode.pixelWidth, heightPixels: mode.pixelHeight, refreshRate: mode.refreshRate,
            physicalMillimeters: CGDisplayScreenSize(displayID), backingScale: screen.backingScaleFactor)
    }

    /// What the protocol accepts in ViewerMonitors (PROTOCOL.md).
    private enum Limit {
        static let minimumWidth = 320
        static let minimumHeight = 240
        static let maximumSide = 16384
        static let minimumMillihertz = 1000.0
        static let maximumMillihertz = 500_000.0
        static let maximumMillimeters = 10_000
    }
}
