#include "present/presentation_thread.hpp"

#include <exception>
#include <unordered_map>
#include <utility>

namespace osplay::engine {
namespace {

constexpr const wchar_t* kControlClass = L"OsplayViewerControl";
constexpr const wchar_t* kViewerClass = L"OsplayViewerWindow";
constexpr UINT kTaskMessage = WM_APP + 1;
constexpr int kHotkeyId = 1;

/// (fork) Placements of windowed viewer windows that are in fullscreen right now. Only touched on
/// the presentation thread, where every message arrives.
std::unordered_map<HWND, WINDOWPLACEMENT>& saved_placements() {
    static std::unordered_map<HWND, WINDOWPLACEMENT> placements;
    return placements;
}

/// (fork) Toggles a windowed viewer between its captioned frame and borderless fullscreen that
/// covers the monitor it is on, taskbar included — the twin of the Mac viewer's ⌃⌥⌘F. F11 or
/// double-clicking the video toggles; Esc only leaves fullscreen. The full-screen overlay windows
/// of the non-windowed mode have neither a caption nor a saved placement, so this is a no-op for
/// them.
void toggle_fullscreen(HWND window, bool leave_only) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    auto& placements = saved_placements();
    if ((style & WS_CAPTION) != 0) {
        if (leave_only) {
            return;
        }
        WINDOWPLACEMENT placement{.length = sizeof(WINDOWPLACEMENT)};
        if (GetWindowPlacement(window, &placement) == 0) {
            return;
        }
        MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) == 0) {
            return;
        }
        placements.insert_or_assign(window, placement);
        SetWindowLongPtrW(window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        return;
    }
    const auto placement = placements.find(window);
    if (placement == placements.end()) {
        return;
    }
    placements.erase(window);
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(window, &placement->second);
}

} // namespace

PresentationThread::PresentationThread(Callbacks callbacks, Hotkey hotkey)
    : callbacks_(std::move(callbacks)) {
    std::promise<bool> ready;
    auto started = ready.get_future();
    thread_ = std::thread(
        [this, hotkey, promise = std::move(ready)]() mutable { run(hotkey, std::move(promise)); });
    hotkey_registered_ = started.get();
}

PresentationThread::~PresentationThread() {
    try {
        if (control_window_ != nullptr) {
            PostMessageW(control_window_, WM_CLOSE, 0, 0);
        }
        thread_.join();
    } catch (...) {
        std::terminate(); // the UI thread cannot be left running behind its owner
    }
}

HWND PresentationThread::create_window(const MonitorRect& rect, bool visible) {
    HWND window = nullptr;
    invoke([&] {
        // No redirection surface: the flip-model swap chain is the only content, which lets DWM
        // hand the monitor to it directly (independent flip).
        window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kViewerClass, L"Osplay",
            WS_POPUP | (visible ? WS_VISIBLE : 0U), rect.left, rect.top, rect.right - rect.left,
            rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    });
    return window;
}

HWND PresentationThread::create_windowed_window(const MonitorRect& monitor_rect, const SIZE& client,
                                                bool visible) {
    HWND window = nullptr;
    invoke([&] {
        // (fork) A normal resizable window with a caption: the streamed screen sits on the
        // desktop and can be moved, resized or maximized, instead of covering a display.
        const int x = monitor_rect.left +
                      static_cast<int>(((monitor_rect.right - monitor_rect.left) - client.cx) / 2);
        const int y = monitor_rect.top +
                      static_cast<int>(((monitor_rect.bottom - monitor_rect.top) - client.cy) / 2);
        window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kViewerClass, L"Osplay",
                                 WS_OVERLAPPEDWINDOW | (visible ? WS_VISIBLE : 0U), x, y, client.cx,
                                 client.cy, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    });
    return window;
}

void PresentationThread::destroy_window(HWND window) {
    invoke([window] { DestroyWindow(window); });
}

void PresentationThread::show_window(HWND window, bool visible) {
    ShowWindowAsync(window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void PresentationThread::set_display_required(bool required) {
    invoke([required] {
        SetThreadExecutionState(ES_CONTINUOUS |
                                (required ? ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : 0U));
    });
}

void PresentationThread::invoke(const std::function<void()>& task) {
    if (std::this_thread::get_id() == thread_.get_id()) {
        task();
        return;
    }
    // Runs synchronously on the UI thread.
    SendMessageW(control_window_, kTaskMessage, 0, reinterpret_cast<LPARAM>(&task));
}

void PresentationThread::run(Hotkey hotkey, std::promise<bool> ready) {
    // Window rectangles and monitor sizes in physical pixels, whatever the host process declares.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_DBLCLKS; // (fork) double-clicking a windowed viewer toggles fullscreen
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = kControlClass;
    RegisterClassExW(&window_class);
    window_class.lpszClassName = kViewerClass;
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&window_class);

    // A hidden top-level window: message-only windows would miss WM_DISPLAYCHANGE broadcasts.
    control_window_ = CreateWindowExW(0, kControlClass, L"Osplay", WS_OVERLAPPED, 0, 0, 0, 0,
                                      nullptr, nullptr, instance, this);
    const bool registered = control_window_ != nullptr &&
                            RegisterHotKey(control_window_, kHotkeyId,
                                           hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE;
    ready.set_value(registered);
    if (control_window_ == nullptr) {
        return;
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    SetThreadExecutionState(ES_CONTINUOUS);
}

LRESULT CALLBACK PresentationThread::window_procedure(HWND window, UINT message, WPARAM wparam,
                                                      LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lparam); // NOLINT(performance-no-int-to-ptr)
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wparam, lparam);
    }
    // Set only on the control window; viewer windows have no owner object.
    auto* self = reinterpret_cast<PresentationThread*>( // NOLINT(performance-no-int-to-ptr)
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case kTaskMessage:
        try {
            // NOLINTNEXTLINE(performance-no-int-to-ptr): the task pointer sent by invoke()
            (*reinterpret_cast<const std::function<void()>*>(lparam))();
        } catch (...) {
            // The caller's task must not unwind through Win32; it reports its own result.
            OutputDebugStringW(L"Osplay: a presentation task threw\n");
        }
        return 0;
    case WM_HOTKEY:
        if (self != nullptr && wparam == kHotkeyId && self->callbacks_.on_hotkey) {
            self->callbacks_.on_hotkey();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (self != nullptr && self->callbacks_.on_display_change) {
            self->callbacks_.on_display_change();
        }
        return 0;
    case WM_SETCURSOR:
        // No local pointer over a viewer window; a windowed viewer keeps the normal cursor, so
        // the desktop window behaves like any other.
        if (self == nullptr && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == 0) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        // (fork) F11 toggles fullscreen and Esc leaves it, once the window has focus (a click
        // inside it). Viewer windows only; the control window never has keyboard focus anyway.
        if (self == nullptr && (wparam == VK_F11 || wparam == VK_ESCAPE)) {
            toggle_fullscreen(window, wparam == VK_ESCAPE);
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (self == nullptr) {
            toggle_fullscreen(window, false);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        if (self != nullptr) {
            UnregisterHotKey(window, kHotkeyId); // fails harmlessly if it was never registered
            DestroyWindow(window);
        }
        return 0; // viewer windows ignore Alt+F4: the hotkey or the session closes them
    case WM_DESTROY:
        // (fork) A window closed while fullscreen takes its saved placement with it.
        saved_placements().erase(window);
        if (self != nullptr) {
            PostQuitMessage(0);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace osplay::engine
