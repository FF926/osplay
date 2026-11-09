using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.Net.NetworkInformation;
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Osplay.App.Diagnostics;
using Osplay.App.Engine;
using Osplay.App.Interop;
using Osplay.App.Services;
using Osplay.App.Settings;

namespace Osplay.App.Connection;

/// <summary>
/// Sessions, discovery, pairing and devices: share this PC's screen or use it as a display. Runs
/// on the UI thread; every engine call goes through <see cref="Native"/>, so nothing here waits
/// for native code. One operation (mode change, connect) runs at a time.
/// </summary>
public sealed partial class ConnectionViewModel : ObservableObject, IDisposable
{
    private readonly Dispatcher _dispatcher;
    private readonly IPairingPrompt _pairing;
    private readonly AppSettings _settings;
    private readonly string _deviceName = System.Net.Dns.GetHostName();
    private readonly Dictionary<string, DeviceInfo> _found = [];
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly HashSet<string> _pairedNames = new(StringComparer.CurrentCultureIgnoreCase);
    private EngineBrowser? _browser;
    /// <summary>Host sessions: the one listening for the next viewer, and one per viewer.</summary>
    private readonly List<HostEntry> _hosts = [];
    private EngineAdvertiser? _advertiser;
    private ushort _advertisedPort;
    private bool _startingListener;
    // While sharing: the iPads on the USB cable, and those a session is being started for.
    private EngineUsbWatcher? _usbWatcher;
    private readonly HashSet<uint> _usbDevices = [];
    private readonly HashSet<uint> _usbStarting = [];
    // While sharing: the Android accessories on the USB cable, and those a session is being started for.
    private EngineAndroidWatcher? _androidWatcher;
    private readonly HashSet<uint> _accessories = [];
    private readonly HashSet<uint> _accessoryStarting = [];
    /// <summary>Pairing codes waiting for the user; the first one is shown.</summary>
    private readonly List<PairingRequest> _pairingQueue = [];
    private EngineViewer? _viewer;
    private string _peerName = string.Empty;
    private IReadOnlyList<StreamFigures> _figures = [];
    private PendingSwap? _pendingSwap;
    private string? _autoConnectDeviceId;
    private CancellationTokenSource? _autoConnectTimeout;
    private GlobalHotkey? _swapHotkey;
    private GlobalHotkey? _statsHotkey;
    /// <summary>Host pause (fork addition): viewers' streams are blanked until it is toggled back.</summary>
    private GlobalHotkey? _pauseHotkey;
    private bool _streamsPaused;
    private ushort _sharePort;
    private readonly ConsoleDisplayMonitor _display = new();
    private ViewedHost? _viewedHost;
    /// <summary>The user ended the session; it does not come back by itself.</summary>
    private bool _endedByUser;
    /// <summary>This PC closed the session itself (sleep, monitor change) and resumes it afterwards.</summary>
    private bool _resumesAfterClose;

    /// <summary>The host being viewed, so the session can come back after an interruption.</summary>
    private sealed record ViewedHost(string DeviceId, string Name);

    /// <summary>After an accepted role swap: the role this PC takes once the session has closed.</summary>
    private sealed record PendingSwap(ConnectionMode Role, string PeerDeviceId, string PeerName);

    /// <summary>One host session (DEVPLAN Phase 13b part SYM-03).</summary>
    private sealed class HostEntry(EngineHost host)
    {
        public EngineHost Host { get; } = host;

        /// <summary>Its row in the panel; null while it waits for a viewer.</summary>
        public ViewerItem? Viewer { get; set; }

        /// <summary>The session listening on the network for the next viewer (not one waiting on the cable).</summary>
        public bool IsListener => Viewer is null && !Host.IsCable;

        public bool IsStreaming { get; set; }

        public uint DisplayCount { get; set; }
    }

    private sealed record PairingRequest(EngineSession Session, uint Code, string PeerName);

    internal ConnectionViewModel(Dispatcher dispatcher, IPairingPrompt pairing, AppSettings settings)
    {
        _dispatcher = dispatcher;
        _pairing = pairing;
        _settings = settings;
        _settings.PropertyChanged += OnSettingChanged;
        Microsoft.Win32.SystemEvents.PowerModeChanged += OnPowerModeChanged;
        // Modern Standby sends no suspend; the display turning off is the moment the PC sleeps.
        _display.DisplayOffChanged += off => _ = off ? SuspendForSleepAsync() : RefreshAfterWakeAsync();
        NetworkChange.NetworkAddressChanged += (_, _) => _dispatcher.BeginInvoke(RefreshNetworkAsync);
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsOff), nameof(IsSharing), nameof(IsViewing))]
    private ConnectionMode _mode = ConnectionMode.Off;

    [ObservableProperty]
    private string _status = Localization.Strings.ConnectionStatusOff;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasError), nameof(IsErrorOpen))]
    private string? _errorMessage;

    [ObservableProperty]
    private bool _isSessionActive;

    /// <summary>Viewer streams are on screen (the hotkey or the tray hides them).</summary>
    [ObservableProperty]
    private bool _isViewerVisible = true;

    [ObservableProperty]
    private bool _isBusy;

    /// <summary>Frames flow in the viewer session.</summary>
    [ObservableProperty]
    private bool _isStreaming;

    /// <summary>Exactly one computer is connected and streaming: the roles can be swapped.</summary>
    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(SwapRolesCommand))]
    private bool _canSwapRoles;

    /// <summary>Sharing: the row under the connected computers.</summary>
    [ObservableProperty]
    private string _waitingText = Localization.Strings.SharingWaitingFirst;

    /// <summary>The swap shortcut is owned by another program; the button still works.</summary>
    [ObservableProperty]
    private bool _isSwapHotkeyTaken;

    /// <summary>False when the engine library could not be loaded; everything is disabled.</summary>
    [ObservableProperty]
    private bool _isEngineAvailable = true;

    /// <summary>Why this PC cannot share its screen right now, or null.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasHostProblem), nameof(HostProblemMessage), nameof(HealthStatusText))]
    private HostHealth? _hostProblem;

    /// <summary>Whether a connected network is marked Public; reported in diagnostics only.</summary>
    [ObservableProperty]
    private bool _isNetworkPublic;

    /// <summary>A viewer session started: the panel gets out of the way of the new display.</summary>
    public event Action? ViewerStarted;

    public bool IsOff => Mode == ConnectionMode.Off;

    public bool IsSharing => Mode == ConnectionMode.Sharing;

    public bool IsViewing => Mode == ConnectionMode.Viewing;

    public bool HasError => ErrorMessage is not null;

    /// <summary>The error banner; closing it clears the error.</summary>
    public bool IsErrorOpen
    {
        get => HasError;
        set
        {
            if (!value)
            {
                ErrorMessage = null;
            }
        }
    }

    public bool HasHostProblem => HostProblem is not null;

    public string HostProblemMessage => HostProblem switch
    {
        HostHealth.HelperMissing => Localization.Strings.HealthHelperMissing,
        HostHealth.HelperRefused => Localization.Strings.HealthHelperRefused,
        HostHealth.DriverMissing => Localization.Strings.HealthDriverMissing,
        _ => string.Empty,
    };

    /// <summary>Settings › Advanced: the problem, or that sharing is ready.</summary>
    public string HealthStatusText => HostProblem is null ? Localization.Strings.HealthReady : HostProblemMessage;

    /// <summary>"Ctrl+Alt+Shift+S swaps the roles…", or that another program owns the shortcut.</summary>
    public string SwapHint => LocalizedText.Format(
        IsSwapHotkeyTaken ? Localization.Strings.SwapHotkeyTaken : Localization.Strings.SwapHint,
        _settings.SwapHotkey.DisplayText);

    partial void OnIsSwapHotkeyTakenChanged(bool value) => OnPropertyChanged(nameof(SwapHint));

    /// <summary>The swap shortcut works only while the roles can be reversed.</summary>
    partial void OnCanSwapRolesChanged(bool value)
    {
        if (value)
        {
            RegisterSwapHotkey();
        }
        else
        {
            UnregisterSwapHotkey();
        }
    }

    /// <summary>The viewer hotkey, for hints ("Ctrl+Alt+Shift+V hides and shows the display").</summary>
    public string HotkeyHint => LocalizedText.Format(Localization.Strings.ViewerHotkeyHint, _settings.ViewerHotkey.DisplayText);

    /// <summary>Hosts to connect to.</summary>
    public ObservableCollection<DeviceItem> Devices { get; } = [];

    /// <summary>Sharing: the computers this PC extends onto, in the order they connected.</summary>
    public ObservableCollection<ViewerItem> Viewers { get; } = [];

    public ObservableCollection<PeerItem> Peers { get; } = [];

    /// <summary>This PC's monitors; the selected ones are offered to hosts.</summary>
    public ObservableCollection<MonitorTile> Monitors { get; } = [];

    public void Dispose()
    {
        Microsoft.Win32.SystemEvents.PowerModeChanged -= OnPowerModeChanged;
        _display.Dispose();
        _statsHotkey?.Dispose();
        _pauseHotkey?.Dispose();
        _gate.Dispose();
        _autoConnectTimeout?.Dispose();
        _swapHotkey?.Dispose();
    }

    /// <summary>Loads the engine state; called once after start.</summary>
    public async Task InitializeAsync()
    {
        IsEngineAvailable = await Native.IsAvailableAsync().ConfigureAwait(true);
        if (!IsEngineAvailable)
        {
            ErrorMessage = Localization.Strings.ErrorEngineMissing;
            return;
        }
        await RefreshPeersAsync().ConfigureAwait(true);
        await RefreshMonitorsAsync().ConfigureAwait(true);
        await CheckHealthAsync().ConfigureAwait(true);
        await RefreshNetworkAsync().ConfigureAwait(true);
        // Host pause (fork addition): a fixed shortcut — the Settings page has no recorder for it.
        _pauseHotkey = GlobalHotkey.Register(HotkeyGesture.DefaultPause,
            () => _ = ToggleStreamsPausedAsync());
    }

    /// <summary>Ends every session and withdraws the Bonjour service; called before quitting.</summary>
    public async Task ShutdownAsync()
    {
        await _gate.WaitAsync().ConfigureAwait(true);
        try
        {
            await StopAllAsync().ConfigureAwait(true);
            _pairing.Close();
        }
        finally
        {
            _ = _gate.Release();
        }
    }

    /// <summary>Asks the helper whether sharing can work; skipped while a host session holds it.</summary>
    [RelayCommand]
    public async Task CheckHealthAsync()
    {
        if (!IsEngineAvailable || _hosts.Count > 0)
        {
            return;
        }
        HostHealth health = await EngineHealth.CheckAsync().ConfigureAwait(true);
        HostProblem = health is HostHealth.Ready or HostHealth.InUse ? null : health;
    }

    [RelayCommand]
    private Task SetModeAsync(ConnectionMode mode) =>
        // Selecting the active role again turns it off, like the Mac's role cards.
        SwitchModeAsync(mode == Mode ? ConnectionMode.Off : mode);

    private async Task SwitchModeAsync(ConnectionMode target)
    {
        await _gate.WaitAsync().ConfigureAwait(true);
        IsBusy = true;
        try
        {
            await StopAllAsync().ConfigureAwait(true);
            Mode = target;
            ErrorMessage = null;
            switch (target)
            {
                case ConnectionMode.Off:
                    Status = Localization.Strings.ConnectionStatusOff;
                    await CheckHealthAsync().ConfigureAwait(true);
                    break;
                case ConnectionMode.Sharing:
                    await StartSharingAsync().ConfigureAwait(true);
                    break;
                case ConnectionMode.Viewing:
                    await RefreshMonitorsAsync().ConfigureAwait(true);
                    await StartBrowsingAsync().ConfigureAwait(true);
                    break;
            }
        }
        finally
        {
            IsBusy = false;
            _ = _gate.Release();
        }
    }

    /// <summary>
    /// Reverses the roles with the connected computer: this PC hosts if it was the display, and
    /// becomes the display if it was hosting. The other computer answers at once.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanSwapRoles))]
    private async Task SwapRolesAsync()
    {
        EngineSession? session = (EngineSession?)ServingHosts().FirstOrDefault()?.Host ?? _viewer;
        if (session is null || _pendingSwap is not null)
        {
            return;
        }
        EngineResult result = await session.RequestRoleSwapAsync().ConfigureAwait(true);
        if (result != EngineResult.Ok)
        {
            ErrorMessage = result == EngineResult.PeerTooOld ? Localization.Strings.ErrorSwapUnsupported : ErrorMessages.For(result);
        }
    }

    /// <summary>Ends every session; sharing keeps waiting for the next viewer.</summary>
    [RelayCommand]
    private async Task DisconnectAsync()
    {
        _endedByUser = true;
        foreach (HostEntry entry in ServingHosts())
        {
            await entry.Host.StopAsync().ConfigureAwait(true);
        }
        if (_viewer is not null)
        {
            await _viewer.StopAsync().ConfigureAwait(true);
        }
    }

    [RelayCommand]
    private async Task ToggleViewerAsync()
    {
        if (_viewer is not null)
        {
            await _viewer.SetVisibleAsync(!IsViewerVisible).ConfigureAwait(true);
        }
    }

    [RelayCommand]
    private Task RefreshMonitorsAsync() => LoadMonitorsAsync();

    // --- Sharing ------------------------------------------------------------------------------

    private async Task StartSharingAsync()
    {
        Status = Localization.Strings.ConnectionStatusWaiting;
        await ListenAsync().ConfigureAwait(true);
        await WatchUsbAsync().ConfigureAwait(true);
        await WatchAndroidAsync().ConfigureAwait(true);
    }

    /// <summary>
    /// A core host session takes one viewer, so a new one listens as soon as a viewer arrives:
    /// several computers can use this PC at once (DEVPLAN Phase 13b part SYM-03).
    /// </summary>
    private async Task ListenAsync()
    {
        if (Mode != ConnectionMode.Sharing || _pendingSwap is not null || _startingListener ||
            _hosts.Exists(h => h.IsListener))
        {
            return;
        }
        _startingListener = true;
        try
        {
            (EngineResult started, EngineHost? host) = await EngineHost
                .StartAsync(_dispatcher, _deviceName, _settings.Quality.MillibitsPerPixel(), _sharePort, _settings.StreamSize.Box())
                .ConfigureAwait(true);
            if (host is null)
            {
                HostProblem = EngineHealth.FromFailure(started) ?? HostProblem;
                if (ServingHosts().Any())
                {
                    ErrorMessage = ErrorMessages.For(started); // the connected computers keep going
                }
                else
                {
                    Fail(ErrorMessages.For(started));
                }
                return;
            }
            if (Mode != ConnectionMode.Sharing)
            {
                await host.DisposeAsync().ConfigureAwait(true); // switched off meanwhile
                return;
            }
            HostProblem = null;
            // The next session listens here again, so the advertisement viewers resolved stays right.
            _sharePort = host.ControlPort;
            if (_advertiser is null || _advertisedPort != host.ControlPort)
            {
                if (_advertiser is not null)
                {
                    await _advertiser.DisposeAsync().ConfigureAwait(true);
                    _advertiser = null;
                }
                (EngineResult advertised, EngineAdvertiser? advertiser) = await EngineAdvertiser
                    .StartAsync(_deviceName, Roles.Host, host.ControlPort)
                    .ConfigureAwait(true);
                if (advertiser is null)
                {
                    await host.DisposeAsync().ConfigureAwait(true);
                    Fail(ErrorMessages.For(advertised));
                    return;
                }
                _advertiser = advertiser;
                _advertisedPort = host.ControlPort;
            }
            AddHost(host);
        }
        finally
        {
            _startingListener = false;
        }
    }

    private void AddHost(EngineHost host)
    {
        var entry = new HostEntry(host);
        _hosts.Add(entry);
        Attach(host);
        host.PeerArrived += (platform, name) => OnViewerArrived(entry, platform, name);
        host.Streaming += count => OnHostStreaming(entry, count);
        UpdateSharingStatus();
    }

    /// <summary>
    /// Tries every iPad on the USB cable while sharing is on: one answers once its user picks the
    /// cable there (DEVPLAN Phase 15 part TAB-03). Without Apple Devices nothing is ever attached.
    /// </summary>
    private async Task WatchUsbAsync()
    {
        if (Mode != ConnectionMode.Sharing || _usbWatcher is not null)
        {
            return;
        }
        (EngineResult started, EngineUsbWatcher? watcher) = await EngineUsbWatcher.StartAsync(_dispatcher).ConfigureAwait(true);
        if (watcher is null)
        {
            Log.Write(LogLevel.Warning, $"Cannot watch USB devices: {started}"); // sharing goes on without the cable
            return;
        }
        if (Mode != ConnectionMode.Sharing || _usbWatcher is not null)
        {
            await watcher.DisposeAsync().ConfigureAwait(true); // switched off meanwhile
            return;
        }
        _usbWatcher = watcher;
        watcher.Attached += device =>
        {
            _ = _usbDevices.Add(device);
            _ = TryUsbAsync(device);
        };
        watcher.Detached += device => _usbDevices.Remove(device);
    }

    private async Task TryUsbAsync(uint device)
    {
        if (Mode != ConnectionMode.Sharing || _pendingSwap is not null || !_usbDevices.Contains(device) ||
            _usbStarting.Contains(device) || _hosts.Exists(h => h.Host.UsbDeviceId == device))
        {
            return;
        }
        _ = _usbStarting.Add(device);
        try
        {
            (EngineResult started, EngineHost? host) = await EngineHost
                .StartUsbAsync(_dispatcher, _deviceName, _settings.Quality.MillibitsPerPixel(), _settings.StreamSize.Box(), device)
                .ConfigureAwait(true);
            if (host is null)
            {
                Log.Write(LogLevel.Warning, $"Could not try the iPad on USB: {started}");
                _ = TryUsbLaterAsync(device);
                return;
            }
            if (Mode != ConnectionMode.Sharing)
            {
                await host.DisposeAsync().ConfigureAwait(true); // switched off meanwhile
                return;
            }
            AddHost(host);
        }
        finally
        {
            _ = _usbStarting.Remove(device);
        }
    }

    private async Task TryUsbLaterAsync(uint device)
    {
        await Task.Delay(AppTunables.UsbRetryDelay).ConfigureAwait(true);
        await TryUsbAsync(device).ConfigureAwait(true); // checks that sharing and the device are still there
    }

    /// <summary>
    /// Switches every Android device on the USB cable to Osplay's accessory while sharing is on, and
    /// waits on each accessory until Osplay there opens it (TAB-05). Without UsbDk nothing is switched.
    /// </summary>
    private async Task WatchAndroidAsync()
    {
        if (Mode != ConnectionMode.Sharing || _androidWatcher is not null)
        {
            return;
        }
        (EngineResult started, EngineAndroidWatcher? watcher) = await EngineAndroidWatcher.StartAsync(_dispatcher).ConfigureAwait(true);
        if (watcher is null)
        {
            Log.Write(LogLevel.Warning, $"Cannot watch Android devices on USB: {started}"); // sharing goes on without the cable
            return;
        }
        if (Mode != ConnectionMode.Sharing || _androidWatcher is not null)
        {
            await watcher.DisposeAsync().ConfigureAwait(true); // switched off meanwhile
            return;
        }
        _androidWatcher = watcher;
        watcher.Attached += accessory =>
        {
            _ = _accessories.Add(accessory);
            _ = TryAccessoryAsync(accessory);
        };
        watcher.Detached += accessory =>
        {
            _ = _accessories.Remove(accessory);
            // Its session would only notice when it next reads; the cable is gone.
            if (_hosts.Find(h => h.Host.AccessoryId == accessory) is { } entry)
            {
                _ = entry.Host.StopAsync();
            }
        };
        watcher.DebuggingOn += () => ErrorMessage = Localization.Strings.WarningUsbDebugging;
    }

    private async Task TryAccessoryAsync(uint accessory)
    {
        if (Mode != ConnectionMode.Sharing || _pendingSwap is not null || _androidWatcher is not { } watcher ||
            !_accessories.Contains(accessory) || _accessoryStarting.Contains(accessory) ||
            _hosts.Exists(h => h.Host.AccessoryId == accessory))
        {
            return;
        }
        _ = _accessoryStarting.Add(accessory);
        try
        {
            (EngineResult started, EngineHost? host) = await EngineHost
                .StartAccessoryAsync(
                    _dispatcher, _deviceName, _settings.Quality.MillibitsPerPixel(), _settings.StreamSize.Box(), watcher, accessory)
                .ConfigureAwait(true);
            if (host is null)
            {
                Log.Write(LogLevel.Warning, $"Could not wait on the Android accessory: {started}");
                _ = TryAccessoryLaterAsync(accessory);
                return;
            }
            if (Mode != ConnectionMode.Sharing)
            {
                await host.DisposeAsync().ConfigureAwait(true); // switched off meanwhile
                return;
            }
            AddHost(host);
        }
        finally
        {
            _ = _accessoryStarting.Remove(accessory);
        }
    }

    private async Task TryAccessoryLaterAsync(uint accessory)
    {
        await Task.Delay(AppTunables.UsbRetryDelay).ConfigureAwait(true);
        await TryAccessoryAsync(accessory).ConfigureAwait(true); // checks that sharing and the accessory are still there
    }

    private void OnViewerArrived(HostEntry entry, Platform platform, string name)
    {
        entry.Viewer = new ViewerItem(
            name, platform == Platform.MacOS, platform is Platform.IPadOS or Platform.Android,
            new AsyncRelayCommand(entry.Host.StopAsync));
        Viewers.Add(entry.Viewer);
        UpdateSharingStatus();
        _ = ListenAsync(); // the next computer can connect while this one pairs and streams
    }

    private void OnHostStreaming(HostEntry entry, uint displayCount)
    {
        entry.DisplayCount = displayCount;
        if (entry.Viewer is not null)
        {
            entry.Viewer.Detail = displayCount == 1
                ? Localization.Strings.SharingDisplayOne
                : LocalizedText.Format(Localization.Strings.SharingDisplayMany, displayCount);
        }
        UpdateSharingStatus();
    }

    private IEnumerable<HostEntry> ServingHosts() => _hosts.Where(h => h.Viewer is not null);

    private HostEntry? EntryOf(EngineSession session) => _hosts.Find(h => ReferenceEquals(h.Host, session));

    private void UpdateSharingStatus()
    {
        if (Mode != ConnectionMode.Sharing)
        {
            return;
        }
        List<HostEntry> serving = [.. ServingHosts()];
        IsSessionActive = serving.Count > 0;
        CanSwapRoles = _pendingSwap is null && serving.Count == 1 && serving[0].IsStreaming;
        long displays = serving.Sum(h => (long)h.DisplayCount);
        WaitingText = displays >= AppTunables.MaxVirtualMonitors
            ? Localization.Strings.SharingFull
            : serving.Count == 0 ? Localization.Strings.SharingWaitingFirst : Localization.Strings.SharingWaitingNext;
        if (_pendingSwap is not null)
        {
            return; // "swapping with …" stays
        }
        Status = serving.Count switch
        {
            0 => Localization.Strings.ConnectionStatusWaiting,
            1 => Format(
                serving[0].IsStreaming ? Localization.Strings.ConnectionStatusConnected : Localization.Strings.ConnectionStatusConnecting,
                serving[0].Viewer!.Name),
            _ => LocalizedText.Format(Localization.Strings.ConnectionStatusConnectedMany, serving.Count),
        };
    }

    // --- Viewing ------------------------------------------------------------------------------

    private async Task StartBrowsingAsync()
    {
        (EngineResult started, EngineBrowser? browser) = await EngineBrowser.StartAsync(_dispatcher).ConfigureAwait(true);
        if (browser is null)
        {
            Fail(ErrorMessages.For(started));
            return;
        }
        _browser = browser;
        browser.Found += OnFound;
        browser.Lost += OnLost;
        Status = Localization.Strings.ConnectionStatusSearching;
    }

    private void OnFound(DeviceInfo device)
    {
        if (!device.Roles.HasFlag(Roles.Host))
        {
            return;
        }
        _found[device.Label] = device;
        RebuildDevices();
        if (device.DeviceId == _autoConnectDeviceId)
        {
            _autoConnectDeviceId = null;
            _autoConnectTimeout?.Cancel();
            _ = ConnectAsync(device);
        }
    }

    private void OnLost(string label)
    {
        _ = _found.Remove(label);
        RebuildDevices();
    }

    private void RebuildDevices()
    {
        Devices.Clear();
        foreach (DeviceInfo device in _found.Values.OrderBy(d => d.Name, StringComparer.CurrentCultureIgnoreCase))
        {
            string platform = device.Platform == Platform.MacOS ? Localization.Strings.PlatformMac : Localization.Strings.PlatformWindows;
            string state = !device.IsCompatible
                ? Localization.Strings.DeviceIncompatible
                : _pairedNames.Contains(device.Name) ? Localization.Strings.DevicePaired : Localization.Strings.DeviceNew;
            Devices.Add(new DeviceItem(
                device.Name, $"{platform} · {state}", device.IsCompatible,
                new AsyncRelayCommand(() => ConnectAsync(device))));
        }
    }

    private async Task ConnectAsync(DeviceInfo device)
    {
        if (!device.IsCompatible)
        {
            ErrorMessage = Localization.Strings.ErrorIncompatible;
            return;
        }
        await _gate.WaitAsync().ConfigureAwait(true);
        IsBusy = true;
        try
        {
            if (_viewer is not null || Mode != ConnectionMode.Viewing)
            {
                return;
            }
            byte[] offered = [.. Monitors.Where(m => m.IsSelected).Select(m => m.Id)];
            if (Monitors.Count > 0 && offered.Length == 0)
            {
                ErrorMessage = Localization.Strings.ErrorNoMonitorSelected;
                return;
            }
            ErrorMessage = null;
            HotkeyGesture hotkey = _settings.ViewerHotkey;
            var options = new ViewerOptions(_deviceName, device.Address, device.Port, offered, hotkey.Modifiers, hotkey.VirtualKey);
            (EngineResult started, EngineViewer? viewer) = await EngineViewer.StartAsync(_dispatcher, options).ConfigureAwait(true);
            if (viewer is null)
            {
                ErrorMessage = ErrorMessages.For(started);
                return;
            }
            _viewer = viewer;
            _peerName = device.Name;
            _viewedHost = new ViewedHost(device.DeviceId, device.Name);
            _endedByUser = false;
            Attach(viewer);
            viewer.MonitorsChanged += () => _resumesAfterClose = true;
            viewer.VisibilityChanged += visible => IsViewerVisible = visible;
            viewer.Statistics += OnStatistics;
            IsViewerVisible = true;
            IsSessionActive = true;
            ViewerStarted?.Invoke();
            Status = Format(Localization.Strings.ConnectionStatusConnecting, _peerName);
            if (!viewer.HotkeyRegistered)
            {
                ErrorMessage = LocalizedText.Format(Localization.Strings.ErrorHotkeyUnavailable, hotkey.DisplayText);
            }
        }
        finally
        {
            IsBusy = false;
            _ = _gate.Release();
        }
    }

    private void OnStatistics(IReadOnlyList<StreamFigures> figures)
    {
        _figures = figures;
        if (_settings.ShowsStatsOverlay)
        {
            _ = ShowOverlayAsync();
        }
    }

    private async Task ShowOverlayAsync()
    {
        EngineViewer? viewer = _viewer;
        if (viewer is null)
        {
            return;
        }
        foreach (StreamFigures stream in _figures)
        {
            string text = _settings.ShowsStatsOverlay ? OverlayText(stream) : string.Empty;
            await viewer.SetOverlayAsync(stream.StreamId, text).ConfigureAwait(true);
        }
    }

    /// <summary>"60 fps · 22.1 Mbit/s · RTT 4 ms · loss 0.0% · latency 12 ms", like the Mac's overlay.</summary>
    private static string OverlayText(StreamFigures stream)
    {
        CultureInfo culture = CultureInfo.CurrentCulture;
        string latency = stream.LatencyMicroseconds > 0
            ? (stream.LatencyMicroseconds / 1000.0).ToString("F0", culture)
            : Localization.Strings.StatsUnknown;
        return LocalizedText.Format(
            Localization.Strings.StatsOverlay, stream.FramesPerSecond,
            (stream.BitrateKbps / 1000.0).ToString("F1", culture),
            (stream.RoundTripMicroseconds / 1000.0).ToString("F0", culture),
            (stream.LossPermille / 10.0).ToString("F1", culture), latency);
    }

    // --- Shared ---------------------------------------------------------------------------------

    private void Attach(EngineSession session)
    {
        session.StateChanged += state => OnState(session, state);
        session.PairingCode += code => RequestPairing(session, code);
        session.Paired += () => _ = RefreshPeersAsync();
        session.Failed += reason =>
        {
            HostProblem = EngineHealth.FromFailure(reason) ?? HostProblem;
            ErrorMessage = ErrorMessages.For(reason);
        };
        session.StreamFailed += reason => ErrorMessage = ErrorMessages.For(reason);
        session.Closed += reason => _ = OnClosedAsync(session, reason);
        session.RoleSwapRequested += peer => _ = AnswerSwapAsync(session, peer);
        session.RoleSwapAnswered += (answer, peer) => SwapAnswered(session, answer, peer);
    }

    // --- Role swap ------------------------------------------------------------------------------

    /// <summary>The role this PC takes after swapping away from the given session.</summary>
    private ConnectionMode RoleAfterSwap(EngineSession session) =>
        EntryOf(session) is not null ? ConnectionMode.Viewing : ConnectionMode.Sharing;

    /// <summary>The other computer asks to reverse the roles: agree when this PC can take its new role.</summary>
    private async Task AnswerSwapAsync(EngineSession session, string peerDeviceId)
    {
        if (EntryOf(session) is not null && ServingHosts().Count() > 1)
        {
            // With several computers connected the roles cannot be reversed.
            await session.AnswerRoleSwapAsync(RoleSwapAnswer.Busy).ConfigureAwait(true);
            return;
        }
        ConnectionMode role = RoleAfterSwap(session);
        bool able = role == ConnectionMode.Sharing
            ? await EngineHealth.CheckAsync().ConfigureAwait(true) == HostHealth.Ready
            : Monitors.Count == 0 || Monitors.Any(m => m.IsSelected);
        if (!able || _pendingSwap is not null)
        {
            await session.AnswerRoleSwapAsync(able ? RoleSwapAnswer.Busy : RoleSwapAnswer.CannotHost).ConfigureAwait(true);
            return;
        }
        _pendingSwap = new PendingSwap(role, peerDeviceId, PeerName(session));
        CanSwapRoles = false;
        Status = Format(Localization.Strings.ConnectionStatusSwapping, PeerName(session));
        await session.AnswerRoleSwapAsync(RoleSwapAnswer.Accepted).ConfigureAwait(true);
    }

    private void SwapAnswered(EngineSession session, RoleSwapAnswer answer, string peerDeviceId)
    {
        if (answer != RoleSwapAnswer.Accepted)
        {
            ErrorMessage = answer switch
            {
                RoleSwapAnswer.CannotHost => Localization.Strings.ErrorSwapCannotHost,
                RoleSwapAnswer.NoAnswer => Localization.Strings.ErrorSwapNoAnswer,
                _ => Localization.Strings.ErrorSwapBusy,
            };
            return;
        }
        _pendingSwap = new PendingSwap(RoleAfterSwap(session), peerDeviceId, PeerName(session));
        CanSwapRoles = false;
        Status = Format(Localization.Strings.ConnectionStatusSwapping, PeerName(session));
    }

    /// <summary>
    /// The session closed after an agreed swap: take the new role. Viewing connects to the same
    /// computer as soon as it advertises itself as a host; it is already paired, so no code.
    /// </summary>
    private async Task PerformSwapAsync()
    {
        PendingSwap? swap = _pendingSwap;
        _pendingSwap = null;
        if (swap is null)
        {
            return;
        }
        await SwitchModeAsync(swap.Role).ConfigureAwait(true);
        if (swap.Role != ConnectionMode.Viewing || Mode != ConnectionMode.Viewing)
        {
            return;
        }
        Status = Format(Localization.Strings.ConnectionStatusSwapping, swap.PeerName);
        if (!await AwaitHostAsync(swap.PeerDeviceId, AppTunables.SwapReconnectTimeout).ConfigureAwait(true))
        {
            ErrorMessage = Localization.Strings.ErrorSwapNotFound;
            Status = Localization.Strings.ConnectionStatusSearching;
        }
    }

    /// <summary>
    /// Connects to this device as soon as Bonjour finds it as a host. Returns false when the
    /// timeout passed first; a null timeout waits as long as viewing stays on.
    /// </summary>
    private async Task<bool> AwaitHostAsync(string deviceId, TimeSpan? timeout)
    {
        DeviceInfo? known = _found.Values.FirstOrDefault(d => d.DeviceId == deviceId);
        if (known is not null)
        {
            await ConnectAsync(known).ConfigureAwait(true);
            return true;
        }
        _autoConnectDeviceId = deviceId;
        _autoConnectTimeout?.Cancel();
        _autoConnectTimeout?.Dispose();
        _autoConnectTimeout = new CancellationTokenSource();
        if (timeout is null)
        {
            return true; // OnFound connects when it appears
        }
        try
        {
            await Task.Delay(timeout.Value, _autoConnectTimeout.Token).ConfigureAwait(true);
        }
        catch (TaskCanceledException)
        {
            return true; // found, or the mode changed
        }
        if (_autoConnectDeviceId != deviceId)
        {
            return true;
        }
        _autoConnectDeviceId = null;
        return false;
    }

    // --- Interruptions --------------------------------------------------------------------------

    // Raised on a system events thread.
    private void OnPowerModeChanged(object? sender, Microsoft.Win32.PowerModeChangedEventArgs e)
    {
        if (e.Mode == Microsoft.Win32.PowerModes.Suspend)
        {
            _ = _dispatcher.BeginInvoke(() => _ = SuspendForSleepAsync());
        }
        else if (e.Mode == Microsoft.Win32.PowerModes.Resume)
        {
            _ = _dispatcher.BeginInvoke(() => _ = RefreshAfterWakeAsync());
        }
    }

    /// <summary>Tells the other computer this PC is going away for a while; a viewer comes back after wake.</summary>
    private async Task SuspendForSleepAsync()
    {
        if (_viewer is not null)
        {
            _resumesAfterClose = true;
            await _viewer.SuspendAsync().ConfigureAwait(true);
        }
        foreach (HostEntry entry in _hosts.ToList())
        {
            await entry.Host.SuspendAsync().ConfigureAwait(true);
        }
    }

    /// <summary>
    /// Sockets and Bonjour registrations from before sleep may be stale: sharing without a viewer
    /// listens and advertises anew, and viewing browses anew (a pending reconnect is kept).
    /// </summary>
    private async Task RefreshAfterWakeAsync()
    {
        if (Mode == ConnectionMode.Sharing)
        {
            // A new advertisement with the next listener; the connected computers go on.
            if (_advertiser is not null)
            {
                await _advertiser.DisposeAsync().ConfigureAwait(true);
                _advertiser = null;
            }
            HostEntry? listener = _hosts.Find(h => h.IsListener);
            if (listener is not null)
            {
                await listener.Host.StopAsync().ConfigureAwait(true); // OnClosedAsync listens again
            }
            else
            {
                await ListenAsync().ConfigureAwait(true);
            }
        }
        else if (Mode == ConnectionMode.Viewing && _viewer is null && _browser is not null)
        {
            EngineBrowser browser = _browser;
            _browser = null;
            _found.Clear();
            Devices.Clear();
            await browser.DisposeAsync().ConfigureAwait(true);
            string status = Status;
            await StartBrowsingAsync().ConfigureAwait(true);
            if (_autoConnectDeviceId is not null)
            {
                Status = status; // still "waiting for … to come back"
            }
        }
    }

    /// <summary>Whether a closed viewer session should wait for its host and connect again.</summary>
    private bool ComesBack(CoreResult reason) =>
        _viewedHost is not null && !_endedByUser && Mode == ConnectionMode.Viewing &&
        (_resumesAfterClose || reason is CoreResult.PeerSleeping or CoreResult.TimedOut or CoreResult.Network);

    private void RegisterSwapHotkey()
    {
        if (_swapHotkey is not null)
        {
            return;
        }
        _swapHotkey = GlobalHotkey.Register(_settings.SwapHotkey, () => SwapRolesCommand.Execute(null));
        IsSwapHotkeyTaken = _swapHotkey is null;
    }

    /// <summary>The overlay shortcut is optional: a taken one leaves the Settings switch.</summary>
    private void RegisterStatsHotkey() =>
        _statsHotkey ??= GlobalHotkey.Register(
            _settings.StatsHotkey, () => _settings.ShowsStatsOverlay = !_settings.ShowsStatsOverlay);

    /// <summary>
    /// Host pause (fork addition): blanks every viewer's streams or brings the picture back.
    /// </summary>
    private async Task ToggleStreamsPausedAsync()
    {
        _streamsPaused = !_streamsPaused;
        foreach (HostEntry entry in ServingHosts())
        {
            await entry.Host.SetPausedAsync(_streamsPaused).ConfigureAwait(true);
        }
    }

    private void UnregisterStatsHotkey()
    {
        _statsHotkey?.Dispose();
        _statsHotkey = null;
    }

    private void UnregisterSwapHotkey()
    {
        _swapHotkey?.Dispose();
        _swapHotkey = null;
        IsSwapHotkeyTaken = false;
    }

    private string PeerName(EngineSession session) =>
        session.PeerName.Length > 0 ? session.PeerName : _peerName;

    private void OnState(EngineSession session, SessionState state)
    {
        if (EntryOf(session) is { } entry)
        {
            if (state == SessionState.Streaming)
            {
                entry.IsStreaming = true;
                _ = RefreshPeersAsync();
            }
            UpdateSharingStatus();
            return;
        }
        switch (state)
        {
            case SessionState.Handshaking:
            case SessionState.AwaitingPairing:
            case SessionState.Negotiating:
                IsSessionActive = true;
                Status = Format(Localization.Strings.ConnectionStatusConnecting, PeerName(session));
                break;
            case SessionState.Streaming:
                Status = Format(Localization.Strings.ConnectionStatusConnected, PeerName(session));
                IsStreaming = true;
                CanSwapRoles = _pendingSwap is null;
                RegisterStatsHotkey();
                _ = RefreshPeersAsync();
                break;
            default:
                break;
        }
    }

    private async Task OnClosedAsync(EngineSession session, CoreResult reason)
    {
        DropPairing(session);
        bool viewerComesBack = ReferenceEquals(session, _viewer) && ComesBack(reason);
        // An iPad on the cable that is not waiting for a computer refuses, and an Android accessory
        // goes away unopened: that is no failure.
        bool usbAttempt = EntryOf(session) is { Viewer: null, Host.IsCable: true };
        // A peer leaving or going to sleep is not an error; neither is a drop that heals by itself.
        if (reason is not (CoreResult.Ok or CoreResult.ConnectionClosed or CoreResult.PeerSleeping) && !viewerComesBack &&
            !usbAttempt)
        {
            ErrorMessage = ErrorMessages.For(reason);
        }
        if (ReferenceEquals(session, _viewer))
        {
            ViewedHost? host = _viewedHost;
            _viewer = null;
            _viewedHost = null;
            _resumesAfterClose = false;
            _figures = [];
            IsSessionActive = false;
            IsStreaming = false;
            CanSwapRoles = false;
            UnregisterStatsHotkey();
            await session.DisposeAsync().ConfigureAwait(true);
            if (_pendingSwap is not null)
            {
                await PerformSwapAsync().ConfigureAwait(true);
            }
            else if (viewerComesBack && host is not null)
            {
                Status = Format(Localization.Strings.ConnectionStatusReconnecting, host.Name);
                await Task.Delay(AppTunables.ReconnectDelay).ConfigureAwait(true);
                if (Mode == ConnectionMode.Viewing && _viewer is null)
                {
                    _ = await AwaitHostAsync(host.DeviceId, null).ConfigureAwait(true);
                }
            }
            else if (Mode == ConnectionMode.Viewing)
            {
                Status = Localization.Strings.ConnectionStatusSearching;
            }
        }
        else if (EntryOf(session) is { } entry)
        {
            _ = _hosts.Remove(entry);
            if (entry.Viewer is not null)
            {
                _ = Viewers.Remove(entry.Viewer);
            }
            await session.DisposeAsync().ConfigureAwait(true);
            if (_pendingSwap is not null && entry.Viewer is not null)
            {
                await PerformSwapAsync().ConfigureAwait(true);
                return;
            }
            UpdateSharingStatus();
            if (entry.Host.UsbDeviceId is uint device)
            {
                // The iPad is tried again; it only answers when its user picks the cable again.
                await TryUsbLaterAsync(device).ConfigureAwait(true);
            }
            else if (entry.Host.AccessoryId is uint accessory)
            {
                // Still plugged in: wait for the user to pick the cable on the device again.
                await TryAccessoryLaterAsync(accessory).ConfigureAwait(true);
            }
            else if (entry.Viewer is null && Mode == ConnectionMode.Sharing)
            {
                // The listener ended (a viewer turned away, a failure, wake): another one takes its place.
                await Task.Delay(AppTunables.RelistenDelay).ConfigureAwait(true);
                await ListenAsync().ConfigureAwait(true);
            }
        }
    }

    // --- Pairing ------------------------------------------------------------------------------

    /// <summary>Shows the code at once, or after the codes before it were answered.</summary>
    private void RequestPairing(EngineSession session, uint code)
    {
        _pairingQueue.Add(new PairingRequest(session, code, PeerName(session)));
        if (_pairingQueue.Count == 1)
        {
            ShowNextPairing();
        }
    }

    private void ShowNextPairing()
    {
        if (_pairingQueue.Count == 0)
        {
            return;
        }
        PairingRequest request = _pairingQueue[0];
        _pairing.Show(request.Code, request.PeerName, accepted =>
        {
            if (_pairingQueue.Count == 0 || !ReferenceEquals(_pairingQueue[0], request))
            {
                return;
            }
            _pairingQueue.RemoveAt(0);
            _ = request.Session.ConfirmPairingAsync(accepted);
            ShowNextPairing();
        });
    }

    /// <summary>The session asking ended: its code goes away, unanswered.</summary>
    private void DropPairing(EngineSession session)
    {
        int index = _pairingQueue.FindIndex(r => ReferenceEquals(r.Session, session));
        if (index < 0)
        {
            return;
        }
        _pairingQueue.RemoveAt(index);
        if (index == 0)
        {
            _pairing.Close();
            ShowNextPairing();
        }
    }

    private async Task RefreshPeersAsync()
    {
        IReadOnlyList<PairedDevice> peers = await PairedDevices.ListAsync().ConfigureAwait(true);
        Peers.Clear();
        _pairedNames.Clear();
        foreach (PairedDevice peer in peers.OrderBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase))
        {
            if (!peer.Revoked)
            {
                _ = _pairedNames.Add(peer.Name);
            }
            string detail = peer.Revoked
                ? Localization.Strings.PeerRevoked
                : LocalizedText.Format(Localization.Strings.PeerLastSeen, peer.LastSeen.ToLocalTime().ToString("g", CultureInfo.CurrentCulture));
            Peers.Add(new PeerItem(
                peer.Name, detail, peer.Revoked,
                new AsyncRelayCommand(() => ChangeAsync(PairedDevices.RevokeAsync(peer.PublicKey))),
                new AsyncRelayCommand(() => ChangeAsync(PairedDevices.RemoveAsync(peer.PublicKey)))));
        }
        RebuildDevices();
    }

    private async Task ChangeAsync(Task<EngineResult> change)
    {
        EngineResult result = await change.ConfigureAwait(true);
        if (result != EngineResult.Ok)
        {
            ErrorMessage = ErrorMessages.For(result);
        }
        await RefreshPeersAsync().ConfigureAwait(true);
    }

    private async Task LoadMonitorsAsync()
    {
        if (!IsEngineAvailable)
        {
            return;
        }
        IReadOnlyList<MonitorInfo> monitors = await EngineMonitors.ListAsync().ConfigureAwait(true);
        foreach (MonitorTile tile in Monitors)
        {
            tile.PropertyChanged -= OnMonitorSelectionChanged;
        }
        Monitors.Clear();
        foreach (MonitorTile tile in MonitorLayout.Tiles(monitors, [.. _settings.ExcludedMonitors]))
        {
            tile.PropertyChanged += OnMonitorSelectionChanged;
            Monitors.Add(tile);
        }
    }

    private void OnMonitorSelectionChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName != nameof(MonitorTile.IsSelected) || sender is not MonitorTile tile)
        {
            return;
        }
        var excluded = new HashSet<string>(_settings.ExcludedMonitors);
        _ = tile.IsSelected ? excluded.Remove(tile.Key) : excluded.Add(tile.Key);
        _settings.ExcludedMonitors = [.. excluded];
    }

    private async Task RefreshNetworkAsync() =>
        IsNetworkPublic = await Services.NetworkProfile.IsAnyNetworkPublicAsync().ConfigureAwait(true);

    private void OnSettingChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(AppSettings.ShowsStatsOverlay):
                _ = ShowOverlayAsync(); // shows the latest figures at once, or clears them
                break;
            case nameof(AppSettings.ViewerHotkey):
                OnPropertyChanged(nameof(HotkeyHint));
                break;
            case nameof(AppSettings.SwapHotkey):
                OnPropertyChanged(nameof(SwapHint));
                break;
            default:
                break;
        }
    }

    private void Fail(string message)
    {
        Log.Write(LogLevel.Warning, message);
        ErrorMessage = message;
        Mode = ConnectionMode.Off;
        Status = Localization.Strings.ConnectionStatusOff;
    }

    private async Task StopAllAsync()
    {
        _pairingQueue.Clear();
        _pairing.Close();
        _autoConnectDeviceId = null;
        _autoConnectTimeout?.Cancel();
        _endedByUser = true;
        _viewedHost = null;
        IsStreaming = false;
        CanSwapRoles = false;
        UnregisterSwapHotkey();
        UnregisterStatsHotkey();
        _found.Clear();
        Devices.Clear();
        EngineBrowser? browser = _browser;
        EngineAdvertiser? advertiser = _advertiser;
        EngineUsbWatcher? usbWatcher = _usbWatcher;
        _usbWatcher = null;
        _usbDevices.Clear();
        EngineAndroidWatcher? androidWatcher = _androidWatcher;
        _androidWatcher = null;
        _accessories.Clear();
        List<HostEntry> hosts = [.. _hosts];
        EngineViewer? viewer = _viewer;
        _browser = null;
        _advertiser = null;
        _hosts.Clear();
        Viewers.Clear();
        _viewer = null;
        _figures = [];
        IsSessionActive = false;
        if (browser is not null)
        {
            await browser.DisposeAsync().ConfigureAwait(true);
        }
        if (advertiser is not null)
        {
            await advertiser.DisposeAsync().ConfigureAwait(true);
        }
        if (usbWatcher is not null)
        {
            await usbWatcher.DisposeAsync().ConfigureAwait(true);
        }
        foreach (HostEntry entry in hosts)
        {
            await entry.Host.DisposeAsync().ConfigureAwait(true);
        }
        if (androidWatcher is not null)
        {
            // After the hosts: no accessory is switched while their sessions close.
            await androidWatcher.DisposeAsync().ConfigureAwait(true);
        }
        if (viewer is not null)
        {
            await viewer.DisposeAsync().ConfigureAwait(true);
        }
    }

    private static string Format(string format, string value) => LocalizedText.Format(format, value);
}
