import CoreBridge
import Foundation
import Testing

@testable import Osplay

@MainActor
@Suite("Host session")
struct HostSessionTests {
    private let monitors = [
        MonitorDescription(
            id: 3, widthPixels: 2560, heightPixels: 1440, refreshMillihertz: 60_000, physicalWidthMillimeters: 597,
            physicalHeightMillimeters: 336, scalePercent: 100, name: "Left"),
        MonitorDescription(
            id: 7, widthPixels: 1920, heightPixels: 1080, refreshMillihertz: 144_000, physicalWidthMillimeters: 527,
            physicalHeightMillimeters: 296, scalePercent: 100, name: "Right"),
    ]

    private struct Harness {
        let core = FakeHostCore()
        let lender = FakeDisplayLender()
        let captures = FakeCaptureFactory()
        let encoders = FakeEncoderFactory()
        let budget = DisplayBudget()

        func session(
            permissions: FakePermissions = FakePermissions(), streamSize: StreamSizePreset = .native
        ) -> HostSession {
            HostSession(
                core: core, policy: { HostDisplayPolicy(peerID: Data("peer".utf8), streamSize: streamSize) },
                displays: lender, budget: budget,
                captures: captures, encoders: encoders,
                permissions: permissions)
        }
    }

    @Test("Viewer monitors become virtual displays, streams and running captures")
    func startsStreamsForMonitors() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }

        harness.core.emit(.viewerMonitors(monitors))
        try await eventually {
            harness.captures.captures.count == 2 && harness.captures.captures.allSatisfy(\.isRunning)
        }

        let streams = try #require(harness.core.configuredStreams.first)
        #expect(streams.map(\.streamID) == [0, 1])
        #expect(streams.map(\.monitorID) == [3, 7])
        // (fork) The 144 Hz monitor follows its rate up to the 120 Hz cap.
        #expect(streams[1].width == 1920 && streams[1].refreshMillihertz == 120_000)
        #expect(streams[0].bitrateKbps == QualityPreset.high.bitrateKbps(width: 2560, height: 1440, frameRate: 60))
        #expect(harness.lender.lentMonitors.first?.map(\.refreshMillihertz) == [60_000, 120_000])
        #expect(harness.captures.captures.map(\.configuration.displayID) == [200, 201])
        #expect(harness.captures.captures[0].configuration.width == 2560)
        #expect(harness.encoders.encoders.map(\.configuration.frameRate) == [60, 120])

        await session.stop()
        #expect(await run.value == nil)
    }

    @Test("The stream is fitted to the size setting; the virtual display keeps the monitor's size")
    func fitsStreamsToTheSizeSetting() async throws {
        let harness = Harness()
        let session = harness.session(streamSize: .hd720)
        let run = Task { await session.run() }

        harness.core.emit(.viewerMonitors(monitors))
        try await eventually { harness.captures.captures.count == 2 }

        let streams = try #require(harness.core.configuredStreams.first)
        #expect(streams.map(\.width) == [1280, 1280])
        #expect(streams.map(\.height) == [720, 720])
        #expect(streams[0].bitrateKbps == QualityPreset.high.bitrateKbps(width: 1280, height: 720, frameRate: 60))
        #expect(harness.lender.lentMonitors.first?.map(\.widthPixels) == [2560, 1920], "displays at full size")
        #expect(harness.captures.captures.map(\.configuration.width) == [1280, 1280])
        await session.stop()
        _ = await run.value
    }

    @Test("Pairing reaches the UI and the user's answer reaches the core")
    func pairing() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        let received = Task {
            var events: [HostSession.Event] = []
            for await event in session.events {
                events.append(event)
                if case .paired = event { break }
            }
            return events
        }
        let key = [UInt8](repeating: 7, count: 32)
        harness.core.emit(.pairingCode(123_456, peerPublicKey: key))
        await session.confirmPairing(true)
        harness.core.emit(.paired(peerPublicKey: key))

        let events = await received.value
        #expect(events.contains { if case .pairingCode(123_456, key) = $0 { true } else { false } })
        #expect(harness.core.answers == [true])
        await session.stop()
        _ = await run.value
    }

    @Test("A role swap request reaches the UI, and the answer and new requests reach the core")
    func roleSwap() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        let received = Task {
            var events: [HostSession.Event] = []
            for await event in session.events {
                events.append(event)
                if case .roleSwapAnswered = event { break }
            }
            return events
        }
        let key = [UInt8](repeating: 9, count: 32)
        harness.core.emit(.roleSwapRequested(peerPublicKey: key))
        await session.answerRoleSwap(.cannotHost)
        try await session.requestRoleSwap()
        harness.core.emit(.roleSwapAnswered(.accepted, peerPublicKey: key))

        let events = await received.value
        #expect(events.contains { if case .roleSwapRequested(key) = $0 { true } else { false } })
        #expect(events.contains { if case .roleSwapAnswered(.accepted, key) = $0 { true } else { false } })
        #expect(harness.core.roleSwapAnswers == [.cannotHost])
        #expect(harness.core.roleSwapRequests == 1)
        await session.stop()
        _ = await run.value
    }

    @Test("The core's bitrate target reaches the stream's encoder")
    func bitrateTarget() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors([monitors[0]]))
        try await eventually { harness.encoders.encoders.count == 1 }
        harness.core.emit(.bitrateTarget(streamID: 0, kbps: 9_000))
        try await eventually { harness.encoders.encoders[0].bitrateChanges == [9_000] }
        await session.stop()
        _ = await run.value
    }

    @Test("Captured frames reach the core, starting with a keyframe")
    func pushesEncodedFrames() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors([monitors[0]]))
        try await eventually { harness.captures.captures.first?.isRunning == true }

        let capture = try #require(harness.captures.captures.first)
        capture.deliverFrame(at: 1.0)
        capture.deliverFrame(at: 1.016)
        try await eventually { harness.core.pushedUnits.count == 2 }

        let units = harness.core.pushedUnits
        #expect(units.map(\.isKeyframe) == [true, false])
        #expect(units[0].carriesParameterSets)
        #expect(units.allSatisfy { $0.streamID == 0 })
        let spacing = Int64(units[1].captureTimeMicroseconds) - Int64(units[0].captureTimeMicroseconds)
        #expect(abs(spacing - 16_000) < 1_000, "capture times keep their spacing in the core's clock")

        await session.stop()
        _ = await run.value
    }

    @Test("A keyframe request re-encodes the last frame when the screen is unchanged")
    func keyframeRequestReencodesLastFrame() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors([monitors[0]]))
        try await eventually { harness.captures.captures.first?.isRunning == true }
        harness.captures.captures[0].deliverFrame(at: 1.0)
        harness.captures.captures[0].deliverFrame(at: 1.016)

        harness.core.emit(.keyframeRequested(streamID: 0))
        try await eventually { harness.core.pushedUnits.count == 3 }

        #expect(harness.encoders.encoders[0].forcedKeyframes == [true, false, true])
        await session.stop()
        _ = await run.value
    }

    @Test("Revoking Screen Recording mid-session ends it with that reason and cleans up")
    func permissionRevokedMidSession() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors(monitors))
        try await eventually {
            harness.captures.captures.count == 2 && harness.captures.captures.allSatisfy(\.isRunning)
        }

        harness.captures.captures[1].fail(.permissionDenied)

        #expect(await run.value == .stream(.capture(.permissionDenied)))
        #expect(harness.core.isClosed)
        #expect(!harness.lender.isLending)
        #expect(harness.captures.captures.allSatisfy { !$0.isRunning })
        #expect(harness.encoders.encoders.allSatisfy { $0.isInvalidated })
    }

    @Test("Without Screen Recording permission nothing starts")
    func permissionMissing() async {
        let harness = Harness()
        let session = harness.session(permissions: FakePermissions(hasScreenRecordingAccess: false))

        #expect(await session.run() == .screenRecordingPermissionMissing)
        #expect(harness.core.isClosed)
        #expect(harness.lender.lentMonitors.isEmpty)
    }

    @Test("A display failure closes the session with that reason")
    func displayFailure() async {
        let harness = Harness()
        harness.lender.error = .creationFailed
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors(monitors))

        #expect(await run.value == .display(.creationFailed))
        #expect(harness.captures.captures.isEmpty)
    }

    @Test("The viewer leaving is an orderly end")
    func viewerLeaves() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors([monitors[0]]))
        try await eventually { harness.captures.captures.first?.isRunning == true }

        harness.core.emit(.state(.closed, reason: .connectionClosed))

        #expect(await run.value == nil)
        #expect(!harness.lender.isLending)
    }

    @Test("A stream the viewer stops ends its capture and returns only its display")
    func viewerStopsOneStream() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors(monitors))
        try await eventually {
            harness.captures.captures.count == 2 && harness.captures.captures.allSatisfy(\.isRunning)
        }

        harness.core.emit(.streamStopped(streamID: 1))
        // The display goes back only after its pipeline has stopped: wait for the last step.
        try await eventually { harness.lender.returnedDisplays == [201] }

        #expect(!harness.captures.captures[1].isRunning)
        #expect(harness.captures.captures[0].isRunning)
        #expect(harness.encoders.encoders[1].isInvalidated)
        await session.stop()
        _ = await run.value
    }

    @Test("Only as many displays as the Mac supports are lent")
    func limitsDisplayCount() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        let third = MonitorDescription(
            id: 9, widthPixels: 1280, heightPixels: 720, refreshMillihertz: 60_000, physicalWidthMillimeters: 300,
            physicalHeightMillimeters: 170, scalePercent: 100, name: "Third")
        harness.core.emit(.viewerMonitors(monitors + [third]))
        try await eventually { harness.core.configuredStreams.count == 1 }

        #expect(harness.lender.lentMonitors.first?.count == Tunables.VirtualDisplay.maximumDisplayCount)
        await session.stop()
        _ = await run.value
    }
}

extension HostSessionTests {
    @Test("A viewer gets only the displays other viewers left")
    func takesWhatIsLeft() async throws {
        let harness = Harness()
        _ = harness.budget.take(upTo: Tunables.VirtualDisplay.maximumTotalCount - 1)
        let session = harness.session()
        let run = Task { await session.run() }

        harness.core.emit(.viewerMonitors(monitors))
        try await eventually { harness.core.configuredStreams.count == 1 }

        #expect(harness.lender.lentMonitors.first?.map(\.id) == [3])
        #expect(harness.budget.isExhausted)
        await session.stop()
        _ = await run.value
    }

    @Test("With every display taken, a new viewer is turned away before it pairs")
    func refusesWhenNoDisplayIsLeft() async throws {
        let harness = Harness()
        _ = harness.budget.take(upTo: Tunables.VirtualDisplay.maximumTotalCount)
        let session = harness.session()
        let run = Task { await session.run() }
        let events = Task { () -> [String] in
            var seen: [String] = []
            for await event in session.events {
                if case .peer = event { seen.append("peer") }
            }
            return seen
        }

        harness.core.emit(.peer(PeerInfo(platform: .windows, protocolMinor: 1, deviceName: "PC")))

        #expect(await run.value == nil, "turning a viewer away is an orderly end")
        #expect(harness.core.refusedBusy)
        #expect(await events.value.isEmpty, "the viewer never reaches the UI")
        #expect(harness.lender.lentMonitors.isEmpty)
    }

    @Test("Displays go back to the shared budget as streams and sessions end")
    func returnsDisplaysToTheBudget() async throws {
        let harness = Harness()
        let session = harness.session()
        let run = Task { await session.run() }
        harness.core.emit(.viewerMonitors(monitors))
        try await eventually {
            harness.captures.captures.count == 2 && harness.captures.captures.allSatisfy(\.isRunning)
        }

        harness.core.emit(.streamStopped(streamID: 1))
        try await eventually { harness.lender.returnedDisplays == [201] }
        let afterOneStream = harness.budget.take(upTo: Tunables.VirtualDisplay.maximumTotalCount)
        #expect(afterOneStream == Tunables.VirtualDisplay.maximumTotalCount - 1)
        harness.budget.giveBack(afterOneStream)

        await session.stop()
        _ = await run.value
        #expect(
            harness.budget.take(upTo: Tunables.VirtualDisplay.maximumTotalCount)
                == Tunables.VirtualDisplay.maximumTotalCount)
    }
}

/// Polls `condition` until it holds; fails the test after 2 s.
@MainActor
private func eventually(_ condition: @MainActor () -> Bool) async throws {
    let deadline = ContinuousClock.now + .seconds(2)
    while !condition() {
        guard ContinuousClock.now < deadline else {
            Issue.record("condition not met within 2 s")
            return
        }
        try await Task.sleep(for: .milliseconds(5))
    }
}
