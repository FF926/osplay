namespace Osplay.App;

/// <summary>Every tunable of the shell in one place (DEVPLAN: no buried constants).</summary>
internal static class AppTunables
{
    /// <summary>A host session takes one viewer; the next listener starts after this pause.</summary>
    public static readonly TimeSpan RelistenDelay = TimeSpan.FromSeconds(1);

    /// <summary>Virtual monitors across every viewer of this PC (the engine's limit, DECISIONS.md 2026-10-25).</summary>
    public const int MaxVirtualMonitors = 4;

    /// <summary>
    /// Pause before a dropped viewer tries its host again (an old advertisement can refuse at once;
    /// this keeps that from becoming a tight loop).
    /// </summary>
    public static readonly TimeSpan ReconnectDelay = TimeSpan.FromSeconds(2);

    /// <summary>While sharing, how often an iPad on the USB cable is tried; it answers once its user picks the cable (TAB-03).</summary>
    public static readonly TimeSpan UsbRetryDelay = TimeSpan.FromSeconds(2);

    /// <summary>After a role swap, how long viewing waits for the other computer to advertise itself.</summary>
    public static readonly TimeSpan SwapReconnectTimeout = TimeSpan.FromSeconds(20);

    /// <summary>(fork) After a swap, the pause before sharing tries its listener again when the helper or the virtual display did not come up.</summary>
    public static readonly TimeSpan SwapListenRetryDelay = TimeSpan.FromSeconds(2);

    /// <summary>The log file is rolled over at this size; one older file is kept.</summary>
    public const long LogFileLimitBytes = 5 * 1024 * 1024;

    /// <summary>The boxes a stream is fitted into for each stream size setting (DEVPLAN Phase 13c).</summary>
    public static readonly (uint Width, uint Height) StreamBox720 = (1280, 720);

    public static readonly (uint Width, uint Height) StreamBox1080 = (1920, 1080);

    public static readonly (uint Width, uint Height) StreamBox1440 = (2560, 1440);

    /// <summary>GitHub's latest release of the repository (no Osplay server exists).</summary>
    public static readonly Uri LatestReleaseUri = new("https://api.github.com/repos/akinalpfdn/osplay/releases/latest");

    /// <summary>Releases page opened when no release could be read.</summary>
    public static readonly Uri ReleasesPageUri = new("https://github.com/akinalpfdn/osplay/releases");

    /// <summary>Automatic update checks happen at most this often, at launch.</summary>
    public static readonly TimeSpan UpdateCheckInterval = TimeSpan.FromDays(1);

    public static readonly TimeSpan UpdateCheckTimeout = TimeSpan.FromSeconds(15);

    /// <summary>A tray click right after the panel lost focus to it closes the panel instead of reopening it.</summary>
    public static readonly TimeSpan PanelReopenGuard = TimeSpan.FromMilliseconds(300);

    /// <summary>Copy diagnostics includes this many of the newest log lines.</summary>
    public const int DiagnosticsLogLines = 300;

    /// <summary>The PC's own network: "Osplay-" and this many characters, and a WPA2 password of this length.</summary>
    public const int PcNetworkNameSuffixLength = 4;

    public const int PcNetworkPasswordLength = 12;

    /// <summary>Pixels per module of the network's QR code.</summary>
    public const int PcNetworkQrModulePixels = 6;

    /// <summary>How often the open network's state is read back from Windows, which turns it off without telling.</summary>
    public static readonly TimeSpan PcNetworkPollInterval = TimeSpan.FromSeconds(2);

    /// <summary>On quit, how long closing the network and restoring the user's hotspot settings may take.</summary>
    public static readonly TimeSpan PcNetworkCloseTimeout = TimeSpan.FromSeconds(5);

    /// <summary>The paired devices the engine can store.</summary>
    public const int MaxPairedDevices = 256;
}
