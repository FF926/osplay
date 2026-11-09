using System.Windows.Input;

namespace Osplay.App.Settings;

/// <summary>A global hotkey as RegisterHotKey takes it: MOD_* bits and a virtual-key code.</summary>
public sealed record HotkeyGesture(uint Modifiers, uint VirtualKey)
{
    public const uint Alt = 0x0001;
    public const uint Control = 0x0002;
    public const uint Shift = 0x0004;
    public const uint Windows = 0x0008;

    /// <summary>User decision: Ctrl+Alt+Shift+V (Ctrl+Alt alone is AltGr on Turkish layouts).</summary>
    public static HotkeyGesture Default { get; } = new(Control | Alt | Shift, 0x56);

    /// <summary>The role swap shortcut (Phase 12b): Ctrl+Alt+Shift+S, next to the viewer's.</summary>
    public static HotkeyGesture DefaultSwap { get; } = new(Control | Alt | Shift, 0x53);

    /// <summary>The stats overlay shortcut (Phase 13): Ctrl+Alt+Shift+I.</summary>
    public static HotkeyGesture DefaultStats { get; } = new(Control | Alt | Shift, 0x49);

    /// <summary>The host pause shortcut (fork addition): Ctrl+Alt+Shift+H.</summary>
    public static HotkeyGesture DefaultPause { get; } = new(Control | Alt | Shift, 0x48);

    /// <summary>A gesture from a WPF key press, or null without Ctrl, Alt or Win (Shift alone types).</summary>
    public static HotkeyGesture? From(Key key, ModifierKeys modifiers)
    {
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift
            or Key.RightShift or Key.LWin or Key.RWin or Key.System or Key.None)
        {
            return null;
        }
        if ((modifiers & (ModifierKeys.Control | ModifierKeys.Alt | ModifierKeys.Windows)) == 0)
        {
            return null;
        }
        uint bits = 0;
        bits |= modifiers.HasFlag(ModifierKeys.Alt) ? Alt : 0;
        bits |= modifiers.HasFlag(ModifierKeys.Control) ? Control : 0;
        bits |= modifiers.HasFlag(ModifierKeys.Shift) ? Shift : 0;
        bits |= modifiers.HasFlag(ModifierKeys.Windows) ? Windows : 0;
        return new HotkeyGesture(bits, (uint)KeyInterop.VirtualKeyFromKey(key));
    }

    /// <summary>"Ctrl+Alt+Shift+V".</summary>
    public string DisplayText
    {
        get
        {
            var parts = new List<string>();
            if ((Modifiers & Control) != 0)
            {
                parts.Add(Localization.Strings.KeyControl);
            }
            if ((Modifiers & Alt) != 0)
            {
                parts.Add(Localization.Strings.KeyAlt);
            }
            if ((Modifiers & Shift) != 0)
            {
                parts.Add(Localization.Strings.KeyShift);
            }
            if ((Modifiers & Windows) != 0)
            {
                parts.Add(Localization.Strings.KeyWindows);
            }
            Key key = KeyInterop.KeyFromVirtualKey((int)VirtualKey);
            parts.Add(new KeyConverter().ConvertToString(key) ?? key.ToString());
            return string.Join('+', parts);
        }
    }
}
