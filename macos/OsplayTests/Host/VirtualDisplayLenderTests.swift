import CoreBridge
import Foundation
import Testing

@testable import Osplay

@MainActor
@Suite("Virtual display lender")
struct VirtualDisplayLenderTests {
    private func monitor(id: UInt8, scalePercent: Int) -> MonitorDescription {
        MonitorDescription(
            id: id, widthPixels: 2560, heightPixels: 1440, refreshMillihertz: 60_000, physicalWidthMillimeters: 598,
            physicalHeightMillimeters: 336, scalePercent: scalePercent, name: "Monitor \(id)")
    }

    @Test("Each session returns only the displays it lent; other viewers keep theirs")
    func lendersShareTheProvider() async throws {
        let provider = FakeVirtualDisplayProvider()
        let first = VirtualDisplayLender(provider: provider, arrange: { _ in })
        let second = VirtualDisplayLender(provider: provider, arrange: { _ in })

        let firstDisplays = try await first.lend(
            [monitor(id: 0, scalePercent: 100), monitor(id: 1, scalePercent: 100)], peerID: Data("pc".utf8),
            hiDPI: .automatic, arrangement: [], saved: [:])
        let secondDisplays = try await second.lend(
            [monitor(id: 0, scalePercent: 100)], peerID: Data("mac".utf8), hiDPI: .automatic,
            arrangement: [], saved: [:])
        #expect(provider.handles.count == 3, "a new viewer leaves the others' displays alone")

        second.returnAll()
        #expect(provider.handles.map(\.displayID) == firstDisplays.map(\.displayID))
        second.returnDisplay(firstDisplays[0].displayID)
        #expect(provider.handles.count == 2, "a display lent by another session is not this one's to return")
        first.returnDisplay(firstDisplays[1].displayID)
        #expect(provider.handles.map(\.displayID) == [firstDisplays[0].displayID])
        #expect(!secondDisplays.isEmpty)
    }

    @Test("A monitor at 100% starts at full size, a high-density one in crisp HiDPI")
    func initialResolutionFollowsTheViewersScaling() async throws {
        let provider = FakeVirtualDisplayProvider()
        let lender = VirtualDisplayLender(provider: provider, arrange: { _ in })

        _ = try await lender.lend(
            [monitor(id: 0, scalePercent: 100), monitor(id: 1, scalePercent: 200)], peerID: Data("peer".utf8),
            hiDPI: .automatic, arrangement: [], saved: [:])

        let full = DisplaySize(width: 2560, height: 1440)
        #expect(provider.handles[0].activated.map(\.points) == [full])
        #expect(provider.handles[0].activated.map(\.pixels) == [full])
        #expect(provider.handles[1].activated.map(\.points) == [DisplaySize(width: 1280, height: 720)])
        #expect(provider.handles[1].activated.map(\.pixels) == [full])
    }

    @Test("The user's choice for a viewer overrides its scaling")
    func choiceOverridesScaling() async throws {
        let provider = FakeVirtualDisplayProvider()
        let lender = VirtualDisplayLender(provider: provider, arrange: { _ in })

        _ = try await lender.lend(
            [monitor(id: 0, scalePercent: 100)], peerID: Data("peer".utf8), hiDPI: .always, arrangement: [], saved: [:])

        #expect(provider.handles[0].activated.map(\.points) == [DisplaySize(width: 1280, height: 720)])
    }

    @Test("A MacBook panel at More Space starts in its crisp mode (3420×2214 looks like 1710×1107)")
    func oddHalfSizeStartsCrisp() async throws {
        let provider = FakeVirtualDisplayProvider()
        let lender = VirtualDisplayLender(provider: provider, arrange: { _ in })
        let panel = MonitorDescription(
            id: 0, widthPixels: 3420, heightPixels: 2214, refreshMillihertz: 60_000, physicalWidthMillimeters: 331,
            physicalHeightMillimeters: 214, scalePercent: 200, name: "Built-in Retina Display")

        _ = try await lender.lend([panel], peerID: Data("peer".utf8), hiDPI: .automatic, arrangement: [], saved: [:])

        #expect(provider.handles[0].activated.map(\.points) == [DisplaySize(width: 1710, height: 1107)])
        #expect(provider.handles[0].activated.map(\.pixels) == [DisplaySize(width: 3420, height: 2214)])
    }

    @Test("Without its preferred resolution a display starts at the native one instead of failing")
    func fallsBackToNative() async throws {
        let provider = FakeVirtualDisplayProvider()
        provider.hidesHiDPIModes = true
        let lender = VirtualDisplayLender(provider: provider, arrange: { _ in })

        let lent = try await lender.lend(
            [monitor(id: 0, scalePercent: 200)], peerID: Data("peer".utf8), hiDPI: .automatic,
            arrangement: [], saved: [:])

        let full = DisplaySize(width: 2560, height: 1440)
        #expect(lent.count == 1)
        #expect(provider.handles[0].activated.map(\.points) == [full])
        #expect(provider.handles[0].activated.map(\.pixels) == [full])
    }
}
