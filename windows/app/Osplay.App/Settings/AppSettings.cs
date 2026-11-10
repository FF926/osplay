using System.IO;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using Osplay.App.Diagnostics;
using Osplay.App.Interop;

namespace Osplay.App.Settings;

/// <summary>What the user chose, saved to %APPDATA%\Osplay\settings.json on every change.</summary>
public sealed partial class AppSettings : ObservableObject
{
    private static readonly JsonSerializerOptions _json = new() { WriteIndented = true };

    private readonly string _path;
    private bool _loading;

    private AppSettings(string path) => _path = path;

    [ObservableProperty]
    private QualityPreset _quality = QualityPreset.High;

    /// <summary>The largest size a shared monitor is streamed at (1080p by default, user decision 2026-10-25).</summary>
    [ObservableProperty]
    private StreamSizePreset _streamSize = StreamSizePreset.Hd1080;

    [ObservableProperty]
    private HotkeyGesture _viewerHotkey = HotkeyGesture.Default;

    /// <summary>Reverses the roles of a running session.</summary>
    [ObservableProperty]
    private HotkeyGesture _swapHotkey = HotkeyGesture.DefaultSwap;

    /// <summary>Shows and hides the stats overlay while viewing.</summary>
    [ObservableProperty]
    private HotkeyGesture _statsHotkey = HotkeyGesture.DefaultStats;

    [ObservableProperty]
    private bool _showsStatsOverlay;

    /// <summary>(fork) Shows the remote screen in a movable window instead of full screen.</summary>
    [ObservableProperty]
    private bool _displayWindowed;

    [ObservableProperty]
    private bool _checksForUpdates = true;

    [ObservableProperty]
    private DateTimeOffset? _lastUpdateCheck;

    /// <summary>The PC's own network (TAB-04): generated once, so devices that joined it join again.</summary>
    [ObservableProperty]
    private string _pcNetworkName = string.Empty;

    [ObservableProperty]
    private string _pcNetworkPassword = string.Empty;

    /// <summary>Monitors not offered to hosts, by <see cref="MonitorKey"/>.</summary>
    [ObservableProperty]
    private IReadOnlyList<string> _excludedMonitors = [];

    /// <summary>A monitor's identity across launches: its name and place (ids change with the layout).</summary>
    public static string MonitorKey(string name, int left, int top) => $"{name}|{left}|{top}";

    public static AppSettings Load()
    {
        string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Osplay");
        var settings = new AppSettings(Path.Combine(folder, "settings.json"));
        try
        {
            if (File.Exists(settings._path))
            {
                Stored? stored = JsonSerializer.Deserialize<Stored>(File.ReadAllText(settings._path), _json);
                if (stored is not null)
                {
                    settings.Apply(stored);
                }
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            Log.Write(LogLevel.Warning, $"Settings could not be read, using defaults: {error.Message}");
        }
        return settings;
    }

    protected override void OnPropertyChanged(System.ComponentModel.PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (!_loading)
        {
            Save();
        }
    }

    private void Apply(Stored stored)
    {
        _loading = true;
        try
        {
            Quality = Enum.IsDefined(stored.Quality) ? stored.Quality : QualityPreset.High;
            StreamSize = Enum.IsDefined(stored.StreamSize) ? stored.StreamSize : StreamSizePreset.Hd1080;
            ViewerHotkey = stored.HotkeyKey == 0
                ? HotkeyGesture.Default
                : new HotkeyGesture(stored.HotkeyModifiers, stored.HotkeyKey);
            ShowsStatsOverlay = stored.ShowsStatsOverlay;
            DisplayWindowed = stored.DisplayWindowed;
            ChecksForUpdates = stored.ChecksForUpdates;
            LastUpdateCheck = stored.LastUpdateCheck;
            ExcludedMonitors = stored.ExcludedMonitors ?? [];
            StatsHotkey = stored.StatsKey == 0
                ? HotkeyGesture.DefaultStats
                : new HotkeyGesture(stored.StatsModifiers, stored.StatsKey);
            SwapHotkey = stored.SwapKey == 0
                ? HotkeyGesture.DefaultSwap
                : new HotkeyGesture(stored.SwapModifiers, stored.SwapKey);
            PcNetworkName = stored.PcNetworkName ?? string.Empty;
            PcNetworkPassword = stored.PcNetworkPassword ?? string.Empty;
        }
        finally
        {
            _loading = false;
        }
    }

    private void Save()
    {
        var stored = new Stored(
            Quality, ViewerHotkey.Modifiers, ViewerHotkey.VirtualKey, ShowsStatsOverlay, ChecksForUpdates, LastUpdateCheck,
            [.. ExcludedMonitors], SwapHotkey.Modifiers, SwapHotkey.VirtualKey,
            StatsHotkey.Modifiers, StatsHotkey.VirtualKey, StreamSize, PcNetworkName, PcNetworkPassword, DisplayWindowed);
        try
        {
            _ = Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
            File.WriteAllText(_path, JsonSerializer.Serialize(stored, _json));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Log.Write(LogLevel.Warning, $"Settings could not be saved: {error.Message}");
        }
    }

    private sealed record Stored(
        QualityPreset Quality, uint HotkeyModifiers, uint HotkeyKey, bool ShowsStatsOverlay, bool ChecksForUpdates,
        DateTimeOffset? LastUpdateCheck, List<string>? ExcludedMonitors,
        uint SwapModifiers = 0, uint SwapKey = 0, uint StatsModifiers = 0, uint StatsKey = 0,
        StreamSizePreset StreamSize = StreamSizePreset.Hd1080, string? PcNetworkName = null,
        string? PcNetworkPassword = null, bool DisplayWindowed = false);
}
