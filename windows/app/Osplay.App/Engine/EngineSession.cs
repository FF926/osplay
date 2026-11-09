using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Windows.Threading;
using Osplay.App.Diagnostics;
using Osplay.App.Interop;

namespace Osplay.App.Engine;

/// <summary>One stream's figures for the stats overlay (DEVPLAN: fps, bitrate, RTT, loss, latency).</summary>
internal sealed record StreamFigures(
    byte StreamId, uint FramesPerSecond, uint BitrateKbps, uint RoundTripMicroseconds, ushort LossPermille,
    uint LatencyMicroseconds);

/// <summary>
/// One host or viewer session in the engine, started with the stored identity (the engine answers
/// trust and stores pairings). Every native call runs through <see cref="Native"/>; events arrive
/// on the dispatcher's thread. Dispose after <see cref="Closed"/> or to abandon the session.
/// </summary>
internal abstract class EngineSession : IAsyncDisposable
{
    private readonly Dispatcher _dispatcher;
    private GCHandle _self;
    private string _peerName = string.Empty;

    protected EngineSession(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        _self = GCHandle.Alloc(this);
    }

    /// <summary>The engine handle; 0 before start and after dispose. Used on the native queue only.</summary>
    protected nint Handle { get; set; }

    protected nint Context => GCHandle.ToIntPtr(_self);

    public string PeerName => _peerName;

    /// <summary>The other computer introduced itself (before any pairing code).</summary>
    public event Action<Platform, string>? PeerArrived;

    public event Action<SessionState>? StateChanged;
    public event Action<uint>? PairingCode;
    public event Action? Paired;
    /// <summary>The session ended; the reason is the core's (Ok for an orderly close).</summary>
    public event Action<CoreResult>? Closed;
    /// <summary>The engine itself failed (helper, capture...); Closed follows.</summary>
    public event Action<EngineResult>? Failed;

    /// <summary>Viewer: one stream could not be shown; the session continues.</summary>
    public event Action<EngineResult>? StreamFailed;

    /// <summary>The other computer asks to reverse the roles; the argument is its device ID (hex).</summary>
    public event Action<string>? RoleSwapRequested;

    /// <summary>The answer to <see cref="RequestRoleSwapAsync"/>; after Accepted the session closes.</summary>
    public event Action<RoleSwapAnswer, string>? RoleSwapAnswered;

    public abstract Task ConfirmPairingAsync(bool accepted);

    /// <summary>Asks the other computer to reverse the roles; PeerTooOld when its Osplay cannot.</summary>
    public Task<EngineResult> RequestRoleSwapAsync() =>
        Native.Run(() => Handle == 0 ? EngineResult.SessionFailed : RequestRoleSwap(Handle));

    /// <summary>Answers <see cref="RoleSwapRequested"/>; Accepted closes the session.</summary>
    public Task AnswerRoleSwapAsync(RoleSwapAnswer answer) => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = AnswerRoleSwap(Handle, answer);
        }
    });

    protected abstract EngineResult RequestRoleSwap(nint handle);

    protected abstract EngineResult AnswerRoleSwap(nint handle, RoleSwapAnswer answer);

    public abstract Task StopAsync();

    /// <summary>Ends the session telling the other computer this PC is going to sleep.</summary>
    public Task SuspendAsync() => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = Suspend(Handle);
        }
    });

    protected abstract EngineResult Suspend(nint handle);

    public async ValueTask DisposeAsync()
    {
        await Native.Run(() =>
        {
            if (Handle != 0)
            {
                Destroy(Handle); // joins the engine's threads; no callback runs afterwards
                Handle = 0;
            }
        }).ConfigureAwait(true);
        if (_self.IsAllocated)
        {
            _self.Free();
        }
    }

    protected abstract void Destroy(nint handle);

    protected void Post(Action action) => _dispatcher.BeginInvoke(action);

    protected static EngineSession From(nint context) => (EngineSession)GCHandle.FromIntPtr(context).Target!;

    protected void RaiseStreamFailed(EngineResult reason) => StreamFailed?.Invoke(reason);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static void OnState(nint context, SessionState state, CoreResult reason)
    {
        var self = From(context);
        self.Post(() =>
        {
            self.StateChanged?.Invoke(state);
            if (state == SessionState.Closed)
            {
                self.Closed?.Invoke(reason);
            }
        });
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static unsafe void OnPairingCode(nint context, uint code, byte* key)
    {
        var self = From(context);
        self.Post(() => self.PairingCode?.Invoke(code));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static unsafe void OnPaired(nint context, byte* key)
    {
        var self = From(context);
        self.Post(() => self.Paired?.Invoke());
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static void OnFailed(nint context, EngineResult reason)
    {
        var self = From(context);
        Log.Write(LogLevel.Error, $"The engine ended the session: {reason}");
        self.Post(() => self.Failed?.Invoke(reason));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static unsafe void OnPeer(nint context, Platform platform, byte* name)
    {
        var self = From(context);
        string text = Utf8.Read(name);
        self.Post(() =>
        {
            self._peerName = text;
            self.PeerArrived?.Invoke(platform, text);
        });
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static unsafe void OnRoleSwapRequest(nint context, byte* peerDeviceId)
    {
        var self = From(context);
        string peer = DeviceIdOf(peerDeviceId);
        self.Post(() => self.RoleSwapRequested?.Invoke(peer));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static unsafe void OnRoleSwapAnswer(nint context, RoleSwapAnswer answer, byte* peerDeviceId)
    {
        var self = From(context);
        string peer = DeviceIdOf(peerDeviceId);
        self.Post(() => self.RoleSwapAnswered?.Invoke(answer, peer));
    }

    /// <summary>Lowercase hex, as Bonjour advertisements carry it (DeviceInfo.DeviceId).</summary>
    private static unsafe string DeviceIdOf(byte* deviceId) =>
        Convert.ToHexStringLower(new ReadOnlySpan<byte>(deviceId, NativeSizes.DeviceIdSize));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    protected static unsafe void OnLog(nint context, LogLevel level, byte* message) => Log.Write(level, Utf8.Read(message));
}

/// <summary>Windows shares its screen: a virtual monitor streamed to one viewer.</summary>
internal sealed unsafe class EngineHost : EngineSession
{
    private EngineHost(Dispatcher dispatcher)
        : base(dispatcher)
    {
    }

    /// <summary>The TCP port to advertise.</summary>
    public ushort ControlPort { get; private set; }

    /// <summary>The virtual monitors stream; the argument is how many this viewer got.</summary>
    public event Action<uint>? Streaming;

    /// <summary>The iPad on the USB cable this session reaches; null for one listening on the network.</summary>
    public uint? UsbDeviceId { get; private set; }

    /// <summary>The Android accessory on the USB cable this session waits on (TAB-05), or null.</summary>
    public uint? AccessoryId { get; private set; }

    /// <summary>The session reaches a device on the USB cable rather than listening on the network.</summary>
    public bool IsCable => UsbDeviceId is not null || AccessoryId is not null;

    public static Task<(EngineResult Result, EngineHost? Host)> StartAsync(
        Dispatcher dispatcher, string deviceName, uint millibitsPerPixel, ushort controlPort,
        (uint Width, uint Height) streamBox) =>
        StartAsync(deviceName, millibitsPerPixel, controlPort, streamBox, new EngineHost(dispatcher));

    /// <summary>
    /// Reaches the iPad on the USB cable through the Apple Mobile Device Service; the session closes
    /// with Network when the iPad is not waiting for a computer (TAB-03).
    /// </summary>
    public static Task<(EngineResult Result, EngineHost? Host)> StartUsbAsync(
        Dispatcher dispatcher, string deviceName, uint millibitsPerPixel, (uint Width, uint Height) streamBox,
        uint usbDeviceId) =>
        StartAsync(deviceName, millibitsPerPixel, 0, streamBox, new EngineHost(dispatcher) { UsbDeviceId = usbDeviceId });

    /// <summary>
    /// Waits on the Android accessory `accessoryId` reported by `watcher` until Osplay on the device
    /// opens it; InvalidArgument when the accessory is gone already (TAB-05).
    /// </summary>
    public static Task<(EngineResult Result, EngineHost? Host)> StartAccessoryAsync(
        Dispatcher dispatcher, string deviceName, uint millibitsPerPixel, (uint Width, uint Height) streamBox,
        EngineAndroidWatcher watcher, uint accessoryId) =>
        StartAsync(
            deviceName, millibitsPerPixel, 0, streamBox,
            new EngineHost(dispatcher) { AccessoryId = accessoryId }, watcher.Handle);

    private static Task<(EngineResult Result, EngineHost? Host)> StartAsync(
        string deviceName, uint millibitsPerPixel, ushort controlPort,
        (uint Width, uint Height) streamBox, EngineHost created, nint androidWatcher = 0)
    {
        return Native.Run(() =>
        {
            nint name = Utf8.Allocate(deviceName);
            try
            {
                var config = new NativeHostConfig
                {
                    SecretKey = 0,
                    DeviceName = name,
                    DumpDirectory = 0,
                    MillibitsPerPixel = millibitsPerPixel,
                    ControlPort = controlPort,
                    MaxStreamWidth = streamBox.Width,
                    MaxStreamHeight = streamBox.Height,
                };
                var callbacks = new NativeHostCallbacks
                {
                    Context = created.Context,
                    OnState = &OnState,
                    OnTrustQuery = null, // the stored identity: the engine answers trust
                    OnPairingCode = &OnPairingCode,
                    OnPaired = &OnPaired,
                    OnStreaming = &OnStreaming,
                    OnFailed = &OnFailed,
                    OnStats = 0,
                    OnLog = &OnLog,
                    OnPeer = &OnPeer,
                    OnRoleSwapRequest = &OnRoleSwapRequest,
                    OnRoleSwapAnswer = &OnRoleSwapAnswer,
                };
                nint handle;
                EngineResult result = (created.UsbDeviceId, created.AccessoryId) switch
                {
                    (uint device, _) => NativeMethods.HostStartUsb(&config, &callbacks, device, &handle),
                    (_, uint accessory) => NativeMethods.HostStartAccessory(&config, &callbacks, androidWatcher, accessory, &handle),
                    _ => NativeMethods.HostStart(&config, &callbacks, &handle),
                };
                if (result != EngineResult.Ok)
                {
                    return (result, (EngineHost?)null);
                }
                created.Handle = handle;
                ushort port;
                _ = NativeMethods.HostControlPort(handle, &port);
                created.ControlPort = port;
                return (result, created);
            }
            finally
            {
                Utf8.Free(name);
            }
        });
    }

    public override Task ConfirmPairingAsync(bool accepted) => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = NativeMethods.HostConfirmPairing(Handle, accepted ? (byte)1 : (byte)0);
        }
    });

    public override Task StopAsync() => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = NativeMethods.HostStop(Handle);
        }
    });

    protected override void Destroy(nint handle) => NativeMethods.HostDestroy(handle);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnStreaming(nint context, uint displayCount, int left, int top, int right, int bottom)
    {
        var self = (EngineHost)From(context);
        self.Post(() => self.Streaming?.Invoke(displayCount));
    }

    protected override EngineResult RequestRoleSwap(nint handle) => NativeMethods.HostRequestRoleSwap(handle);

    protected override EngineResult Suspend(nint handle) => NativeMethods.HostSuspend(handle);

    /// <summary>Host pause (fork addition): viewers' streams encode black until resumed.</summary>
    public Task SetPausedAsync(bool paused) => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = NativeMethods.HostSetPaused(Handle, paused ? 1 : 0);
        }
    });

    protected override EngineResult AnswerRoleSwap(nint handle, RoleSwapAnswer answer) =>
        NativeMethods.HostAnswerRoleSwap(handle, answer);
}

/// <summary>Where and how a viewer session runs.</summary>
internal sealed record ViewerOptions(
    string DeviceName, string Address, ushort Port, IReadOnlyList<byte> MonitorIds, uint HotkeyModifiers,
    uint HotkeyKey);

/// <summary>Windows shows a remote screen on its monitors.</summary>
internal sealed unsafe class EngineViewer : EngineSession
{
    private EngineViewer(Dispatcher dispatcher)
        : base(dispatcher)
    {
    }

    public bool HotkeyRegistered { get; private set; }

    /// <summary>The streams were hidden (false) or shown (true), by the hotkey or by the shell.</summary>
    public event Action<bool>? VisibilityChanged;

    /// <summary>The figures of every stream, about once a second.</summary>
    public event Action<IReadOnlyList<StreamFigures>>? Statistics;

    /// <summary>An offered monitor changed its mode; the session closes so it can start over.</summary>
    public event Action? MonitorsChanged;

    public static Task<(EngineResult Result, EngineViewer? Viewer)> StartAsync(
        Dispatcher dispatcher, ViewerOptions options)
    {
        var created = new EngineViewer(dispatcher);
        return Native.Run(() =>
        {
            nint name = Utf8.Allocate(options.DeviceName);
            nint host = Utf8.Allocate(options.Address);
            byte[] monitors = [.. options.MonitorIds];
            try
            {
                fixed (byte* monitorIds = monitors)
                {
                    var config = new NativeViewerConfig
                    {
                        SecretKey = 0,
                        DeviceName = name,
                        Host = host,
                        Port = options.Port,
                        MonitorIds = monitors.Length == 0 ? 0 : (nint)monitorIds,
                        MonitorIdCount = (nuint)monitors.Length,
                        HotkeyModifiers = options.HotkeyModifiers,
                        HotkeyKey = options.HotkeyKey,
                    };
                    var callbacks = new NativeViewerCallbacks
                    {
                        Context = created.Context,
                        OnState = &OnState,
                        OnTrustQuery = null, // the stored identity: the engine answers trust
                        OnPairingCode = &OnPairingCode,
                        OnPaired = &OnPaired,
                        OnVisibility = &OnVisibility,
                        OnStreamFailed = &OnStreamFailed,
                        OnStats = &OnStats,
                        OnLog = &OnLog,
                        OnPeer = &OnPeer,
                        OnRoleSwapRequest = &OnRoleSwapRequest,
                        OnRoleSwapAnswer = &OnRoleSwapAnswer,
                        OnMonitorsChanged = &OnMonitorsChanged,
                    };
                    nint handle;
                    EngineResult result = NativeMethods.ViewerStart(&config, &callbacks, &handle);
                    if (result != EngineResult.Ok)
                    {
                        return (result, (EngineViewer?)null);
                    }
                    created.Handle = handle;
                    byte registered;
                    _ = NativeMethods.ViewerHotkeyRegistered(handle, &registered);
                    created.HotkeyRegistered = registered == 1;
                    return (result, created);
                }
            }
            finally
            {
                Utf8.Free(name);
                Utf8.Free(host);
            }
        });
    }

    public override Task ConfirmPairingAsync(bool accepted) => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = NativeMethods.ViewerConfirmPairing(Handle, accepted ? (byte)1 : (byte)0);
        }
    });

    public override Task StopAsync() => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = NativeMethods.ViewerStop(Handle);
        }
    });

    public Task SetVisibleAsync(bool visible) => Native.Run(() =>
    {
        if (Handle != 0)
        {
            _ = NativeMethods.ViewerSetVisible(Handle, visible ? (byte)1 : (byte)0);
        }
    });

    /// <summary>Draws one line in the stream's corner; empty hides it.</summary>
    public Task SetOverlayAsync(byte streamId, string text) => Native.Run(() =>
    {
        if (Handle == 0)
        {
            return;
        }
        nint line = Utf8.Allocate(text);
        try
        {
            _ = NativeMethods.ViewerSetOverlay(Handle, streamId, (byte*)line);
        }
        finally
        {
            Utf8.Free(line);
        }
    });

    protected override void Destroy(nint handle) => NativeMethods.ViewerDestroy(handle);

    protected override EngineResult RequestRoleSwap(nint handle) => NativeMethods.ViewerRequestRoleSwap(handle);

    protected override EngineResult Suspend(nint handle) => NativeMethods.ViewerSuspend(handle);

    protected override EngineResult AnswerRoleSwap(nint handle, RoleSwapAnswer answer) =>
        NativeMethods.ViewerAnswerRoleSwap(handle, answer);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnStreamFailed(nint context, byte streamId, EngineResult reason)
    {
        var self = (EngineViewer)From(context);
        Log.Write(LogLevel.Warning, $"Stream {streamId} stopped: {reason}");
        self.Post(() => self.RaiseStreamFailed(reason));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnMonitorsChanged(nint context)
    {
        var self = (EngineViewer)From(context);
        Log.Write(LogLevel.Info, "An offered monitor changed its mode; connecting again");
        self.Post(() => self.MonitorsChanged?.Invoke());
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnVisibility(nint context, byte visible)
    {
        var self = (EngineViewer)From(context);
        self.Post(() => self.VisibilityChanged?.Invoke(visible == 1));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnStats(nint context, NativeSessionStats* stats)
    {
        var self = (EngineViewer)From(context);
        var figures = new List<StreamFigures>();
        for (nuint i = 0; i < stats->StreamCount; i++)
        {
            NativeStreamStats stream = stats->Streams[i];
            figures.Add(new StreamFigures(
                stream.StreamId, stream.FramesPerSecond, stream.BitrateKbps, stats->RoundTripMicroseconds,
                stream.LossPermille, stream.LatencyMicroseconds));
        }
        self.Post(() => self.Statistics?.Invoke(figures));
    }
}
