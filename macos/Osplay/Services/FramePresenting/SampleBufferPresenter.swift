import AVFoundation
import AppKit
import CoreBridge
import OSLog

/// Presents a stream with `AVSampleBufferDisplayLayer`: every decoded frame is shown immediately,
/// with no buffering for smoothness. A cursor layer on top draws separate-cursor streams.
@MainActor
final class SampleBufferPresenter: NSObject, FramePresenting {
    nonisolated let sink: any FrameSink
    private let stream: StreamConfiguration
    private let target: PresentationTarget
    private let window: NSWindow
    private let content: PresenterView
    private var cursorShape: CursorShape?
    private var cursorPosition: CursorPosition?
    private var idleTimer: Timer?

    init(stream: StreamConfiguration, target: PresentationTarget) {
        self.stream = stream
        self.target = target
        content = PresenterView(streamSize: CGSize(width: stream.width, height: stream.height))
        sink = RendererSink(renderer: content.displayLayer.sampleBufferRenderer)
        window = Self.makeWindow(target: target, stream: stream)
        super.init()
        window.contentView = content
    }

    func show() {
        if case .fullScreen = target {
            NSApp.activate()
            window.makeKeyAndOrderFront(nil)
            restartIdleTimer()
        } else {
            window.makeKeyAndOrderFront(nil)
        }
    }

    func hide() {
        idleTimer?.invalidate()
        window.orderOut(nil)
    }

    func close() {
        idleTimer?.invalidate()
        content.displayLayer.sampleBufferRenderer.flush(removingDisplayedImage: true, completionHandler: nil)
        window.close()
    }

    func setCursorShape(_ shape: CursorShape) {
        cursorShape = shape
        content.cursorLayer.contents = CursorImage.make(shape)
        layoutCursor()
    }

    func moveCursor(_ position: CursorPosition) {
        cursorPosition = position
        layoutCursor()
    }

    func showStats(_ figures: StatsFigures?) {
        guard let figures else {
            content.statsLabel.isHidden = true
            return
        }
        content.statsLabel.stringValue = figures.overlayText
        content.statsLabel.isHidden = false
        content.needsLayout = true
    }

    private func layoutCursor() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        guard let shape = cursorShape, let position = cursorPosition, position.isVisible,
            position.shapeID == shape.shapeID,
            let frame = CursorGeometry.frame(
                position: position, shape: shape, streamSize: content.streamSize, bounds: content.bounds)
        else {
            content.cursorLayer.isHidden = true
            return
        }
        content.cursorLayer.isHidden = false
        // CursorGeometry works top-down like the stream; layers count from the bottom.
        content.cursorLayer.frame = CGRect(
            x: frame.minX, y: content.bounds.height - frame.maxY, width: frame.width, height: frame.height)
    }

    /// The local pointer disappears after a short idle time over a full-screen viewer.
    private func restartIdleTimer() {
        idleTimer?.invalidate()
        idleTimer = Timer.scheduledTimer(withTimeInterval: Tunables.Viewer.cursorIdleDelay, repeats: false) { _ in
            MainActor.assumeIsolated { NSCursor.setHiddenUntilMouseMoves(true) }
        }
        content.onMouseMoved = { [weak self] in self?.restartIdleTimer() }
    }

    private static func makeWindow(target: PresentationTarget, stream: StreamConfiguration) -> NSWindow {
        switch target {
        case .fullScreen(let displayID):
            let screen = NSScreen.screens.first { $0.displayID == displayID } ?? NSScreen.main
            let frame = screen?.frame ?? .zero
            let window = NSWindow(
                contentRect: frame, styleMask: .borderless, backing: .buffered, defer: false, screen: screen)
            // Above the menu bar and the Dock of that screen, on every Space.
            window.level = .init(rawValue: NSWindow.Level.mainMenu.rawValue + 1)
            window.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle, .fullScreenAuxiliary]
            window.backgroundColor = NSColor(Theme.Viewer.background)
            window.setFrame(frame, display: false)
            window.acceptsMouseMovedEvents = true
            window.isReleasedWhenClosed = false
            return window
        case .window(let title):
            let size = Tunables.Viewer.loopbackWindowSize(streamWidth: stream.width, streamHeight: stream.height)
            let window = NSWindow(
                contentRect: CGRect(origin: .zero, size: size),
                styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered, defer: false)
            window.title = title
            window.contentAspectRatio = CGSize(width: stream.width, height: stream.height)
            window.backgroundColor = NSColor(Theme.Viewer.background)
            window.isReleasedWhenClosed = false
            window.center()
            return window
        }
    }
}

/// The window's content: the video layer and the cursor layer above it.
private final class PresenterView: NSView {
    let streamSize: CGSize
    let displayLayer = AVSampleBufferDisplayLayer()
    let cursorLayer = CALayer()
    /// The stats overlay in the top-left corner (Settings › Advanced).
    let statsLabel = NSTextField(labelWithString: "")
    var onMouseMoved: (() -> Void)?

    init(streamSize: CGSize) {
        self.streamSize = streamSize
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor(Theme.Viewer.background).cgColor
        displayLayer.videoGravity = .resizeAspect
        displayLayer.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        cursorLayer.isHidden = true
        cursorLayer.magnificationFilter = .nearest
        layer?.addSublayer(displayLayer)
        layer?.addSublayer(cursorLayer)
        statsLabel.isHidden = true
        statsLabel.font = NSFont.monospacedDigitSystemFont(
            ofSize: Theme.StatsOverlay.fontSize, weight: .medium)
        statsLabel.textColor = NSColor(Theme.StatsOverlay.text)
        statsLabel.wantsLayer = true
        statsLabel.layer?.backgroundColor = NSColor(Theme.StatsOverlay.background).cgColor
        statsLabel.layer?.cornerRadius = Theme.StatsOverlay.cornerRadius
        statsLabel.setAccessibilityLabel(String(localized: "stats.accessibilityLabel"))
        addSubview(statsLabel)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { nil }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        displayLayer.frame = bounds
        CATransaction.commit()
        let size = statsLabel.intrinsicContentSize
        let padding = Theme.StatsOverlay.padding
        let margin = Theme.StatsOverlay.margin
        statsLabel.frame = CGRect(
            x: margin, y: bounds.height - margin - size.height - 2 * padding,
            width: size.width + 2 * padding, height: size.height + 2 * padding)
        statsLabel.alignment = .center
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(
            NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self))
    }

    override func mouseMoved(with event: NSEvent) {
        onMouseMoved?()
    }
}

/// Creates layer-backed presenters.
@MainActor
final class SampleBufferPresenterFactory: FramePresenterMaking {
    func makePresenter(for stream: StreamConfiguration, target: PresentationTarget) -> any FramePresenting {
        SampleBufferPresenter(stream: stream, target: target)
    }
}
