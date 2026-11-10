import CoreGraphics
import Foundation

/// One of a viewer's monitors in this Mac's display arrangement. `origin` is relative to the top-left
/// corner of this Mac's main display, in points (y grows downwards, as in Quartz).
struct ArrangedMonitor: Codable, Hashable, Sendable, Identifiable {
    let id: UInt8
    var name: String
    var size: CGSize
    var origin: CGPoint

    var frame: CGRect { CGRect(origin: origin, size: size) }
}

/// (fork) Where one of a viewer's virtual displays ended: its final resolution and its origin
/// next to this Mac's main display. The next session for the same viewer starts there, so a
/// resolution or position the user picked in System Settings survives reconnects and swaps.
struct SavedDisplayState: Codable, Hashable, Sendable {
    let resolution: DisplayResolution
    let origin: CGPoint
}

/// Keeps an arrangement valid the way System Settings does: displays never overlap, and a dropped
/// display ends up touching the main display or another display along an edge.
enum DisplayLayout {
    /// Within this distance a dropped display's edge lines up with a neighbour's (points).
    static let alignmentSnap: CGFloat = 24

    /// Where `moving` lands when dropped among `fixed` (the main display and the other monitors).
    static func snap(_ moving: CGRect, among fixed: [CGRect]) -> CGRect {
        guard !fixed.isEmpty else { return moving }
        let candidates = fixed.flatMap { attachments(of: moving, to: $0) }
            .filter { candidate in !fixed.contains { $0.intersects(candidate.insetBy(dx: 0.5, dy: 0.5)) } }
        guard
            let best = candidates.min(by: { distance($0.origin, moving.origin) < distance($1.origin, moving.origin) })
        else { return moving }
        return aligned(best, among: fixed)
    }

    /// The default row: every monitor to the right of the main display, tops aligned, in the
    /// viewer's order (left to right on the viewer).
    static func defaultArrangement(
        for monitors: [(id: UInt8, name: String, size: CGSize)], main: CGSize
    ) -> [ArrangedMonitor] {
        var x = main.width
        return monitors.map { monitor in
            defer { x += monitor.size.width }
            return ArrangedMonitor(id: monitor.id, name: monitor.name, size: monitor.size, origin: CGPoint(x: x, y: 0))
        }
    }

    /// `saved` when it describes exactly these monitors at these sizes, otherwise the default row.
    static func arrangement(
        for monitors: [(id: UInt8, name: String, size: CGSize)], saved: [ArrangedMonitor], main: CGSize
    ) -> [ArrangedMonitor] {
        let matches =
            saved.count == monitors.count
            && monitors.allSatisfy { monitor in saved.contains { $0.id == monitor.id && $0.size == monitor.size } }
        guard matches else { return defaultArrangement(for: monitors, main: main) }
        return monitors.compactMap { monitor in
            saved.first { $0.id == monitor.id }.map { saved in
                var arranged = saved
                arranged.name = monitor.name
                return arranged
            }
        }
    }

    /// The four positions where `moving` touches `target`, sliding along the shared edge as little
    /// as needed to keep the edges overlapping.
    private static func attachments(of moving: CGRect, to target: CGRect) -> [CGRect] {
        let x = min(max(moving.minX, target.minX - moving.width + 1), target.maxX - 1)
        let y = min(max(moving.minY, target.minY - moving.height + 1), target.maxY - 1)
        return [
            CGRect(origin: CGPoint(x: target.maxX, y: y), size: moving.size),
            CGRect(origin: CGPoint(x: target.minX - moving.width, y: y), size: moving.size),
            CGRect(origin: CGPoint(x: x, y: target.minY - moving.height), size: moving.size),
            CGRect(origin: CGPoint(x: x, y: target.maxY), size: moving.size),
        ]
    }

    /// Lines the rectangle's edges up with a neighbour's when they are nearly level.
    private static func aligned(_ rect: CGRect, among fixed: [CGRect]) -> CGRect {
        var result = rect
        for other in fixed {
            let touchingSide = rect.minX == other.maxX || rect.maxX == other.minX
            let touchingTopOrBottom = rect.minY == other.maxY || rect.maxY == other.minY
            if touchingSide {
                for (edge, target) in [(rect.minY, other.minY), (rect.maxY, other.maxY)]
                where abs(edge - target) < alignmentSnap {
                    let moved = result.offsetBy(dx: 0, dy: target - edge)
                    if !fixed.contains(where: { $0.intersects(moved.insetBy(dx: 0.5, dy: 0.5)) }) { result = moved }
                    break
                }
            }
            if touchingTopOrBottom {
                for (edge, target) in [(rect.minX, other.minX), (rect.maxX, other.maxX)]
                where abs(edge - target) < alignmentSnap {
                    let moved = result.offsetBy(dx: target - edge, dy: 0)
                    if !fixed.contains(where: { $0.intersects(moved.insetBy(dx: 0.5, dy: 0.5)) }) { result = moved }
                    break
                }
            }
        }
        return result
    }

    private static func distance(_ a: CGPoint, _ b: CGPoint) -> CGFloat {
        hypot(a.x - b.x, a.y - b.y)
    }
}
