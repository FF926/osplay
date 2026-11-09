using System.Runtime.InteropServices;

namespace Osplay.App.Interop;

/// <summary>The engine's flat C API (windows/engine/include/osplay/engine.h).</summary>
internal static unsafe partial class NativeMethods
{
    private const string _engine = "OsplayEngine";

    [LibraryImport(_engine, EntryPoint = "osplay_engine_core_version")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial NativeVersion CoreVersion();

    [LibraryImport(_engine, EntryPoint = "osplay_engine_identity")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult Identity(byte* publicKey, byte* deviceId);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_peers")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult Peers(NativePeer* peers, nuint capacity, nuint* count);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_peer_revoke")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult PeerRevoke(byte* publicKey);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_peer_remove")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult PeerRemove(byte* publicKey);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_browser_start")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult BrowserStart(NativeBrowserCallbacks* callbacks, nint* browser);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_browser_destroy")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial void BrowserDestroy(nint browser);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_advertiser_start")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult AdvertiserStart(byte* name, Roles roles, ushort port, nint* advertiser);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_advertiser_destroy")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial void AdvertiserDestroy(nint advertiser);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_start")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostStart(NativeHostConfig* config, NativeHostCallbacks* callbacks, nint* host);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_start_usb")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostStartUsb(
        NativeHostConfig* config, NativeHostCallbacks* callbacks, uint usbDeviceId, nint* host);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_usb_watcher_start")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult UsbWatcherStart(NativeUsbCallbacks* callbacks, nint* watcher);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_usb_watcher_destroy")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial void UsbWatcherDestroy(nint watcher);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_android_watcher_start")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult AndroidWatcherStart(NativeAndroidCallbacks* callbacks, nint* watcher);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_android_watcher_destroy")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial void AndroidWatcherDestroy(nint watcher);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_start_accessory")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostStartAccessory(
        NativeHostConfig* config, NativeHostCallbacks* callbacks, nint watcher, uint accessoryId, nint* host);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_control_port")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostControlPort(nint host, ushort* port);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_confirm_pairing")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostConfirmPairing(nint host, byte accepted);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_stop")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostStop(nint host);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_destroy")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial void HostDestroy(nint host);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_start")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerStart(NativeViewerConfig* config, NativeViewerCallbacks* callbacks, nint* viewer);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_hotkey_registered")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerHotkeyRegistered(nint viewer, byte* registered);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_confirm_pairing")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerConfirmPairing(nint viewer, byte accepted);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_set_visible")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerSetVisible(nint viewer, byte visible);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_stop")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerStop(nint viewer);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_destroy")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial void ViewerDestroy(nint viewer);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_list_monitors")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ListMonitors(NativeMonitor* monitors, nuint capacity, nuint* count);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_set_overlay")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerSetOverlay(nint viewer, byte streamId, byte* text);

    [LibraryImport(_engine, EntryPoint = "osplay_virtual_display_open")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult VirtualDisplayOpen(nint* display);

    [LibraryImport(_engine, EntryPoint = "osplay_virtual_display_status")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult VirtualDisplayStatus(nint display, NativeVirtualMonitorStatus* status);

    [LibraryImport(_engine, EntryPoint = "osplay_virtual_display_close")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial void VirtualDisplayClose(nint display);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_request_role_swap")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostRequestRoleSwap(nint host);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_answer_role_swap")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostAnswerRoleSwap(nint host, RoleSwapAnswer answer);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_request_role_swap")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerRequestRoleSwap(nint viewer);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_answer_role_swap")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerAnswerRoleSwap(nint viewer, RoleSwapAnswer answer);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_suspend")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostSuspend(nint host);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_host_set_paused")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult HostSetPaused(nint host, int paused);

    [LibraryImport(_engine, EntryPoint = "osplay_engine_viewer_suspend")]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static partial EngineResult ViewerSuspend(nint viewer);
}
