import CoreBridge
import SwiftUI

/// The Settings window: one tab per DEVPLAN group.
struct SettingsView: View {
    let viewModel: SettingsViewModel
    let updates: UpdatesViewModel

    var body: some View {
        TabView {
            GeneralSettings(viewModel: viewModel, updates: updates)
                .tabItem { Label("settings.general", systemImage: Theme.Symbol.general) }
            DisplaySettings(viewModel: viewModel)
                .tabItem { Label("settings.displays", systemImage: Theme.Symbol.displays) }
            DeviceSettings(viewModel: viewModel)
                .tabItem { Label("settings.devices", systemImage: Theme.Symbol.devices) }
            AdvancedSettings(viewModel: viewModel, settings: viewModel.settings)
                .tabItem { Label("settings.advanced", systemImage: Theme.Symbol.advanced) }
        }
        .frame(width: Theme.Size.settingsWidth, height: Theme.Size.settingsHeight)
        .background(SettingsWindowReader())
        .onAppear(perform: viewModel.appeared)
    }
}

private struct GeneralSettings: View {
    let viewModel: SettingsViewModel
    let updates: UpdatesViewModel

    var body: some View {
        Form {
            Toggle(
                "settings.launchAtLogin",
                isOn: Binding(get: { viewModel.launchesAtLogin }, set: { viewModel.setLaunchesAtLogin($0) }))
            if viewModel.launchNeedsApproval {
                Text("settings.launchAtLogin.approval")
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            LabeledContent("settings.hotkey") {
                VStack(alignment: .trailing, spacing: Theme.Spacing.small) {
                    HotkeyRecorder(hotkey: viewModel.settings.viewerHotkey, record: viewModel.record)
                    Button("settings.hotkey.reset", action: viewModel.resetHotkey)
                        .buttonStyle(.link)
                }
            }
            Text("settings.hotkey.description")
                .font(Theme.Typography.caption)
                .foregroundStyle(Theme.Colors.secondary)
                .fixedSize(horizontal: false, vertical: true)
            LabeledContent("settings.swapHotkey") {
                VStack(alignment: .trailing, spacing: Theme.Spacing.small) {
                    HotkeyRecorder(hotkey: viewModel.settings.swapHotkey, record: viewModel.recordSwapHotkey)
                    Button("settings.hotkey.reset", action: viewModel.resetSwapHotkey)
                        .buttonStyle(.link)
                }
            }
            Text("settings.swapHotkey.description")
                .font(Theme.Typography.caption)
                .foregroundStyle(Theme.Colors.secondary)
                .fixedSize(horizontal: false, vertical: true)
            LabeledContent("settings.permissions") {
                Button("settings.permissions.open", action: viewModel.openPermissions)
            }
            if let message = viewModel.message {
                Text(message)
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.error)
                    .fixedSize(horizontal: false, vertical: true)
            }
            UpdateSettings(settings: viewModel.settings, updates: updates)
        }
        .formStyle(.grouped)
    }
}

/// The daily check for a newer release, as in the Windows shell.
private struct UpdateSettings: View {
    let settings: AppSettings
    let updates: UpdatesViewModel

    var body: some View {
        Section("updates.title") {
            Toggle(
                "updates.automatic",
                isOn: Binding(get: { settings.checksForUpdates }, set: { settings.checksForUpdates = $0 }))
            HStack {
                Button("updates.checkNow") { Task { await updates.checkNow() } }
                    .disabled(updates.isChecking)
                if updates.available != nil {
                    Button("updates.download", action: updates.openDownload)
                }
            }
            if let result {
                Text(result)
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text("updates.version \(updates.currentVersion.description)")
                .font(Theme.Typography.caption)
                .foregroundStyle(Theme.Colors.secondary)
        }
    }

    private var result: LocalizedStringKey? {
        switch updates.result {
        case .checking: "updates.checking"
        case .upToDate: "updates.upToDate"
        case .available(let version): "updates.available \(version.description)"
        case .failed: "updates.failed"
        case nil: nil
        }
    }
}

private struct DisplaySettings: View {
    let viewModel: SettingsViewModel

    var body: some View {
        Form {
            Section {
                if viewModel.peers.isEmpty {
                    Text("settings.noDevices")
                        .foregroundStyle(Theme.Colors.secondary)
                }
                ForEach(viewModel.peers) { peer in
                    VStack(alignment: .leading, spacing: Theme.Spacing.medium) {
                        Text(verbatim: peer.name)
                            .font(Theme.Typography.rowTitle)
                        let arrangement = viewModel.arrangement(for: peer)
                        if arrangement.isEmpty {
                            Text("settings.arrange.empty")
                                .font(Theme.Typography.caption)
                                .foregroundStyle(Theme.Colors.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        } else {
                            ArrangementCanvas(main: viewModel.mainDisplaySize, monitors: arrangement) { id, origin in
                                viewModel.move(id, to: origin, for: peer)
                            }
                        }
                        Picker(
                            "settings.hiDPI",
                            selection: Binding(
                                get: { viewModel.hiDPIChoice(for: peer) },
                                set: { viewModel.setHiDPIChoice($0, for: peer) })
                        ) {
                            Text("settings.hiDPI.automatic").tag(HiDPIChoice.automatic)
                            Text("settings.hiDPI.always").tag(HiDPIChoice.always)
                            Text("settings.hiDPI.never").tag(HiDPIChoice.never)
                        }
                    }
                }
            } footer: {
                Text("settings.hiDPI.description")
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
    }
}

private struct DeviceSettings: View {
    let viewModel: SettingsViewModel

    var body: some View {
        Form {
            Section {
                if viewModel.peers.isEmpty {
                    Text("settings.noDevices")
                        .foregroundStyle(Theme.Colors.secondary)
                }
                ForEach(viewModel.peers) { peer in
                    PeerRow(peer: peer, viewModel: viewModel)
                }
            } footer: {
                Text("settings.devices.description")
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
    }
}

private struct PeerRow: View {
    let peer: TrustedPeer
    let viewModel: SettingsViewModel
    @State private var name = ""

    var body: some View {
        HStack(spacing: Theme.Spacing.medium) {
            TextField("settings.devices.name", text: $name)
                .labelsHidden()
                .onSubmit { viewModel.rename(peer, to: name) }
                .accessibilityLabel(Text("settings.devices.name"))
            if peer.revoked {
                Text("connection.revoked")
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.error)
            } else {
                Text(peer.lastSeen, format: .relative(presentation: .named))
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.secondary)
                Button("connection.revoke") { viewModel.revoke(peer) }
            }
            Button("connection.remove", role: .destructive) { viewModel.remove(peer) }
        }
        .onAppear { name = peer.name }
        .onChange(of: peer.name) { _, newName in name = newName }
    }
}

private struct AdvancedSettings: View {
    let viewModel: SettingsViewModel
    @Bindable var settings: AppSettings

    var body: some View {
        Form {
            Toggle("settings.viewerInWindow", isOn: $settings.viewerShowsInWindow)
            Text("settings.viewerInWindow.description")
                .font(Theme.Typography.caption)
                .foregroundStyle(Theme.Colors.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Toggle("settings.statsOverlay", isOn: $settings.showsStatsOverlay)
            Text("settings.statsOverlay.description")
                .font(Theme.Typography.caption)
                .foregroundStyle(Theme.Colors.secondary)
                .fixedSize(horizontal: false, vertical: true)
            LabeledContent("settings.statsHotkey") {
                VStack(alignment: .trailing, spacing: Theme.Spacing.small) {
                    HotkeyRecorder(hotkey: settings.statsHotkey, record: viewModel.recordStatsHotkey)
                    Button("settings.hotkey.reset", action: viewModel.resetStatsHotkey)
                        .buttonStyle(.link)
                }
            }
            LabeledContent("settings.logs") {
                HStack {
                    Button("settings.logs.export", action: viewModel.exportLogs)
                    Button("settings.logs.open", action: viewModel.openLogsFolder)
                }
            }
            LabeledContent("settings.diagnostics") {
                Button("settings.diagnostics.copy", action: viewModel.copyDiagnostics)
            }
            Text(viewModel.diagnosticsCopied ? "settings.diagnostics.copied" : "settings.diagnostics.description")
                .font(Theme.Typography.caption)
                .foregroundStyle(Theme.Colors.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if let message = viewModel.message {
                Text(message)
                    .font(Theme.Typography.caption)
                    .foregroundStyle(Theme.Colors.error)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text("menu.coreVersion \(CoreVersion.linked.description)")
                .font(Theme.Typography.caption)
                .foregroundStyle(Theme.Colors.secondary)
        }
        .formStyle(.grouped)
    }
}
