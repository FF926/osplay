import CoreBridge
import CoreGraphics
import Foundation

/// A virtual display standing in for one of the viewer's monitors.
struct LentDisplay: Sendable, Hashable {
    let monitor: MonitorDescription
    let displayID: CGDirectDisplayID
    /// Where it sits next to this Mac's main display.
    var arrangement: ArrangedMonitor?
}

/// Creates the virtual displays a host session streams; injected so the session can be tested
/// without the window server.
@MainActor
protocol DisplayLending: AnyObject, Sendable {
    /// One display per monitor, ready, in the saved display state for that monitor when there is
    /// one (resolution and origin), otherwise in the HiDPI choice's resolution — placed as
    /// `arrangement` says when it still fits these monitors (a row to the right otherwise). All
    /// or nothing.
    func lend(
        _ monitors: [MonitorDescription], peerID: Data, hiDPI: HiDPIChoice,
        arrangement: [ArrangedMonitor], saved: [UInt8: SavedDisplayState]
    ) async throws(VirtualDisplayError) -> [LentDisplay]
    func returnDisplay(_ displayID: CGDirectDisplayID)
    func returnAll()
}

/// One per host session: it returns only the displays it lent, since every session of this Mac
/// shares the provider (DEVPLAN Phase 13b part SYM-03).
@MainActor
final class VirtualDisplayLender: DisplayLending {
    private let provider: any VirtualDisplayProviding
    private let isHiDPI: Bool
    private var handles: [any VirtualDisplayHandle] = []

    typealias Arrange = @MainActor ([(id: CGDirectDisplayID, origin: CGPoint)]) throws(VirtualDisplayError) -> Void
    private let arrange: Arrange

    init(
        provider: any VirtualDisplayProviding, isHiDPI: Bool = Tunables.Host.defaultHiDPI,
        arrange: @escaping Arrange = DisplayArranger.apply
    ) {
        self.provider = provider
        self.isHiDPI = isHiDPI
        self.arrange = arrange
    }

    func lend(
        _ monitors: [MonitorDescription], peerID: Data, hiDPI: HiDPIChoice,
        arrangement: [ArrangedMonitor], saved: [UInt8: SavedDisplayState]
    ) async throws(VirtualDisplayError) -> [LentDisplay] {
        returnAll()
        do throws(VirtualDisplayError) {
            var lent: [(MonitorDescription, any VirtualDisplayHandle)] = []
            for (index, monitor) in monitors.enumerated() {
                let handle = try provider.createDisplay(configuration(monitor, index: index, peerID: peerID))
                handles.append(handle)
                lent.append((monitor, handle))
            }
            var sizes: [(id: UInt8, name: String, size: CGSize)] = []
            for (monitor, handle) in lent {
                try await handle.waitUntilReady()
                let resolution = try Self.activateInitialResolution(
                    on: handle, for: monitor, choice: hiDPI, saved: saved[monitor.id])
                let points = CGSize(width: resolution.points.width, height: resolution.points.height)
                sizes.append((monitor.id, handle.configuration.name, points))
            }
            let arranged = DisplayLayout.arrangement(
                for: sizes, saved: arrangement, main: DisplayArranger.mainDisplaySize)
            // (fork) a display that ended at a known origin starts there again
            let placed = arranged.map { placed in
                var updated = placed
                if let state = saved[placed.id] {
                    updated.origin = state.origin
                }
                return updated
            }
            do throws(VirtualDisplayError) {
                try arrange(zip(lent, placed).map { ($0.1.displayID, $1.origin) })
            } catch {
                // The displays work anywhere; the user can still move them in Osplay's Settings.
                Log.virtualDisplay.error("Arranging displays failed: \(String(describing: error), privacy: .public)")
            }
            return zip(lent, placed).map { pair, arrangement in
                LentDisplay(monitor: pair.0, displayID: pair.1.displayID, arrangement: arrangement)
            }
        } catch {
            returnAll()
            throw error
        }
    }

    func returnDisplay(_ displayID: CGDirectDisplayID) {
        guard let index = handles.firstIndex(where: { $0.displayID == displayID }) else { return }
        handles.remove(at: index).destroy()
    }

    func returnAll() {
        let lent = handles
        handles = []
        for handle in lent {
            handle.destroy()
        }
    }

    /// The saved display state for this monitor (its resolution), else the automatic one — or the
    /// native mode when the window server does not offer the choice: a missing default must not
    /// end the session, and the native mode is always there.
    private static func activateInitialResolution(
        on handle: any VirtualDisplayHandle, for monitor: MonitorDescription, choice: HiDPIChoice,
        saved: SavedDisplayState?
    ) throws(VirtualDisplayError) -> DisplayResolution {
        let computed = initialResolution(for: monitor, choice: choice, configuration: handle.configuration)
        let native = handle.configuration.nativeResolution
        let attempts = saved.map { [$0.resolution, computed].filter { $0 != native } } ?? [computed]
        for resolution in attempts {
            do throws(VirtualDisplayError) {
                try handle.activate(resolution)
                return resolution
            } catch .resolutionUnavailable {
                continue  // not offered (the offered modes changed): the next attempt still works
            }
        }
        try handle.activate(native)
        return native
    }

    /// The user's choice for this viewer, or by default the viewer's own scaling: a monitor run
    /// at 100% shows the desktop at full size, a high-density one (150% and up) the crisp HiDPI
    /// resolution.
    static func initialResolution(
        for monitor: MonitorDescription, choice: HiDPIChoice, configuration: VirtualDisplayConfiguration
    ) -> DisplayResolution {
        let wantsHiDPI =
            switch choice {
            case .automatic: monitor.scalePercent >= Tunables.Host.hiDPIMinimumScalePercent
            case .always: true
            case .never: false
            }
        if wantsHiDPI, let crisp = configuration.crispHiDPIResolution {
            return crisp
        }
        return configuration.nativeResolution
    }

    private func configuration(
        _ monitor: MonitorDescription, index: Int, peerID: Data
    ) throws(VirtualDisplayError) -> VirtualDisplayConfiguration {
        try VirtualDisplayConfiguration(
            name: monitor.name.isEmpty ? String(localized: "host.displayName \(index + 1)") : monitor.name,
            identity: .derived(peerID: peerID, monitorIndex: Int(monitor.id)),
            pixelSize: DisplaySize(width: monitor.widthPixels, height: monitor.heightPixels),
            refreshRate: monitor.refreshRate,
            physicalSizeMillimeters: CGSize(
                width: monitor.physicalWidthMillimeters, height: monitor.physicalHeightMillimeters),
            isHiDPI: isHiDPI)
    }
}
