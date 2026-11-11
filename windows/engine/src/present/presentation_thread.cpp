#include "present/presentation_thread.hpp"

#include <exception>
#include <utility>

namespace osplay::engine {
namespace {

constexpr const wchar_t* kControlClass = L"OsplayViewerControl";
constexpr const wchar_t* kViewerClass = L"OsplayViewerWindow";
constexpr UINT kTaskMessage = WM_APP + 1;
constexpr int kHotkeyId = 1;

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
        if (self == nullptr) {
            SetCursor(nullptr); // no local pointer over a viewer window
            return TRUE;
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
