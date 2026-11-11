import CoreBridge
import Foundation
import OSLog

/// The part of a core session the viewer uses; `CoreSession` in the app, a fake in tests.
protocol ViewerCoreSession: AnyObject, Sendable {
    var events: AsyncStream<SessionEvent> { get }
    func confirmPairing(_ accepted: Bool) throws(CoreError)
    func requestRoleSwap() throws(CoreError)
    func answerRoleSwap(_ answer: RoleSwapAnswer) throws(CoreError)
    func requestKeyframe(_ streamID: UInt8) throws(CoreError)
    func stopStream(_ streamID: UInt8) throws(CoreError)
    func pauseStream(_ streamID: UInt8) throws(CoreError)
    func resumeStream(_ streamID: UInt8) throws(CoreError)
    func close()
    func closeForSleep()
}

extension CoreSession: ViewerCoreSession {}

/// Owns one viewer session: the core session, a decoder and a presenter per stream, and the power
/// assertion while frames are on screen (DEVPLAN Architecture, `Features/Viewer`).
actor ViewerSession {
    enum Event: Sendable {
        case state(SessionState)
        /// The host introduced itself.
        case peer(PeerInfo)
        /// First contact: show the code; answer with `confirmPairing`.
        case pairingCode(UInt32, peerPublicKey: [UInt8])
        /// Both users accepted: store the key as trusted.
        case paired(peerPublicKey: [UInt8])
        case streamAdded(StreamConfiguration)
        case visibility(Bool)
        case statistics(SessionStatistics)
        /// The other computer asks to reverse the roles; answer with `answerRoleSwap`.
        case roleSwapRequested(peerPublicKey: [UInt8])
        /// The answer to `requestRoleSwap`; after `.accepted` the session closes.
        case roleSwapAnswered(RoleSwapAnswer, peerPublicKey: [UInt8])
    }

    nonisolated let events: AsyncStream<Event>
    private let eventSink: AsyncStream<Event>.Continuation
    private let core: any ViewerCoreSession
    private let decoders: any VideoDecoderMaking
    private let presenters: any FramePresenterMaking
    private let power: any PowerAssertionManaging
    /// Where a stream is shown; nil for a monitor this Mac did not offer, which the host then stops.
    private let target: @Sendable (StreamConfiguration) -> PresentationTarget?
    /// Read with every statistics update, so the setting applies at once.
    private let showsStats: @Sendable () async -> Bool
    private var overlaysShown = false
    private var streams: [UInt8: (pipeline: ViewerStreamPipeline, presenter: any FramePresenting)] = [:]
    private var visible = true
    private var assertion: (any PowerAssertion)?
    private var failure: CoreError?

    init(
        core: any ViewerCoreSession, decoders: any VideoDecoderMaking, presenters: any FramePresenterMaking,
        power: any PowerAssertionManaging, target: @escaping @Sendable (StreamConfiguration) -> PresentationTarget?,
        showsStats: @escaping @Sendable () async -> Bool = { false }
    ) {
        self.showsStats = showsStats
        self.core = core
        self.decoders = decoders
        self.presenters = presenters
        self.power = power
        self.target = target
        (events, eventSink) = AsyncStream.makeStream(of: Event.self)
    }

    /// Runs until the session closes; returns the reason when it was an error.
    func run() async -> CoreError? {
        defer { eventSink.finish() }
        for await event in core.events {
            switch event {
            case .streamConfiguration(let stream):
                await add(stream)
            case .accessUnit(let unit):
                streams[unit.streamID]?.pipeline.decode(unit)
            case .cursorShape(let shape):
                if let presenter = streams[shape.streamID]?.presenter {
                    await presenter.setCursorShape(shape)
                }
            case .cursorPosition(let position):
                if let presenter = streams[position.streamID]?.presenter {
                    await presenter.moveCursor(position)
                }
            case .state(let state, let reason):
                eventSink.yield(.state(state))
                if state == .closed {
                    await tearDown()
                    core.close()  // releases the core session also after closeForSleep
                    if let reason, reason != .connectionClosed {
                        failure = reason
                    }
                    return failure
                }
            case .statistics(let statistics):
                eventSink.yield(.statistics(statistics))
                await updateOverlays(statistics)
            case .log(let level, let message):
                log(level, message)
            case .streamStopped(let streamID):
                if let entry = streams.removeValue(forKey: streamID) {
                    entry.pipeline.stop()
                    await entry.presenter.close()
                    updatePowerAssertion()
                }
            case .peer(let peer):
                eventSink.yield(.peer(peer))
            case .pairingCode(let code, let key):
                eventSink.yield(.pairingCode(code, peerPublicKey: key))
            case .paired(let key):
                eventSink.yield(.paired(peerPublicKey: key))
            case .roleSwapRequested(let key):
                eventSink.yield(.roleSwapRequested(peerPublicKey: key))
            case .roleSwapAnswered(let answer, let key):
                eventSink.yield(.roleSwapAnswered(answer, peerPublicKey: key))
            case .viewerMonitors, .keyframeRequested, .bitrateTarget:
                break  // host-only
            }
        }
        await tearDown()
        return failure
    }

    private func updateOverlays(_ statistics: SessionStatistics) async {
        let shows = await showsStats()
        guard shows || overlaysShown else { return }
        overlaysShown = shows
        for stream in statistics.streams {
            guard let presenter = streams[stream.streamID]?.presenter else { continue }
            await presenter.showStats(
                shows ? StatsFigures(stream: stream, roundTripMicroseconds: statistics.roundTripMicroseconds) : nil)
        }
    }

    /// Hides or shows every stream. Hidden streams are paused, so the host stops sending.
    func setVisible(_ visible: Bool) async {
        guard visible != self.visible else { return }
        self.visible = visible
        for streamID in streams.keys.sorted() {
            guard let entry = streams[streamID] else { continue }
            do throws(CoreError) {
                try visible ? core.resumeStream(streamID) : core.pauseStream(streamID)
            } catch {
                Log.viewer.error(
                    "Pause/resume of stream \(streamID) failed: \(String(describing: error), privacy: .public)")
            }
            if visible {
                await entry.presenter.show()
            } else {
                await entry.presenter.hide()
            }
        }
        updatePowerAssertion()
        eventSink.yield(.visibility(visible))
    }

    func toggleVisibility() async {
        await setVisible(!visible)
    }

    /// (fork) Toggles each display between windowed and fullscreen (own Space).
    func toggleFullScreen() {
        for (_, stream) in streams {
            Task { @MainActor in stream.presenter.toggle_fullscreen() }
        }
    }

    var isVisible: Bool { visible }

    /// Ends the session with Bye; `run` returns once the core reports `.closed`.
    func stop() {
        core.close()
    }

    /// Ends the session telling the other computer this Mac is going to sleep (it waits for it).
    func stopForSleep() {
        core.closeForSleep()
    }

    /// Answers `.pairingCode` with the local user's decision.
    func confirmPairing(_ accepted: Bool) {
        do throws(CoreError) {
            try core.confirmPairing(accepted)
        } catch {
            Log.viewer.error("Pairing answer failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// Asks the other computer to reverse the roles; throws when it cannot be asked now.
    func requestRoleSwap() throws(CoreError) {
        try core.requestRoleSwap()
    }

    /// Answers `.roleSwapRequested`; `.accepted` closes the session.
    func answerRoleSwap(_ answer: RoleSwapAnswer) {
        do throws(CoreError) {
            try core.answerRoleSwap(answer)
        } catch {
            Log.viewer.error("Role swap answer failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// These offered displays went away (unplugged): their streams end here, and the host stops
    /// only those; the other displays keep streaming.
    func displaysRemoved(_ monitorIDs: Set<UInt8>) async {
        let removed = streams.filter { monitorIDs.contains($0.value.pipeline.stream.monitorID) }.keys.sorted()
        for streamID in removed {
            guard let entry = streams.removeValue(forKey: streamID) else { continue }
            entry.pipeline.stop()
            await entry.presenter.close()
            stopOnHost(streamID)
        }
        updatePowerAssertion()
    }

    func statistics() -> [UInt8: ViewerStreamStatistics] {
        streams.mapValues(\.pipeline.currentStatistics)
    }

    /// Debug: damages the next access unit of every stream to exercise recovery.
    func corruptNextFrames() {
        streams.values.forEach { $0.pipeline.corruptNextFrame() }
    }

    private func add(_ stream: StreamConfiguration) async {
        if let existing = streams.removeValue(forKey: stream.streamID) {
            existing.pipeline.stop()
            await existing.presenter.close()
        }
        guard let place = target(stream) else {
            Log.viewer.error(
                "Stream \(stream.streamID) is for monitor \(stream.monitorID), which this Mac did not offer")
            stopOnHost(stream.streamID)
            return
        }
        let presenter = await presenters.makePresenter(for: stream, target: place)
        let pipeline = ViewerStreamPipeline(
            stream: stream, decoders: decoders, sink: presenter.sink,
            needsKeyframe: { [weak self] streamID in
                Task { await self?.requestKeyframe(streamID) }
            })
        streams[stream.streamID] = (pipeline, presenter)
        if visible {
            await presenter.show()
        }
        updatePowerAssertion()
        eventSink.yield(.streamAdded(stream))
    }

    private func stopOnHost(_ streamID: UInt8) {
        do throws(CoreError) {
            try core.stopStream(streamID)
        } catch {
            Log.viewer.error("Stopping stream \(streamID) failed: \(String(describing: error), privacy: .public)")
        }
    }

    private func requestKeyframe(_ streamID: UInt8) {
        do throws(CoreError) {
            try core.requestKeyframe(streamID)
        } catch {
            Log.viewer.info(
                "Keyframe request for stream \(streamID) not sent: \(String(describing: error), privacy: .public)")
        }
    }

    private func updatePowerAssertion() {
        let needed = visible && !streams.isEmpty
        if needed, assertion == nil {
            assertion = power.preventDisplaySleep(reason: Tunables.Viewer.powerAssertionReason)
        } else if !needed, let current = assertion {
            current.release()
            assertion = nil
        }
    }

    private func tearDown() async {
        let entries = streams.keys.sorted().compactMap { streams[$0] }
        streams.removeAll()
        for entry in entries {
            entry.pipeline.stop()
            await entry.presenter.close()
        }
        updatePowerAssertion()
    }

    private func log(_ level: SessionLogLevel, _ message: String) {
        switch level {
        case .debug: Log.viewer.debug("core: \(message, privacy: .public)")
        case .info: Log.viewer.info("core: \(message, privacy: .public)")
        case .warning: Log.viewer.warning("core: \(message, privacy: .public)")
        case .error: Log.viewer.error("core: \(message, privacy: .public)")
        }
    }
}
