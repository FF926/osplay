import CoreGraphics
import Foundation

/// A width and height in whole pixels or points.
struct DisplaySize: Hashable, Sendable, Codable, CustomStringConvertible {
    let width: Int
    let height: Int

    var description: String { "\(width)×\(height)" }

    func scaled(by factor: Double) -> DisplaySize {
        DisplaySize(width: Self.evenRounded(Double(width) * factor), height: Self.evenRounded(Double(height) * factor))
    }

    // Video encoders and the window server both prefer even dimensions.
    private static func evenRounded(_ value: Double) -> Int {
        Int((value / 2).rounded()) * 2
    }
}

/// The identity macOS uses to remember a display's arrangement and chosen resolution.
///
/// It must be stable for the same remote monitor across reconnects, so it is derived from the
/// peer's identity and the remote monitor index instead of being random.
struct VirtualDisplayIdentity: Hashable, Sendable {
    let vendorID: UInt32
    let productID: UInt32
    let serialNumber: UInt32

    /// "OSP" packed as an EDID manufacturer ID (three 5-bit letters). Not a registered PNP ID.
    static let osplayVendorID: UInt32 = 0x3E70
    static let osplayProductID: UInt32 = 0x0001

    static func derived(peerID: Data, monitorIndex: Int) -> VirtualDisplayIdentity {
        var bytes = [UInt8](peerID)
        withUnsafeBytes(of: UInt32(truncatingIfNeeded: monitorIndex).littleEndian) { bytes.append(contentsOf: $0) }
        let hash = fnv1a32(bytes)
        // Serial 0 means "no serial number" to the window server, which would merge identities.
        return VirtualDisplayIdentity(
            vendorID: osplayVendorID,
            productID: osplayProductID,
            serialNumber: hash == 0 ? 1 : hash
        )
    }

    private static func fnv1a32(_ bytes: [UInt8]) -> UInt32 {
        bytes.reduce(UInt32(2_166_136_261)) { ($0 ^ UInt32($1)) &* 16_777_619 }
    }
}

/// A resolution the user can choose for a display: what it "looks like" in points, and the
/// pixel backing the window server renders (twice the points for HiDPI modes).
struct DisplayResolution: Hashable, Sendable, Codable {
    let points: DisplaySize
    let pixels: DisplaySize
    let refreshRate: Double

    var isHiDPI: Bool { pixels.width > points.width }
}

/// Everything needed to create one virtual display matching a remote monitor.
struct VirtualDisplayConfiguration: Hashable, Sendable {
    let name: String
    let identity: VirtualDisplayIdentity
    /// Physical pixel size of the remote monitor; the stream is always captured at this size.
    let pixelSize: DisplaySize
    let refreshRate: Double
    let physicalSizeMillimeters: CGSize
    let isHiDPI: Bool

    init(
        name: String,
        identity: VirtualDisplayIdentity,
        pixelSize: DisplaySize,
        refreshRate: Double,
        physicalSizeMillimeters: CGSize,
        isHiDPI: Bool
    ) throws(VirtualDisplayError) {
        let limits = Tunables.VirtualDisplay.self
        guard limits.pixelWidthRange.contains(pixelSize.width), limits.pixelHeightRange.contains(pixelSize.height)
        else { throw .invalidConfiguration(.pixelSizeOutOfRange) }
        guard limits.refreshRateRange.contains(refreshRate) else {
            throw .invalidConfiguration(.refreshRateOutOfRange)
        }
        guard physicalSizeMillimeters.width > 0, physicalSizeMillimeters.height > 0 else {
            throw .invalidConfiguration(.physicalSizeInvalid)
        }
        guard !name.isEmpty else { throw .invalidConfiguration(.nameEmpty) }

        self.name = name
        self.identity = identity
        self.pixelSize = pixelSize
        self.refreshRate = refreshRate
        self.physicalSizeMillimeters = physicalSizeMillimeters
        self.isHiDPI = isHiDPI
    }

    /// Largest backing the display may render. HiDPI needs room for a 2× backing of the native size.
    var maximumPixelSize: DisplaySize {
        isHiDPI ? pixelSize.scaled(by: 2) : pixelSize
    }

    /// Modes handed to the window server. Each one becomes a 1× resolution and, when HiDPI is on and
    /// the backing fits `maximumPixelSize`, a 2× "looks like" resolution of the same point size.
    /// The half-size mode is exact whenever it can be, so its 2× backing equals the monitor.
    var modes: [DisplaySize] {
        var seen = Set<DisplaySize>()
        return Tunables.VirtualDisplay.looksLikeScaleFactors
            .map { factor in factor == 0.5 ? exactHalf ?? pixelSize.scaled(by: factor) : pixelSize.scaled(by: factor) }
            .filter { seen.insert($0).inserted }
    }

    /// One point per pixel at the remote monitor's size: what a monitor at 100% scaling shows.
    var nativeResolution: DisplayResolution {
        DisplayResolution(points: pixelSize, pixels: pixelSize, refreshRate: refreshRate)
    }

    /// The HiDPI resolution whose backing equals the remote monitor exactly: sharpest text and no
    /// scaling during capture. The window server's own default is the largest 2× mode, which
    /// renders four times as many pixels as the stream carries.
    var crispHiDPIResolution: DisplayResolution? {
        guard isHiDPI, let half = exactHalf else { return nil }
        return DisplayResolution(points: half, pixels: pixelSize, refreshRate: refreshRate)
    }

    /// Half the monitor in whole points, which may be odd: a MacBook panel at "More Space" is
    /// 3420×2214 pixels, so its crisp mode looks like 1710×1107. Rounding that to an even 1108 gave a
    /// 3420×2216 backing that matched nothing. Nil when the monitor itself has an odd side.
    private var exactHalf: DisplaySize? {
        guard pixelSize.width.isMultiple(of: 2), pixelSize.height.isMultiple(of: 2) else { return nil }
        return DisplaySize(width: pixelSize.width / 2, height: pixelSize.height / 2)
    }
}
