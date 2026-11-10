using System.Runtime.InteropServices;

namespace Osplay.App.Interop;

// Mirrors of the plain C types in osplay.h and engine.h. Values are stable (never renumbered).

/// <summary>osplay_engine_result.</summary>
internal enum EngineResult
{
    Ok = 0,
    InvalidArgument = 1,
    HelperUnavailable = 2,
    HelperAccessDenied = 3,
    HelperNotTrusted = 4,
    ProtocolMismatch = 5,
    ConnectionLost = 6,
    VddNotInstalled = 7,
    DeviceControlFailed = 8,
    ConfigWriteFailed = 9,
    Rejected = 10,
    Internal = 11,
    MonitorNotFound = 12,
    ModeChangeFailed = 13,
    HelperBusy = 14,
    SessionFailed = 15,
    CaptureFailed = 16,
    EncoderUnavailable = 17,
    EncoderFailed = 18,
    DecoderUnavailable = 19,
    PresentationFailed = 20,
    KeyStoreUnavailable = 21,
    KeyStoreDamaged = 22,
    DiscoveryUnavailable = 23,
    PeerTooOld = 24,
}

/// <summary>osplay_role_swap_answer.</summary>
internal enum RoleSwapAnswer
{
    Accepted = 1,
    CannotHost = 2,
    Busy = 3,
    NoAnswer = 4,
}

/// <summary>osplay_result: the core's reason a session closed.</summary>
internal enum CoreResult
{
    Ok = 0,
    UnsupportedVersion = 5,
    TimedOut = 17,
    ConnectionClosed = 18,
    NotTrusted = 21,
    PairingRejected = 22,
    PeerBusy = 23,
    Network = 25,
    PeerSleeping = 27,
}

/// <summary>osplay_session_state.</summary>
internal enum SessionState
{
    Connecting = 1,
    Handshaking = 2,
    AwaitingPairing = 3,
    Negotiating = 4,
    Streaming = 5,
    Paused = 6,
    Closed = 7,
}

/// <summary>osplay_trust.</summary>
internal enum Trust
{
    Trusted = 1,
    Unknown = 2,
    Blocked = 3,
}

/// <summary>osplay_log_level.</summary>
internal enum LogLevel
{
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
}

/// <summary>osplay_platform.</summary>
internal enum Platform
{
    MacOS = 1,
    Windows = 2,
    IPadOS = 3,
    Android = 4,
}

/// <summary>OSPLAY_ROLE_* bits.</summary>
[Flags]
internal enum Roles : byte
{
    None = 0,
    Host = 1,
    Viewer = 2,
}

internal static class NativeSizes
{
    public const int KeySize = 32;
    public const int DeviceIdSize = 16;
    public const int NameSize = 65;
    public const int AddressSize = 64;
    public const int MaxStreams = 4;
    public const int MaxMonitors = 4;
    public const uint ProtocolVersion = 1;
}

/// <summary>osplay_advertisement.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeAdvertisement
{
    public uint ProtocolVersion;
    public Platform Platform;
    public Roles Roles;
    public byte ProtocolMinor;
    public fixed byte DeviceId[NativeSizes.DeviceIdSize];
    public fixed byte Name[NativeSizes.NameSize];
}

/// <summary>osplay_engine_device.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeDevice
{
    public fixed byte Label[NativeSizes.NameSize];
    public NativeAdvertisement Advertisement;
    public fixed byte Address[NativeSizes.AddressSize];
    public ushort Port;
}

/// <summary>osplay_engine_peer.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativePeer
{
    public fixed byte PublicKey[NativeSizes.KeySize];
    public fixed byte Name[NativeSizes.NameSize];
    public byte Revoked;
    public long LastSeenUnix;
}

/// <summary>osplay_engine_browser_callbacks.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeBrowserCallbacks
{
    public nint Context;
    public delegate* unmanaged[Cdecl]<nint, NativeDevice*, void> OnFound;
    public delegate* unmanaged[Cdecl]<nint, byte*, void> OnLost;
}

/// <summary>osplay_usb_callbacks.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeUsbCallbacks
{
    public nint Context;
    public delegate* unmanaged[Cdecl]<nint, uint, void> OnAttached;
    public delegate* unmanaged[Cdecl]<nint, uint, void> OnDetached;
}

/// <summary>osplay_engine_android_callbacks.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeAndroidCallbacks
{
    public nint Context;
    public delegate* unmanaged[Cdecl]<nint, uint, void> OnAttached;
    public delegate* unmanaged[Cdecl]<nint, uint, void> OnDetached;
    public delegate* unmanaged[Cdecl]<nint, void> OnDebuggingOn;
    public delegate* unmanaged[Cdecl]<nint, LogLevel, byte*, void> OnLog;
}

/// <summary>osplay_engine_host_config.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeHostConfig
{
    public nint SecretKey;
    public nint DeviceName;
    public nint DumpDirectory;
    public uint MillibitsPerPixel;
    public ushort ControlPort;
    public uint MaxStreamWidth;
    public uint MaxStreamHeight;
}

/// <summary>osplay_engine_host_callbacks.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeHostCallbacks
{
    public nint Context;
    public delegate* unmanaged[Cdecl]<nint, SessionState, CoreResult, void> OnState;
    public delegate* unmanaged[Cdecl]<nint, byte*, Trust> OnTrustQuery;
    public delegate* unmanaged[Cdecl]<nint, uint, byte*, void> OnPairingCode;
    public delegate* unmanaged[Cdecl]<nint, byte*, void> OnPaired;
    public delegate* unmanaged[Cdecl]<nint, uint, int, int, int, int, void> OnStreaming;
    public delegate* unmanaged[Cdecl]<nint, EngineResult, void> OnFailed;
    public nint OnStats;
    public delegate* unmanaged[Cdecl]<nint, LogLevel, byte*, void> OnLog;
    public delegate* unmanaged[Cdecl]<nint, Platform, byte*, void> OnPeer;
    public delegate* unmanaged[Cdecl]<nint, byte*, void> OnRoleSwapRequest;
    public delegate* unmanaged[Cdecl]<nint, RoleSwapAnswer, byte*, void> OnRoleSwapAnswer;
}

/// <summary>osplay_engine_viewer_config.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeViewerConfig
{
    public nint SecretKey;
    public nint DeviceName;
    public nint Host;
    public ushort Port;
    public nint MonitorIds;
    public nuint MonitorIdCount;
    public uint HotkeyModifiers;
    public uint HotkeyKey;
    public int Windowed;
}

/// <summary>osplay_engine_viewer_callbacks.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeViewerCallbacks
{
    public nint Context;
    public delegate* unmanaged[Cdecl]<nint, SessionState, CoreResult, void> OnState;
    public delegate* unmanaged[Cdecl]<nint, byte*, Trust> OnTrustQuery;
    public delegate* unmanaged[Cdecl]<nint, uint, byte*, void> OnPairingCode;
    public delegate* unmanaged[Cdecl]<nint, byte*, void> OnPaired;
    public delegate* unmanaged[Cdecl]<nint, byte, void> OnVisibility;
    public delegate* unmanaged[Cdecl]<nint, byte, EngineResult, void> OnStreamFailed;
    public delegate* unmanaged[Cdecl]<nint, NativeSessionStats*, void> OnStats;
    public delegate* unmanaged[Cdecl]<nint, LogLevel, byte*, void> OnLog;
    public delegate* unmanaged[Cdecl]<nint, Platform, byte*, void> OnPeer;
    public delegate* unmanaged[Cdecl]<nint, byte*, void> OnRoleSwapRequest;
    public delegate* unmanaged[Cdecl]<nint, RoleSwapAnswer, byte*, void> OnRoleSwapAnswer;
    public delegate* unmanaged[Cdecl]<nint, void> OnMonitorsChanged;
}

/// <summary>osplay_stream_stats.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeStreamStats
{
    public byte StreamId;
    public ushort LossPermille;
    public uint FramesPerSecond;
    public uint BitrateKbps;
    public uint LatencyMicroseconds;
    public ulong FramesDropped;
    public ulong RetransmittedFragments;
    public ulong KeyframeRequests;
}

/// <summary>osplay_session_stats.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeSessionStats
{
    public uint RoundTripMicroseconds;
    public long ClockOffsetMicroseconds;
    public ulong DatagramsRejected;
    public NativeStreamStats* Streams;
    public nuint StreamCount;
}

/// <summary>osplay_engine_monitor.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeMonitor
{
    public byte MonitorId;
    public ushort WidthPixels;
    public ushort HeightPixels;
    public uint RefreshMillihertz;
    public ushort PhysicalWidthMillimeters;
    public ushort PhysicalHeightMillimeters;
    public ushort ScalePercent;
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
    public fixed byte Name[NativeSizes.NameSize];
}

/// <summary>osplay_virtual_monitor_status.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeVirtualMonitorStatus
{
    public byte DriverInstalled;
    public byte MonitorPresent;
    public byte Configured;
    public uint Width;
    public uint Height;
    public uint RefreshMillihertz;
}

/// <summary>osplay_version.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeVersion
{
    public ushort Major;
    public ushort Minor;
    public ushort Patch;
}
