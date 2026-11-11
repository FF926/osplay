import CoreBridge
import CoreVideo
import Foundation

@testable import Osplay

final class FakeViewerCore: ViewerCoreSession, @unchecked Sendable {
    let events: AsyncStream<SessionEvent>
    private let continuation: AsyncStream<SessionEvent>.Continuation
    private let lock = NSLock()
    private var keyframeRequestList: [UInt8] = []
    private var pausedList: [UInt8] = []
    private var resumedList: [UInt8] = []
    private var stoppedList: [UInt8] = []
    private var closed = false
    private var pairingAnswers: [Bool] = []

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

    func requestKeyframe(_ streamID: UInt8) throws(CoreError) { lock.withLock { keyframeRequestList.append(streamID) } }
    func stopStream(_ streamID: UInt8) throws(CoreError) { lock.withLock { stoppedList.append(streamID) } }
    func pauseStream(_ streamID: UInt8) throws(CoreError) { lock.withLock { pausedList.append(streamID) } }
    func resumeStream(_ streamID: UInt8) throws(CoreError) { lock.withLock { resumedList.append(streamID) } }

    func closeForSleep() { close() }

    func close() {
        let first = lock.withLock {
            defer { closed = true }
            return !closed
        }
        guard first else { return }
        continuation.yield(.state(.closed, reason: nil))
        continuation.finish()
    }

    var keyframeRequests: [UInt8] { lock.withLock { keyframeRequestList } }
    var paused: [UInt8] { lock.withLock { pausedList } }
    var resumed: [UInt8] { lock.withLock { resumedList } }
    var stopped: [UInt8] { lock.withLock { stoppedList } }
}

/// Decodes synchronously: data starting with 0xBA fails, anything else yields a frame.
final class FakeDecoder: VideoDecoding, @unchecked Sendable {
    static let corrupt: UInt8 = 0xBA
    private let output: DecoderOutput
    private let lock = NSLock()
    private var invalidated = false

    init(output: @escaping DecoderOutput) { self.output = output }

    func decode(_ unit: AccessUnit) {
        guard !lock.withLock({ invalidated }) else { return }
        if unit.data.first == Self.corrupt {
            output(.failure(.decodeFailed(-12909)))
        } else {
            output(
                .success(
                    DecodedFrame(
                        streamID: unit.streamID, pixelBuffer: makeViewerPixelBuffer(),
                        captureTimeMicroseconds: unit.captureTimeMicroseconds)))
        }
    }

    func invalidate() { lock.withLock { invalidated = true } }
    var isInvalidated: Bool { lock.withLock { invalidated } }
}

final class FakeDecoderFactory: VideoDecoderMaking, @unchecked Sendable {
    private let lock = NSLock()
    private var made: [FakeDecoder] = []

    func makeDecoder(output: @escaping DecoderOutput) -> any VideoDecoding {
        let decoder = FakeDecoder(output: output)
        lock.withLock { made.append(decoder) }
        return decoder
    }

    var decoders: [FakeDecoder] { lock.withLock { made } }
}

final class RecordingSink: FrameSink, @unchecked Sendable {
    private let lock = NSLock()
    private var times: [UInt64] = []
    func enqueue(_ frame: DecodedFrame) { lock.withLock { times.append(frame.captureTimeMicroseconds) } }
    var frames: [UInt64] { lock.withLock { times } }
}

@MainActor
final class FakePresenter: FramePresenting {
    nonisolated let sink: any FrameSink
    let recording = RecordingSink()
    let stream: StreamConfiguration
    let target: PresentationTarget
    private(set) var isShown = false
    private(set) var isClosed = false
    private(set) var cursorShapes: [UInt32] = []
    private(set) var cursorPositions: [CursorPosition] = []

    init(stream: StreamConfiguration, target: PresentationTarget) {
        self.stream = stream
        self.target = target
        sink = recording
    }

    func show() { isShown = true }
    func hide() { isShown = false }
    func close() {
        isShown = false
        isClosed = true
    }
    func toggleFullScreen() {}
    func setCursorShape(_ shape: CursorShape) { cursorShapes.append(shape.shapeID) }
    func moveCursor(_ position: CursorPosition) { cursorPositions.append(position) }
    private(set) var stats: [StatsFigures?] = []
    func showStats(_ figures: StatsFigures?) { stats.append(figures) }
}

@MainActor
final class FakePresenterFactory: FramePresenterMaking {
    private(set) var presenters: [FakePresenter] = []

    func makePresenter(for stream: StreamConfiguration, target: PresentationTarget) -> any FramePresenting {
        let presenter = FakePresenter(stream: stream, target: target)
        presenters.append(presenter)
        return presenter
    }
}

final class FakePower: PowerAssertionManaging, @unchecked Sendable {
    private let lock = NSLock()
    private var activeCount = 0

    func preventDisplaySleep(reason: String) -> any PowerAssertion {
        lock.withLock { activeCount += 1 }
        return Assertion { [weak self] in self?.decrement() }
    }

    private func decrement() { lock.withLock { activeCount -= 1 } }

    var active: Int { lock.withLock { activeCount } }

    private final class Assertion: PowerAssertion, @unchecked Sendable {
        private let lock = NSLock()
        private var onRelease: (() -> Void)?
        init(onRelease: @escaping () -> Void) { self.onRelease = onRelease }
        func release() {
            let action = lock.withLock {
                defer { onRelease = nil }
                return onRelease
            }
            action?()
        }
    }
}

func makeViewerPixelBuffer() -> CVPixelBuffer {
    var buffer: CVPixelBuffer?
    CVPixelBufferCreate(nil, 16, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nil, &buffer)
    guard let buffer else { fatalError("CVPixelBufferCreate failed for a 16×16 test buffer") }
    return buffer
}
