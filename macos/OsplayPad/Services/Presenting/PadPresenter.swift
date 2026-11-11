import AVFoundation
import CoreBridge
import UIKit

/// Presents the stream in a view the viewer screen embeds: every decoded frame is shown at once,
/// and a cursor layer on top draws separate-cursor streams (Windows hosts).
@MainActor
final class PadPresenter: FramePresenting {
    nonisolated let sink: any FrameSink
    let view: PadPresenterView
    private let onClose: () -> Void
    private var cursorShape: CursorShape?
    private var cursorPosition: CursorPosition?

    init(stream: StreamConfiguration, onClose: @escaping () -> Void) {
        view = PadPresenterView(streamSize: CGSize(width: stream.width, height: stream.height))
        sink = RendererSink(renderer: view.displayLayer.sampleBufferRenderer)
        self.onClose = onClose
        view.onLayout = { [weak self] in self?.layoutCursor() }
    }

    func show() {
        view.isHidden = false
    }

    func hide() {
        view.isHidden = true
    }

    func close() {
        view.displayLayer.sampleBufferRenderer.flush(removingDisplayedImage: true, completionHandler: nil)
        onClose()
    }

    /// (fork) iPad has no Space concept; fullscreen toggle is a no-op.
    func toggleFullScreen() { }

    func setCursorShape(_ shape: CursorShape) {
        cursorShape = shape
        view.cursorLayer.contents = CursorImage.make(shape)
        layoutCursor()
    }

    func moveCursor(_ position: CursorPosition) {
        cursorPosition = position
        layoutCursor()
    }

    func showStats(_ figures: StatsFigures?) {
        view.statsLabel.text = figures.map { " \($0.overlayText) " }
        view.statsLabel.isHidden = figures == nil
        view.setNeedsLayout()
    }

    private func layoutCursor() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        guard let shape = cursorShape, let position = cursorPosition, position.isVisible,
            position.shapeID == shape.shapeID,
            let frame = CursorGeometry.frame(
                position: position, shape: shape, streamSize: view.streamSize, bounds: view.bounds)
        else {
            view.cursorLayer.isHidden = true
            return
        }
        // UIKit counts from the top like the stream, so the frame applies as it is.
        view.cursorLayer.isHidden = false
        view.cursorLayer.frame = frame
    }
}

/// The video layer, the cursor layer above it, and the statistics label.
final class PadPresenterView: UIView {
    let streamSize: CGSize
    let displayLayer = AVSampleBufferDisplayLayer()
    let cursorLayer = CALayer()
    let statsLabel = UILabel()
    var onLayout: (() -> Void)?

    init(streamSize: CGSize) {
        self.streamSize = streamSize
        super.init(frame: .zero)
        backgroundColor = UIColor(Theme.Viewer.background)
        displayLayer.videoGravity = .resizeAspect
        cursorLayer.isHidden = true
        cursorLayer.magnificationFilter = .nearest
        layer.addSublayer(displayLayer)
        layer.addSublayer(cursorLayer)
        statsLabel.isHidden = true
        statsLabel.font = .monospacedDigitSystemFont(ofSize: Theme.StatsOverlay.fontSize, weight: .medium)
        statsLabel.textColor = Theme.StatsOverlay.text
        statsLabel.backgroundColor = Theme.StatsOverlay.background
        statsLabel.layer.cornerRadius = Theme.StatsOverlay.cornerRadius
        statsLabel.layer.masksToBounds = true
        statsLabel.accessibilityLabel = String(localized: "stats.accessibilityLabel")
        addSubview(statsLabel)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { nil }

    override func layoutSubviews() {
        super.layoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        displayLayer.frame = bounds
        CATransaction.commit()
        let size = statsLabel.intrinsicContentSize
        let padding = Theme.StatsOverlay.padding
        let margin = Theme.StatsOverlay.margin
        statsLabel.frame = CGRect(
            x: safeAreaInsets.left + margin, y: safeAreaInsets.top + margin, width: size.width + 2 * padding,
            height: size.height + 2 * padding)
        statsLabel.textAlignment = .center
        onLayout?()
    }
}

/// Creates the presenter and tells the viewer screen which view to show.
@MainActor
final class PadPresenterFactory: FramePresenterMaking {
    /// The stream's view, or nil when it closed.
    var onStageChange: ((PadPresenterView?) -> Void)?

    func makePresenter(for stream: StreamConfiguration, target: PresentationTarget) -> any FramePresenting {
        weak var created: PadPresenter?
        let presenter = PadPresenter(stream: stream) { [weak self] in
            // Only the current stream's view leaves the screen.
            self?.closed(created?.view)
        }
        created = presenter
        current = presenter.view
        onStageChange?(presenter.view)
        return presenter
    }

    private weak var current: PadPresenterView?

    private func closed(_ view: PadPresenterView?) {
        guard let view, view === current else { return }
        current = nil
        onStageChange?(nil)
    }
}
