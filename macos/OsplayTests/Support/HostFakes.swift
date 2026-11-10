import CoreBridge
import CoreGraphics
import CoreMedia
import CoreVideo
import Foundation

@testable import Osplay

/// A core session driven by the test: `emit` plays the core's role, `close` answers like the core.
final class FakeHostCore: HostCoreSession, @unchecked Sendable {
    let events: AsyncStream<SessionEvent>
    private let continuation: AsyncStream<SessionEvent>.Continuation
    private let lock = NSLock()
    private var configured: [[StreamConfiguration]] = []
    private var pushed: [AccessUnit] = []
    private var closed = false
    private var pairingAnswers: [Bool] = []
    var configureError: CoreError?

    var answers: [Bool] { lock.withLock { pairingAnswers } }

    func confirmPairing(_ accepted: Bool) throws(CoreError) { lock.withLock { pairingAnswers.append(accepted) } }

    private var swapRequests = 0
    private var swapAnswers: [RoleSwapAnswer] = []
    var roleSwapRequests: Int { lock.withLock { swapRequests } }
    var roleSwapAnswers: [RoleSwapAnswer] { lock.withLock { swapAnswers } }
    func requestRoleSwap() throws(CoreError) { lock.withLock { swapRequests += 1 } }
    func answerRoleSwap(_ answer: RoleSwapAnswer) throws(CoreError) { lock.withLock { swapAnswers.append(answer) } }

    init() {
        (events, continuation) = AsyncStream.makeStream(of: SessionEvent.self)
    }

    func emit(_ event: SessionEvent) { continuation.yield(event) }

    func configureStreams(_ streams: [StreamConfiguration]) throws(CoreError) {
        if let configureError { throw configureError }
        lock.withLock { configured.append(streams) }
    }

    func push(_ unit: AccessUnit) throws(CoreError) {
        lock.lock()
        defer { lock.unlock() }
        guard !closed else { throw .invalidState }
        pushed.append(unit)
    }

    func closeForSleep() { close() }

    private var refused = false
    var refusedBusy: Bool { lock.withLock { refused } }
    func refuseBusy() {
        lock.withLock { refused = true }
        close()
    }

    func close() {
        let first = lock.withLock {
            defer { closed = true }
            return !closed
        }
        guard first else { return }
        continuation.yield(.state(.closed, reason: nil))
        continuation.finish()
    }

    var configuredStreams: [[StreamConfiguration]] { lock.withLock { configured } }
    var pushedUnits: [AccessUnit] { lock.withLock { pushed } }
    var isClosed: Bool { lock.withLock { closed } }
}

@MainActor
final class FakeDisplayLender: DisplayLending {
    var error: VirtualDisplayError?
    private(set) var lentMonitors: [[MonitorDescription]] = []
    private(set) var returnCount = 0
    private(set) var isLending = false

    private(set) var hiDPIChoices: [HiDPIChoice] = []

    func lend(
        _ monitors: [MonitorDescription], peerID: Data, hiDPI: HiDPIChoice, arrangement: [ArrangedMonitor],
        saved: [UInt8: SavedDisplayState]
    ) async throws(VirtualDisplayError) -> [LentDisplay] {
        hiDPIChoices.append(hiDPI)
        if let error { throw error }
        lentMonitors.append(monitors)
        isLending = true
        return monitors.enumerated().map { LentDisplay(monitor: $1, displayID: CGDirectDisplayID(200 + $0)) }
    }

    private(set) var returnedDisplays: [CGDirectDisplayID] = []

    func returnDisplay(_ displayID: CGDirectDisplayID) {
        returnedDisplays.append(displayID)
    }

    func returnAll() {
        returnCount += 1
        isLending = false
    }
}

final class FakeCapture: ScreenCapturing, @unchecked Sendable {
    let configuration: CaptureConfiguration
    private let queue: DispatchQueue
    private let output: CaptureOutput
    private let lock = NSLock()
    private var running = false

    init(_ configuration: CaptureConfiguration, queue: DispatchQueue, output: @escaping CaptureOutput) {
        self.configuration = configuration
        self.queue = queue
        self.output = output
    }

    func start() async throws(ScreenCaptureError) { lock.withLock { running = true } }
    func stop() async { lock.withLock { running = false } }
    var isRunning: Bool { lock.withLock { running } }

    /// Delivers a frame on the capture queue, as ScreenCaptureKit does.
    func deliverFrame(at seconds: Double) {
        let frame = CapturedFrame(
            pixelBuffer: makePixelBuffer(), presentationTime: CMTime(seconds: seconds, preferredTimescale: 1_000_000))
        queue.sync { output(.frame(frame)) }
    }

    func fail(_ error: ScreenCaptureError) {
        queue.sync { output(.stopped(error)) }
    }
}

final class FakeCaptureFactory: ScreenCaptureMaking, @unchecked Sendable {
    private let lock = NSLock()
    private var made: [FakeCapture] = []

    func makeCapture(
        _ configuration: CaptureConfiguration, queue: DispatchQueue, output: @escaping CaptureOutput
    ) -> any ScreenCapturing {
        let capture = FakeCapture(configuration, queue: queue, output: output)
        lock.withLock { made.append(capture) }
        return capture
    }

    var captures: [FakeCapture] { lock.withLock { made } }
}

/// Encodes synchronously: every frame becomes a 4-byte unit, keyframes when forced.
final class FakeEncoder: VideoEncoding, @unchecked Sendable {
    let configuration: EncoderConfiguration
    private let output: EncoderOutput
    private let lock = NSLock()
    private var forced: [Bool] = []
    private var invalidated = false

    init(_ configuration: EncoderConfiguration, output: @escaping EncoderOutput) {
        self.configuration = configuration
        self.output = output
    }

    func encode(_ pixelBuffer: CVPixelBuffer, presentationTime: CMTime, forceKeyframe: Bool) throws(VideoEncoderError) {
        lock.lock()
        guard !invalidated else {
            lock.unlock()
            throw .invalidated
        }
        forced.append(forceKeyframe)
        lock.unlock()
        output(
            .success(
                EncodedFrame(
                    data: Data(forceKeyframe ? [0, 0, 0, 1, 0x67] : [0, 0, 0, 1, 0x41]), isKeyframe: forceKeyframe,
                    captureTimeMicroseconds: UInt64(max(presentationTime.seconds, 0) * 1_000_000))))
    }

    private var bitrates: [Int] = []
    func setBitrate(kbps: Int) { lock.withLock { bitrates.append(kbps) } }
    var bitrateChanges: [Int] { lock.withLock { bitrates } }

    func flush() {}
    func invalidate() { lock.withLock { invalidated = true } }

    var forcedKeyframes: [Bool] { lock.withLock { forced } }
    var isInvalidated: Bool { lock.withLock { invalidated } }
}

final class FakeEncoderFactory: VideoEncoderMaking, @unchecked Sendable {
    private let lock = NSLock()
    private var made: [FakeEncoder] = []

    func makeEncoder(
        _ configuration: EncoderConfiguration, output: @escaping EncoderOutput
    ) throws(VideoEncoderError)
        -> any VideoEncoding
    {
        let encoder = FakeEncoder(configuration, output: output)
        lock.withLock { made.append(encoder) }
        return encoder
    }

    var encoders: [FakeEncoder] { lock.withLock { made } }
}

struct FakePermissions: PermissionChecking {
    var hasScreenRecordingAccess = true
    func requestScreenRecordingAccess() -> Bool { hasScreenRecordingAccess }
    var screenRecordingSettingsURL: URL? { nil }
    var localNetworkSettingsURL: URL? { nil }
}

private func makePixelBuffer() -> CVPixelBuffer {
    var buffer: CVPixelBuffer?
    CVPixelBufferCreate(nil, 16, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nil, &buffer)
    guard let buffer else { fatalError("CVPixelBufferCreate failed for a 16×16 test buffer") }
    return buffer
}
