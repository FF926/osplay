#pragma once

// The viewer's UI thread (DEVPLAN: presentation runs on its own thread): it owns the borderless
// windows that cover the target monitors, the hide/show hotkey and the display-required power
// state, and reports display changes. Swap chains present from the stream threads; only window
// creation, destruction and messages happen here.

#include "host/stream_pipeline.hpp"

#include <windows.h>

#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace osplay::engine {

class PresentationThread {
public:
    struct Callbacks {
        std::function<void()> on_hotkey;         ///< the hide/show hotkey was pressed
        std::function<void()> on_display_change; ///< monitors were added, removed or changed
    };

    /// RegisterHotKey's MOD_* bits (without MOD_NOREPEAT) and virtual-key code.
    struct Hotkey {
        unsigned modifiers = 0;
        unsigned key = 0;
    };

    /// Starts the thread; `hotkey_registered` reports whether the hotkey could be taken.
    PresentationThread(Callbacks callbacks, Hotkey hotkey);
    ~PresentationThread();
    PresentationThread(const PresentationThread&) = delete;
    PresentationThread& operator=(const PresentationThread&) = delete;
    PresentationThread(PresentationThread&&) = delete;
    PresentationThread& operator=(PresentationThread&&) = delete;

    [[nodiscard]] bool hotkey_registered() const { return hotkey_registered_; }

    /// A borderless topmost window exactly covering `rect` (desktop pixels); null on failure.
    [[nodiscard]] HWND create_window(const MonitorRect& rect, bool visible);
    /// (fork) A normal fixed-size window with a caption, centered on `monitor_rect` with a client
    /// area of `client`; the streamed screen sits on the desktop instead of covering a display.
    /// Null on failure.
    [[nodiscard]] HWND create_windowed_window(const MonitorRect& monitor_rect, const SIZE& client,
                                              bool visible);
    void destroy_window(HWND window);
    /// Asynchronous, so a decode thread never waits for the UI thread to repaint.
    static void show_window(HWND window, bool visible);
    /// Keeps the displays on while a viewer is visible (SetThreadExecutionState on this thread).
    void set_display_required(bool required);

private:
    static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam);
    void run(Hotkey hotkey, std::promise<bool> ready);
    void invoke(const std::function<void()>& task);

    Callbacks callbacks_;
    HWND control_window_ = nullptr; // hidden; receives the hotkey, display changes and tasks
    bool hotkey_registered_ = false;
    std::thread thread_;
};

} // namespace osplay::engine
