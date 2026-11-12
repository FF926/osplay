#pragma once

#include <cstddef>
#include <cstdint>

namespace osplay::engine {

/// How long the engine waits for a newly enabled virtual monitor to join the desktop.
inline constexpr std::uint32_t kMonitorArrivalTimeoutMilliseconds = 5000;
inline constexpr std::uint32_t kMonitorArrivalPollMilliseconds = 100;
/// Topology can change between sizing and querying the display configuration.
inline constexpr int kDisplayConfigQueryAttempts = 3;

/// A Windows host sends one stream per viewer monitor, each from its own virtual monitor
/// (OSPLAY_MAX_STREAMS); stream IDs are 0...n-1 in the viewer's order.
inline constexpr std::size_t kMaxHostStreams = 4;
/// Target bitrate in bits per pixel per frame: 0.1 gives ~25 Mbit/s for 2560×1664 at 60 Hz. The
/// shell's quality preset replaces it; this is the default.
inline constexpr double kBitsPerPixel = 0.1;
inline constexpr double kMinBitsPerPixel = 0.02;
inline constexpr double kMaxBitsPerPixel = 0.3;
/// Host: streams never exceed this rate, whatever the viewer's monitor runs at. (fork) 120 Hz —
/// the RTX-class GPUs this fork's users pair with NVENC-encode it easily, and the bitrate in
/// stream_for() scales with the rate, so the network budget follows automatically.
inline constexpr std::uint32_t kMaxStreamRefreshMillihertz = 120'000;
inline constexpr std::uint32_t kMinBitrateKbps = 2'000;
inline constexpr std::uint32_t kMaxBitrateKbps = 80'000;

/// Desktop Duplication: waits for a new frame, and how fast to poll while the encoder works.
inline constexpr unsigned kIdleAcquireMilliseconds = 16;
inline constexpr unsigned kEncoderPollMilliseconds = 1;
/// After a frame went to the encoder, its output is expected within this time.
inline constexpr std::int64_t kEncoderBusyMilliseconds = 40;
/// While duplication is unavailable (secure desktop, mode change): retry spacing and idle sleep.
inline constexpr unsigned long long kDuplicationRetryMilliseconds = 100;
inline constexpr unsigned kDuplicationIdleMilliseconds = 10;
/// How often each capture loop reports what it saw (matches the core's stats summary).
inline constexpr std::int64_t kCaptureStatsSeconds = 10;

/// Viewer hide/show hotkey unless the shell sets one. User decision: Ctrl+Alt+Shift+V (Ctrl+Alt
/// alone is AltGr on Turkish layouts). Values of MOD_ALT | MOD_CONTROL | MOD_SHIFT and the virtual
/// key 'V'.
inline constexpr unsigned kViewerHotkeyModifiers = 0x0001U | 0x0002U | 0x0004U;
inline constexpr unsigned kViewerHotkeyKey = 0x56U;

/// Viewer: access units waiting for the decoder before it starts over from a keyframe.
inline constexpr std::size_t kViewerQueueLimit = 30;
/// Decoder errors closer together than this send one keyframe request (the core also limits).
inline constexpr unsigned long long kViewerKeyframeRequestSpacingMilliseconds = 100;
/// Longest wait for the swap chain to accept the next frame.
inline constexpr unsigned kPresentWaitMilliseconds = 100;

/// Stats overlay on viewer streams, matching the Mac's (Theme.StatsOverlay): top-left corner,
/// sizes in points at 100% scaling.
inline constexpr std::size_t kOverlayTextLimit = 255;
inline constexpr const wchar_t* kOverlayFontFamily = L"Segoe UI";
inline constexpr float kOverlayFontSize = 12.0F;
inline constexpr float kOverlayPadding = 6.0F;
inline constexpr float kOverlayMargin = 12.0F;
inline constexpr float kOverlayCornerRadius = 6.0F;
inline constexpr float kOverlayBackgroundOpacity = 0.6F; // black
inline constexpr std::uint32_t kOverlayTextColor = 0xFFFFFF;

/// Android on the USB cable (TAB-05): one AOA control request through UsbDk may take this long.
inline constexpr std::uint32_t kAccessoryControlTimeoutMilliseconds = 2000;
/// A device's interfaces register after the device; it is looked at this long after it arrives.
inline constexpr std::uint32_t kAccessoryProbeDelayMilliseconds = 1000;
/// Switches per device (by instance ID) while a watcher runs: Android drops back to its normal
/// mode when no app opens the accessory within 10 s, and is then switched again.
inline constexpr int kAccessorySwitchAttempts = 3;

} // namespace osplay::engine
