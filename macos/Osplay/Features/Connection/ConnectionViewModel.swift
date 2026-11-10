import AppKit
import CoreBridge
import Foundation
import Observation

/// Temporary connection controls (DEVPLAN Phase 10; Phase 11 builds the final UI on top): share
/// this Mac's screens or use it as a display, the hosts Bonjour finds, pairing, and the paired
/// devices.
@MainActor
@Observable
final class ConnectionViewModel {
    enum Mode: Hashable, Sendable {
        case off
        /// Hosting: advertised over Bonjour, a viewer connects.
        case sharing
        /// Viewing: browsing for hosts, the user picks one.
        case viewing
    }

    enum Status: Equatable, Sendable {
        case idle
        case waitingForViewer
        case connecting(peer: String)
        case connected(peer: String)
        /// Sharing with several computers at once (DEVPLAN Phase 13b part SYM-03).
        case connectedMany(count: Int)
        /// The roles are being reversed with this computer.
        case swapping(peer: String)
        /// The connection dropped (sleep, network); it comes back when this computer reappears.
        case reconnecting(peer: String)
    }

    /// A computer this Mac extends onto: one row in the menu.
    struct ConnectedViewer: Identifiable, Equatable, Sendable {
        let id: UUID
        let name: String
        let platform: PeerPlatform
        /// The virtual displays it uses; 0 until its streams run.
        var displayCount = 0
    }

    /// One host session: the one listening for the next viewer, one trying an iPad or waiting on
    /// an Android device on the USB cable, or one serving a viewer.
    @MainActor
    private final class HostConnection {
        let id = UUID()
        let session: HostSession
        let peer: SessionPeer
        /// usbmuxd's ID of the iPad this session reaches over the cable; nil on the network.
        let usbDeviceID: UInt32?
        /// The Android accessory this session waits on or serves over the cable; nil otherwise.
        let accessoryID: UInt64?
        /// nil while it waits for a viewer.
        var viewer: ConnectedViewer?
        var isStreaming = false
        /// The displays standing in for the viewer's monitors, by monitor ID.
        var liveDisplays: [UInt8: CGDirectDisplayID] = [:]
        var deviceID: String?
        var tasks: [Task<Void, Never>] = []

        init(session: HostSession, peer: SessionPeer, usbDeviceID: UInt32?, accessoryID: UInt64?) {
            self.session = session
            self.peer = peer
            self.usbDeviceID = usbDeviceID
            self.accessoryID = accessoryID
        }

        /// The session listening on the network for the next viewer.
        var isListener: Bool { viewer == nil && usbDeviceID == nil && accessoryID == nil }
    }

    /// The host being viewed, so the session can come back after sleep, a network change or a
    /// display change on this Mac.
    private struct ViewedHost {
        let deviceID: String
        let name: String
        /// The displays this session shows the host's monitors on.
        let displays: [LocalDisplay]
    }

    /// After an accepted role swap: the role this Mac takes once the session has closed.
    private struct PendingSwap {
        let role: Mode
        let peerDeviceID: String
        let peerName: String
    }

    /// A pairing code waiting for the user; one is shown at a time.
    private struct PairingRequest {
        let owner: UUID
        let code: UInt32
        let peerName: String
        let answer: (Bool) -> Void
    }

    private(set) var mode: Mode = .off
    private(set) var status: Status = .idle
    /// Hosts to connect to, by name.
    private(set) var devices: [DiscoveredDevice] = []
    private(set) var peers: [TrustedPeer] = []
    private(set) var error: ConnectionError?
    private(set) var hotkeyUnavailable = false
    /// This Mac's displays, for the picker of those offered to hosts.
    private(set) var displays: [LocalDisplay] = LocalDisplays.all()
    /// While sharing: the computers this Mac extends onto, in the order they connected.
    private(set) var viewers: [ConnectedViewer] = []

    private let dependencies: AppDependencies
    private let trust: PeerTrustCache
    private let deviceName: String
    private var identity: DeviceIdentity?
    private var modeTask: Task<Void, Never>?
    /// The virtual displays every viewer of this Mac shares.
    private let displayBudget = DisplayBudget()
    private var hostConnections: [HostConnection] = []
    private var advertising: (any Advertising)?
    /// While sharing: the iPads on the USB cable, and the pauses before one is tried again.
    private var usbTask: Task<Void, Never>?
    private var usbDevices: Set<UInt32> = []
    private var usbRetries: [UInt32: Task<Void, Never>] = [:]
    /// While sharing: the Android devices on the cable in Osplay's accessory mode, and the pauses
    /// before a new session waits on one again.
    private var accessoryTask: Task<Void, Never>?
    private var accessories: [UInt64: AndroidAccessoryLink] = [:]
    private var accessoryRetries: [UInt64: Task<Void, Never>] = [:]
    private var advertisedPort: UInt16?
    private var relistenTask: Task<Void, Never>?
    private var viewer: ViewerSession?
    private var viewerPeer = SessionPeer()
    private var viewerPairingOwner = UUID()
    private var viewerTasks: [Task<Void, Never>] = []
    private var peerName = ""
    private var pairingQueue: [PairingRequest] = []
    private var hotkey: HotkeyRegistration?
    private var swapHotkey: HotkeyRegistration?
    private(set) var swapHotkeyUnavailable = false
    private var pendingSwap: PendingSwap?
    /// Viewing after a swap: connects to this device as soon as Bonjour finds it.
    private var autoConnectDeviceID: String?
    private var autoConnectTimeout: Task<Void, Never>?
    /// (fork) The last computer each peer was seen as: a swap connects straight to it instead of
    /// waiting for Bonjour.
    private var lastSeenDevices: [String: DiscoveredDevice] = [:]
    private var reconnectTask: Task<Void, Never>?
    /// The port sharing listened on last; the next session listens there again.
    private var sharePort: UInt16 = 0
    private var statsHotkey: HotkeyRegistration?
    private var viewedHost: ViewedHost?
    /// The user ended the session; it does not come back by itself.
    private var endedByUser = false
    /// This Mac closed the session itself (sleep, display change) and resumes it afterwards.
    private var resumesAfterClose = false
    @ObservationIgnored private var systemObservers: [NSObjectProtocol] = []

    init(dependencies: AppDependencies, deviceName: String = Host.current().localizedName ?? "Mac") {
        self.dependencies = dependencies
        self.deviceName = deviceName
        trust = PeerTrustCache(store: dependencies.keyStore)
        observeSystem()
    }

    /// This Mac's name as other computers see it (hidden in diagnostics).
    var localName: String { deviceName }

    var isSessionActive: Bool {
        switch status {
        case .connecting, .connected, .connectedMany: true
        case .idle, .waitingForViewer, .swapping, .reconnecting: false
        }
    }

    /// Whether another computer can still get a virtual display from this Mac.
    var hasDisplaysLeft: Bool { !displayBudget.isExhausted }

    /// Roles can be reversed with exactly one connected computer.
    var canSwapRoles: Bool {
        guard pendingSwap == nil else { return false }
        switch mode {
        case .sharing:
            let serving = hostConnections.filter { $0.viewer != nil }
            return serving.count == 1 && serving[0].isStreaming
        case .viewing:
            if case .connected = status { return true }
            return false
        case .off:
            return false
        }
    }

    func setMode(_ newMode: Mode) {
        guard newMode != mode else { return }
        stopAll()
        mode = newMode
        error = nil
        switch newMode {
        case .off:
            status = .idle
        case .sharing:
            startSharing()
        case .viewing:
            startBrowsing()
        }
    }

    func connect(to device: DiscoveredDevice) {
        guard viewer == nil, let identity = prepare() else { return }
        guard device.advertisement.protocolVersion == Advertisement.protocolVersion else {
            error = .incompatible
            return
        }
        displays = LocalDisplays.all()
        let offered = offeredDisplays()
        guard !offered.isEmpty else {
            error = displays.isEmpty ? .noDisplay : .noDisplaySelected
            return
        }
        error = nil
        let peer = SessionPeer()
        let core: CoreSession
        do throws(CoreError) {
            core = try CoreSession.viewer(
                device: sessionDevice(identity, maximumStreams: offered.count), monitors: offered.map(\.description),
                host: device.host, port: device.port, trust: { [trust, peer] key in peer.trust(key, in: trust) })
        } catch {
            self.error = .session(error)
            return
        }
        // Each of the host's streams is shown on the display whose monitor it extends onto.
        let places = Dictionary(uniqueKeysWithValues: offered.map { ($0.id, $0.displayID) })
        let session = ViewerSession(
            core: core, decoders: dependencies.decoders, presenters: dependencies.presenters,
            power: dependencies.power,
            target: { stream in places[stream.monitorID].map(PresentationTarget.fullScreen) },
            showsStats: { [settings = dependencies.settings] in await MainActor.run { settings.showsStatsOverlay } })
        viewer = session
        viewerPeer = peer
        viewerPairingOwner = UUID()
        peerName = device.advertisement.name
        viewedHost = ViewedHost(
            deviceID: device.advertisement.deviceID, name: displayName(of: device), displays: offered)
        endedByUser = false
        status = .connecting(peer: peerName)
        registerHotkey(for: session)
        viewerTasks = [
            Task { [weak self] in
                let failure = await session.run()
                self?.viewerEnded(failure)
            },
            Task { [weak self] in
                for await event in session.events {
                    self?.handle(event, from: session)
                }
            },
        ]
    }

    /// Ends every session; sharing keeps waiting for the next viewer.
    func disconnect() {
        endedByUser = true
        let sessions = hostConnections.filter { $0.viewer != nil }.map(\.session)
        Task { [viewer] in
            for session in sessions {
                await session.stop()
            }
            await viewer?.stop()
        }
    }

    /// Ends the session with this computer only; the others go on.
    func disconnect(_ connected: ConnectedViewer) {
        guard let connection = hostConnections.first(where: { $0.viewer?.id == connected.id }) else { return }
        Task { [session = connection.session] in await session.stop() }
    }

    /// Reverses the roles with the connected computer: this Mac hosts if it was the display, and
    /// becomes the display if it was hosting. The other computer must agree (it answers at once).
    func swapRoles() {
        guard canSwapRoles else { return }
        let host = hostConnections.first { $0.viewer != nil }?.session
        Task { [weak self, viewer] in
            do throws(CoreError) {
                if let host {
                    try await host.requestRoleSwap()
                } else if let viewer {
                    try await viewer.requestRoleSwap()
                }
            } catch {
                self?.error = error == .unsupportedVersion ? .roleSwapUnsupported : .session(error)
            }
        }
    }

    /// Shown in place of the name the device introduced itself with.
    func rename(_ peer: TrustedPeer, to name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        update { () throws(KeyStoreError) in try trust.rename(peer.publicKey, to: trimmed) }
    }

    /// Whether a discovered device is already paired (no code when connecting).
    func isPaired(_ device: DiscoveredDevice) -> Bool {
        peers.contains {
            !$0.revoked && DeviceIdentity.deviceID(publicKey: $0.publicKey) == device.advertisement.deviceID
        }
    }

    /// The paired device's name, or the one it advertises.
    func displayName(of device: DiscoveredDevice) -> String {
        peers.first { DeviceIdentity.deviceID(publicKey: $0.publicKey) == device.advertisement.deviceID }?.name
            ?? device.advertisement.name
    }

    /// Whether this display is offered to hosts (the picker's selection, kept across launches).
    func isOffered(_ display: LocalDisplay) -> Bool {
        !dependencies.settings.excludedDisplays.contains(display.key)
    }

    /// Offers this display to hosts from the next connection on, or leaves it out.
    func setOffered(_ display: LocalDisplay, _ offered: Bool) {
        if offered {
            dependencies.settings.excludedDisplays.remove(display.key)
        } else {
            dependencies.settings.excludedDisplays.insert(display.key)
        }
    }

    private func offeredDisplays() -> [LocalDisplay] {
        displays.filter(isOffered)
    }

    /// Loads the paired devices for Settings before any connection did.
    func loadPeers() {
        _ = prepare()
    }

    /// Refused from now on; a session with it ends at once.
    func revoke(_ peer: TrustedPeer) {
        update { () throws(KeyStoreError) in try trust.revoke(peer.publicKey) }
        endSessions(with: peer.publicKey)
    }

    /// Forgotten: its next connection pairs again. A session with it ends at once.
    func remove(_ peer: TrustedPeer) {
        update { () throws(KeyStoreError) in try trust.remove(peer.publicKey) }
        endSessions(with: peer.publicKey)
    }

    private func endSessions(with publicKey: [UInt8]) {
        for connection in hostConnections where connection.peer.key == publicKey {
            Task { [session = connection.session] in await session.stop() }
        }
        if let viewer, viewerPeer.key == publicKey {
            endedByUser = true
            Task { await viewer.stop() }
        }
    }

    func openScreenRecordingSettings() {
        guard let url = dependencies.permissions.screenRecordingSettingsURL else { return }
        NSWorkspace.shared.open(url)
    }

    /// Read when the menu window opens; sharing needs it.
    var hasScreenRecordingAccess: Bool { dependencies.permissions.hasScreenRecordingAccess }

    func openLocalNetworkSettings() {
        guard let url = dependencies.permissions.localNetworkSettingsURL else { return }
        NSWorkspace.shared.open(url)
    }

    // MARK: - Sharing

    private func startSharing() {
        guard let identity = prepare() else {
            mode = .off
            return
        }
        listen(identity)
        watchUSB(identity)
        watchAccessories(identity)
        updateSharingStatus()
    }

    /// A core host session takes one viewer, so a new one listens as soon as a viewer arrives: several
    /// computers can use this Mac at once (DEVPLAN Phase 13b part SYM-03).
    private func listen(_ identity: DeviceIdentity) {
        guard mode == .sharing, pendingSwap == nil, !hostConnections.contains(where: \.isListener) else {
            return
        }
        let peer = SessionPeer()
        let core: CoreSession
        do throws(CoreError) {
            // The previous session's port keeps the advertisement viewers already resolved right.
            core = try CoreSession.host(
                device: sessionDevice(identity, maximumStreams: Tunables.VirtualDisplay.maximumDisplayCount),
                controlPort: sharePort, trust: { [trust, peer] key in peer.trust(key, in: trust) })
        } catch {
            Log.connection.error("Could not listen for a viewer: \(String(describing: error), privacy: .public)")
            self.error = .session(error)
            listenAgainLater(identity)
            return
        }
        Log.connection.info("Listening for a viewer on port \(core.controlPort)")
        sharePort = core.controlPort
        do throws(DiscoveryError) {
            try advertise(identity, port: core.controlPort)
        } catch {
            core.close()
            self.error = .discovery(error)
            stopSharing()
            mode = .off
            status = .idle
            return
        }
        startHost(core, peer: peer, usbDeviceID: nil, accessoryID: nil)
    }

    /// Runs a host session for the viewer that connects (or answers over the cable) on `core`.
    private func startHost(_ core: CoreSession, peer: SessionPeer, usbDeviceID: UInt32?, accessoryID: UInt64?) {
        let settings = dependencies.settings
        let session = HostSession(
            core: core,
            policy: { [peer, settings] in
                let key = peer.key ?? []
                return await MainActor.run {
                    let deviceID = DeviceIdentity.deviceID(publicKey: key)
                    return HostDisplayPolicy(
                        peerID: Data(key), hiDPI: settings.hiDPIChoice(for: deviceID),
                        arrangement: settings.arrangement(for: deviceID), quality: settings.qualityPreset,
                        streamSize: settings.streamSize,
                        savedResolutions: settings.savedResolutions(for: deviceID),
                        saveResolutions: { [settings] resolutions in
                            Task { @MainActor in
                                settings.setSavedResolutions(resolutions, for: deviceID)
                            }
                        })
                }
            },
            displays: VirtualDisplayLender(provider: dependencies.virtualDisplays), budget: displayBudget,
            captures: dependencies.captures, encoders: dependencies.encoders, permissions: dependencies.permissions)
        let connection = HostConnection(
            session: session, peer: peer, usbDeviceID: usbDeviceID, accessoryID: accessoryID)
        hostConnections.append(connection)
        connection.tasks = [
            Task { [weak self] in
                for await event in session.events {
                    self?.handle(event, from: connection)
                }
            },
            Task { [weak self] in
                let failure = await session.run()
                self?.hostEnded(connection, failure: failure)
            },
        ]
    }

    /// Tries every iPad on the USB cable while sharing is on: one answers once its user picks the
    /// cable there (DEVPLAN Phase 15 part TAB-03).
    private func watchUSB(_ identity: DeviceIdentity) {
        usbTask?.cancel()
        let events = dependencies.usbDevices.watch()
        usbTask = Task { [weak self] in
            for await event in events {
                switch event {
                case .attached(let deviceID):
                    self?.usbDevices.insert(deviceID)
                    self?.tryUSB(deviceID, identity)
                case .detached(let deviceID):
                    self?.usbDevices.remove(deviceID)
                    self?.usbRetries.removeValue(forKey: deviceID)?.cancel()
                }
            }
        }
    }

    private func tryUSB(_ deviceID: UInt32, _ identity: DeviceIdentity) {
        guard mode == .sharing, pendingSwap == nil, usbDevices.contains(deviceID),
            !hostConnections.contains(where: { $0.usbDeviceID == deviceID })
        else { return }
        let peer = SessionPeer()
        let core: CoreSession
        do throws(CoreError) {
            core = try CoreSession.hostOverUSB(
                device: sessionDevice(identity, maximumStreams: Tunables.VirtualDisplay.maximumDisplayCount),
                deviceID: deviceID, trust: { [trust, peer] key in peer.trust(key, in: trust) })
        } catch {
            Log.connection.error("Could not try the iPad on USB: \(String(describing: error), privacy: .public)")
            tryUSBLater(deviceID, identity)
            return
        }
        startHost(core, peer: peer, usbDeviceID: deviceID, accessoryID: nil)
    }

    private func tryUSBLater(_ deviceID: UInt32, _ identity: DeviceIdentity) {
        usbRetries[deviceID]?.cancel()
        usbRetries[deviceID] = Task { [weak self] in
            // Cancelled when the device goes or sharing stops; tryUSB() checks both anyway.
            try? await Task.sleep(for: Tunables.Connection.usbRetryDelay)
            guard !Task.isCancelled else { return }
            self?.usbRetries[deviceID] = nil
            self?.tryUSB(deviceID, identity)
        }
    }

    /// Switches every Android device on the cable to Osplay's accessory while sharing is on and keeps
    /// a session waiting on each: Osplay there starts it when Android hands it the accessory (TAB-05).
    private func watchAccessories(_ identity: DeviceIdentity) {
        accessoryTask?.cancel()
        let events = dependencies.androidAccessories.watch()
        accessoryTask = Task { [weak self] in
            for await event in events {
                switch event {
                case .attached(let link):
                    self?.accessories[link.id] = link
                    self?.serveAccessory(link.id, identity)
                case .detached(let id):
                    self?.accessories.removeValue(forKey: id)
                    self?.accessoryRetries.removeValue(forKey: id)?.cancel()
                }
            }
        }
    }

    private func serveAccessory(_ id: UInt64, _ identity: DeviceIdentity) {
        guard mode == .sharing, pendingSwap == nil, let link = accessories[id],
            !hostConnections.contains(where: { $0.accessoryID == id })
        else { return }
        let peer = SessionPeer()
        let core: CoreSession
        do throws(CoreError) {
            core = try CoreSession.hostOverAccessory(
                device: sessionDevice(identity, maximumStreams: Tunables.VirtualDisplay.maximumDisplayCount),
                pipe: link.makePipe(), trust: { [trust, peer] key in peer.trust(key, in: trust) })
        } catch {
            Log.connection.error(
                "Could not wait on the Android accessory: \(String(describing: error), privacy: .public)")
            serveAccessoryLater(id, identity)
            return
        }
        startHost(core, peer: peer, usbDeviceID: nil, accessoryID: id)
    }

    private func serveAccessoryLater(_ id: UInt64, _ identity: DeviceIdentity) {
        accessoryRetries[id]?.cancel()
        accessoryRetries[id] = Task { [weak self] in
            // Cancelled when the device goes or sharing stops; serveAccessory() checks both anyway.
            try? await Task.sleep(for: Tunables.Connection.usbRetryDelay)
            guard !Task.isCancelled else { return }
            self?.accessoryRetries[id] = nil
            self?.serveAccessory(id, identity)
        }
    }

    /// One advertisement for as long as sharing is on; a new one only when the port changed.
    private func advertise(_ identity: DeviceIdentity, port: UInt16) throws(DiscoveryError) {
        guard advertising == nil || advertisedPort != port else { return }
        advertising?.cancel()
        advertising = nil
        advertising = try dependencies.discovery.advertise(
            Advertisement(identity: identity, name: deviceName, roles: .host), port: port)
        advertisedPort = port
        Log.connection.info("Advertising on port \(port)")
    }

    private func listenAgainLater(_ identity: DeviceIdentity) {
        relistenTask?.cancel()
        relistenTask = Task { [weak self] in
            // Cancelled when sharing stops; listen() checks the mode anyway.
            try? await Task.sleep(for: Tunables.Connection.relistenDelay)
            guard !Task.isCancelled else { return }
            self?.listen(identity)
        }
    }

    private func hostEnded(_ connection: HostConnection, failure: HostSessionError?) {
        // Gone already when sharing stopped on purpose.
        guard hostConnections.contains(where: { $0 === connection }) else { return }
        hostConnections.removeAll { $0 === connection }
        dropPairing(owner: connection.id)
        publishViewers()
        if pendingSwap != nil, connection.viewer != nil {
            performSwap()
            return
        }
        if let deviceID = connection.usbDeviceID, let identity {
            // The iPad is tried again; it only answers when its user picks the cable again.
            tryUSBLater(deviceID, identity)
            // An iPad not waiting refuses the connection: that is no failure to show.
            guard connection.viewer != nil else { return }
        }
        if let accessoryID = connection.accessoryID, let identity {
            // A new session waits on the device; after a pause, so a cable that just went is
            // reported gone first.
            serveAccessoryLater(accessoryID, identity)
            guard connection.viewer != nil else { return }
        }
        if let failure {
            error = .host(failure)
            if case .screenRecordingPermissionMissing = failure {
                stopSharing()
                mode = .off
                status = .idle
                return
            }
        }
        updateSharingStatus()
        updateSwapHotkey()
        // The listener ended (a viewer turned away, a failure, wake): another one takes its place.
        if connection.isListener, let identity {
            listenAgainLater(identity)
        }
    }

    private func handle(_ event: HostSession.Event, from connection: HostConnection) {
        let session = connection.session
        switch event {
        case .peer(let peer):
            connection.viewer = ConnectedViewer(id: connection.id, name: peer.deviceName, platform: peer.platform)
            if let accessoryID = connection.accessoryID { accessories[accessoryID]?.markOpened() }
            publishViewers()
            updateSharingStatus()
            updateSwapHotkey()
            // The next computer can connect while this one pairs and streams.
            if let identity { listen(identity) }
        case .pairingCode(let code, _):
            requestPairing(owner: connection.id, code: code, peerName: connection.viewer?.name ?? "") { accepted in
                Task { await session.confirmPairing(accepted) }
            }
        case .paired(let key):
            storePairing(key, name: connection.viewer?.name ?? "")
        case .state(let state):
            if state == .streaming { hostConnected(connection) }
        case .streaming(let displays):
            rememberArrangement(displays, for: connection)
            connection.viewer?.displayCount = displays.count
            publishViewers()
        case .statistics:
            break
        case .roleSwapRequested(let key):
            let name = connection.viewer?.name ?? ""
            // With several computers connected the roles cannot be reversed.
            guard hostConnections.filter({ $0.viewer != nil }).count == 1 else {
                Task { await session.answerRoleSwap(.busy) }
                return
            }
            answerSwap(from: key, becoming: .viewing, peerName: name) { answer in
                Task { await session.answerRoleSwap(answer) }
            }
        case .roleSwapAnswered(let answer, let key):
            swapAnswered(answer, peer: key, becoming: .viewing, peerName: connection.viewer?.name ?? "")
        }
    }

    private func hostConnected(_ connection: HostConnection) {
        connection.isStreaming = true
        if let key = connection.peer.key {
            update { () throws(KeyStoreError) in try trust.seen(key) }
        }
        updateSharingStatus()
        updateSwapHotkey()
    }

    private func publishViewers() {
        viewers = hostConnections.compactMap(\.viewer)
    }

    private func updateSharingStatus() {
        guard mode == .sharing else { return }
        if let swap = pendingSwap {
            status = .swapping(peer: swap.peerName)
            return
        }
        let serving = hostConnections.filter { $0.viewer != nil }
        switch serving.count {
        case 0:
            status = .waitingForViewer
        case 1:
            let name = serving[0].viewer?.name ?? ""
            status = serving[0].isStreaming ? .connected(peer: name) : .connecting(peer: name)
        default:
            status = .connectedMany(count: serving.count)
        }
    }

    /// Ends every host session and the advertisement.
    private func stopSharing() {
        relistenTask?.cancel()
        relistenTask = nil
        usbTask?.cancel()
        usbTask = nil
        usbRetries.values.forEach { $0.cancel() }
        usbRetries = [:]
        usbDevices = []
        accessoryTask?.cancel()
        accessoryTask = nil
        accessoryRetries.values.forEach { $0.cancel() }
        accessoryRetries = [:]
        accessories = [:]
        let connections = hostConnections
        hostConnections = []
        for connection in connections {
            dropPairing(owner: connection.id)
            Task { [session = connection.session] in await session.stop() }
        }
        advertising?.cancel()
        advertising = nil
        advertisedPort = nil
        publishViewers()
        unregisterSwapHotkey()
    }

    // MARK: - Viewing

    private func startBrowsing() {
        guard prepare() != nil else {
            mode = .off
            return
        }
        status = .idle
        let events = dependencies.discovery.browse()
        modeTask = Task { [weak self] in
            for await event in events {
                self?.discovered(event)
            }
        }
    }

    private func discovered(_ event: DiscoveryEvent) {
        switch event {
        case .found(let device):
            guard device.advertisement.roles.contains(.host), device.advertisement.deviceID != identity?.deviceID
            else { return }
            devices.removeAll { $0.label == device.label }
            devices.append(device)
            devices.sort { $0.advertisement.name.localizedStandardCompare($1.advertisement.name) == .orderedAscending }
            lastSeenDevices[device.advertisement.deviceID] = device
            if device.advertisement.deviceID == autoConnectDeviceID {
                autoConnectDeviceID = nil
                autoConnectTimeout?.cancel()
                connect(to: device)
            }
        case .lost(let label):
            devices.removeAll { $0.label == label }
        case .failed(let reason):
            error = .discovery(reason)
            devices = []
        }
    }

    private func handle(_ event: ViewerSession.Event, from session: ViewerSession) {
        switch event {
        case .peer(let peer):
            peerName = peer.deviceName
        case .pairingCode(let code, _):
            requestPairing(owner: viewerPairingOwner, code: code, peerName: peerName) { accepted in
                Task { await session.confirmPairing(accepted) }
            }
        case .paired(let key):
            storePairing(key, name: peerName)
        case .state(let state):
            if state == .streaming { viewerConnected() }
        case .streamAdded, .visibility, .statistics:
            break
        case .roleSwapRequested(let key):
            answerSwap(from: key, becoming: .sharing, peerName: peerName) { answer in
                Task { await session.answerRoleSwap(answer) }
            }
        case .roleSwapAnswered(let answer, let key):
            swapAnswered(answer, peer: key, becoming: .sharing, peerName: peerName)
        }
    }

    private func viewerEnded(_ failure: CoreError?) {
        if let hotkey { dependencies.hotkeys.unregister(hotkey) }
        hotkey = nil
        if let statsHotkey { dependencies.hotkeys.unregister(statsHotkey) }
        statsHotkey = nil
        hotkeyUnavailable = false
        unregisterSwapHotkey()
        viewerTasks.forEach { $0.cancel() }
        viewerTasks = []
        viewer = nil
        dropPairing(owner: viewerPairingOwner)
        status = .idle
        let host = viewedHost
        viewedHost = nil
        let resumes = resumesAfterClose
        resumesAfterClose = false
        if pendingSwap != nil {
            performSwap()
            return
        }
        // Sleep, a network change or a display change: wait for the same host and connect again.
        let expectedBack = resumes || failure == .peerSleeping || failure == .timedOut || failure == .network
        if let host, expectedBack, !endedByUser, mode == .viewing {
            status = .reconnecting(peer: host.name)
            reconnectTask?.cancel()
            reconnectTask = Task { [weak self] in
                try? await Task.sleep(for: Tunables.Connection.reconnectDelay)
                guard !Task.isCancelled, let self, mode == .viewing, viewer == nil else { return }
                awaitHost(host.deviceID, timeout: nil)
            }
            return
        }
        if let failure { error = .session(failure) }
    }

    private func viewerConnected() {
        status = .connected(peer: peerName)
        updateSwapHotkey()
        if let key = viewerPeer.key {
            update { () throws(KeyStoreError) in try trust.seen(key) }
        }
    }

    // MARK: - Interruptions

    private func observeSystem() {
        let workspace = NSWorkspace.shared.notificationCenter
        systemObservers = [
            workspace.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { [weak self] in self?.willSleep() }
            },
            workspace.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { [weak self] in self?.didWake() }
            },
            NotificationCenter.default.addObserver(
                forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
            ) { _ in
                MainActor.assumeIsolated { [weak self] in self?.screensChanged() }
            },
        ]
    }

    /// Tells the other computers this Mac is going away for a while; a viewer comes back after wake.
    private func willSleep() {
        if let viewer {
            resumesAfterClose = true
            Task { await viewer.stopForSleep() }
        }
        for connection in hostConnections {
            Task { [session = connection.session] in await session.stopForSleep() }
        }
    }

    /// Sockets and the Bonjour registration from before sleep may be stale: sharing listens and
    /// advertises anew.
    private func didWake() {
        guard mode == .sharing else { return }
        advertising?.cancel()
        advertising = nil
        advertisedPort = nil
        if let listener = hostConnections.first(where: \.isListener) {
            // Its end starts the next listener, which advertises again.
            Task { [session = listener.session] in await session.stop() }
        } else if let identity {
            listen(identity)
        }
    }

    /// This Mac's displays changed. A viewed display with a new size, refresh rate or scale makes the
    /// session start again so the host matches it; one that went away ends only its own stream,
    /// unless it was the last, which also starts the session again with what is offered now.
    private func screensChanged() {
        displays = LocalDisplays.all()
        guard let viewer, let host = viewedHost else { return }
        let current = Dictionary(displays.map { ($0.displayID, $0) }, uniquingKeysWith: { first, _ in first })
        let removed = host.displays.filter { current[$0.displayID] == nil }
        let changed = host.displays.contains { viewed in
            current[viewed.displayID].map { !$0.hasSameMode(as: viewed) } ?? false
        }
        if changed || (!removed.isEmpty && removed.count == host.displays.count) {
            resumesAfterClose = true
            Task { await viewer.stop() }
            return
        }
        guard !removed.isEmpty else { return }
        let removedIDs = Set(removed.map(\.id))
        viewedHost = ViewedHost(
            deviceID: host.deviceID, name: host.name, displays: host.displays.filter { !removedIDs.contains($0.id) })
        Task { await viewer.displaysRemoved(removedIDs) }
    }

    /// Connects to this device as soon as Bonjour finds it as a host; `timeout` nil waits as long as
    /// viewing stays on. A direct connect that is already talking to the host wins over this.
    private func awaitHost(_ deviceID: String, timeout: Duration?, onTimeout: (() -> Void)? = nil) {
        autoConnectTimeout?.cancel()
        if viewer != nil {
            return  // (fork) the direct connect already got in; nothing to wait for
        }
        if let device = devices.first(where: { $0.advertisement.deviceID == deviceID }) {
            autoConnectDeviceID = nil
            connect(to: device)
            return
        }
        autoConnectDeviceID = deviceID
        guard let timeout else { return }
        autoConnectTimeout = Task { [weak self] in
            try? await Task.sleep(for: timeout)
            guard !Task.isCancelled, let self, autoConnectDeviceID == deviceID, viewer == nil else { return }
            autoConnectDeviceID = nil
            onTimeout?()
        }
    }

    // MARK: - Pairing

    /// Shows the code at once, or after the codes before it were answered.
    private func requestPairing(owner: UUID, code: UInt32, peerName: String, answer: @escaping (Bool) -> Void) {
        pairingQueue.append(PairingRequest(owner: owner, code: code, peerName: peerName, answer: answer))
        if pairingQueue.count == 1 { showNextPairing() }
    }

    private func showNextPairing() {
        guard let request = pairingQueue.first else { return }
        dependencies.pairingPrompt.show(code: request.code, peerName: request.peerName) {
            [weak self, request] accepted in
            guard let self, pairingQueue.first?.owner == request.owner else { return }
            pairingQueue.removeFirst()
            request.answer(accepted)
            showNextPairing()
        }
    }

    /// The session asking ended: its code goes away, unanswered.
    private func dropPairing(owner: UUID) {
        guard let index = pairingQueue.firstIndex(where: { $0.owner == owner }) else { return }
        pairingQueue.remove(at: index)
        guard index == 0 else { return }
        dependencies.pairingPrompt.dismiss()
        showNextPairing()
    }

    // MARK: - Role swap

    /// The other computer asks to reverse the roles: agree when this Mac can take its new role.
    private func answerSwap(
        from key: [UInt8], becoming role: Mode, peerName: String, answer: @escaping (RoleSwapAnswer) -> Void
    ) {
        let able = role == .sharing ? hasScreenRecordingAccess : !offeredDisplays().isEmpty
        guard able, pendingSwap == nil else {
            answer(able ? .busy : .cannotHost)
            return
        }
        pendingSwap = PendingSwap(
            role: role, peerDeviceID: DeviceIdentity.deviceID(publicKey: key), peerName: peerName)
        status = .swapping(peer: peerName)
        unregisterSwapHotkey()
        answer(.accepted)
    }

    private func swapAnswered(_ answer: RoleSwapAnswer, peer key: [UInt8], becoming role: Mode, peerName: String) {
        guard answer == .accepted else {
            error = .roleSwap(answer)
            return
        }
        pendingSwap = PendingSwap(
            role: role, peerDeviceID: DeviceIdentity.deviceID(publicKey: key), peerName: peerName)
        status = .swapping(peer: peerName)
        unregisterSwapHotkey()
    }

    /// The session closed after an agreed swap: take the new role. Viewing connects to the same
    /// computer as soon as it advertises itself as a host; it is already paired, so no code.
    private func performSwap() {
        guard let swap = pendingSwap else { return }
        pendingSwap = nil
        setMode(swap.role)
        guard swap.role == .viewing else { return }
        status = .swapping(peer: swap.peerName)
        // (fork) The new host listens on its last known address and port within a second or two:
        // connecting straight there beats waiting for Bonjour. The discovery wait below stays as
        // the fallback for when that address has changed.
        if let last = lastSeenDevices[swap.peerDeviceID] {
            connect(to: last)
        }
        awaitHost(swap.peerDeviceID, timeout: Tunables.Connection.swapReconnectTimeout) { [weak self] in
            self?.error = .roleSwapPeerNotFound
            self?.status = .idle
        }
    }

    /// The swap shortcut works only while the roles can be reversed.
    private func updateSwapHotkey() {
        if canSwapRoles {
            registerSwapHotkey()
        } else {
            unregisterSwapHotkey()
        }
    }

    private func registerSwapHotkey() {
        guard swapHotkey == nil else { return }
        do throws(HotkeyError) {
            swapHotkey = try dependencies.hotkeys.register(dependencies.settings.swapHotkey) { [weak self] in
                MenuWindow.close()
                self?.swapRoles()
            }
            swapHotkeyUnavailable = false
        } catch {
            swapHotkeyUnavailable = true
        }
    }

    private func unregisterSwapHotkey() {
        if let swapHotkey { dependencies.hotkeys.unregister(swapHotkey) }
        swapHotkey = nil
        swapHotkeyUnavailable = false
    }

    private func registerHotkey(for session: ViewerSession) {
        do throws(HotkeyError) {
            hotkey = try dependencies.hotkeys.register(dependencies.settings.viewerHotkey) {
                MenuWindow.close()
                Task { await session.toggleVisibility() }
            }
        } catch {
            hotkeyUnavailable = true
        }
        // The overlay switch is optional: a taken shortcut leaves the Settings toggle.
        statsHotkey = try? dependencies.hotkeys.register(dependencies.settings.statsHotkey) {
            [settings = dependencies.settings] in
            MenuWindow.close()
            settings.showsStatsOverlay.toggle()
        }
    }

    // MARK: - Shared

    /// The identity and the paired devices, loaded once.
    private func prepare() -> DeviceIdentity? {
        if let identity { return identity }
        do throws(KeyStoreError) {
            let loaded = try dependencies.keyStore.identity()
            try trust.load()
            identity = loaded
            peers = trust.peers
            return loaded
        } catch {
            self.error = .keyStore(error)
            return nil
        }
    }

    private func sessionDevice(_ identity: DeviceIdentity, maximumStreams: Int) -> SessionDevice {
        SessionDevice(
            identity: identity, name: deviceName,
            capabilities: SessionCapabilities(
                maximumStreams: maximumStreams, maximumWidth: Tunables.Connection.maximumWidth,
                maximumHeight: Tunables.Connection.maximumHeight,
                maximumRefreshHz: Tunables.Connection.maximumRefreshHz))
    }

    /// Keeps where the viewer's displays sit, so Settings shows them and the next connection reuses it.
    private func rememberArrangement(_ displays: [LentDisplay], for connection: HostConnection) {
        guard let key = connection.peer.key else { return }
        let deviceID = DeviceIdentity.deviceID(publicKey: key)
        dependencies.settings.setArrangement(displays.compactMap(\.arrangement), for: deviceID)
        connection.liveDisplays = Dictionary(uniqueKeysWithValues: displays.map { ($0.monitor.id, $0.displayID) })
        connection.deviceID = deviceID
    }

    /// Moves the displays of a running session at once; a saved arrangement alone waits for the
    /// next connection.
    func applyArrangement(_ arrangement: [ArrangedMonitor], for deviceID: String) {
        guard let connection = hostConnections.first(where: { $0.deviceID == deviceID }) else { return }
        let origins = arrangement.compactMap { monitor in
            connection.liveDisplays[monitor.id].map { ($0, monitor.origin) }
        }
        do throws(VirtualDisplayError) {
            try DisplayArranger.apply(origins)
        } catch {
            Log.virtualDisplay.error("Moving displays failed: \(String(describing: error), privacy: .public)")
        }
    }

    private func storePairing(_ key: [UInt8], name: String) {
        update { () throws(KeyStoreError) in try trust.paired(key, name: name) }
    }

    private func update(_ change: () throws(KeyStoreError) -> Void) {
        do throws(KeyStoreError) {
            try change()
        } catch {
            self.error = .keyStore(error)
        }
        peers = trust.peers
    }

    private func stopAll() {
        modeTask?.cancel()
        modeTask = nil
        reconnectTask?.cancel()
        reconnectTask = nil
        autoConnectDeviceID = nil
        autoConnectTimeout?.cancel()
        autoConnectTimeout = nil
        stopSharing()
        disconnect()
        devices = []
        pairingQueue = []
        dependencies.pairingPrompt.dismiss()
    }
}
