#pragma once

// One capture → encode pipeline for one virtual monitor (Desktop Duplication and Media Foundation
// on Windows, fakes in tests). The orchestrator only sees this interface.

#include "media/pointer_shape.hpp"

#include <osplay/win/expected.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace osplay::engine {

struct StreamSettings {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_millihertz = 0;
    std::uint32_t bitrate_kbps = 0;
    /// The virtual monitor to capture (\\.\DISPLAYn).
    std::wstring gdi_name;
};

enum class PipelineError : std::uint8_t {
    MonitorNotFound,    ///< the virtual monitor's output is not visible to DXGI
    CaptureFailed,      ///< Desktop Duplication could not start or stopped for good
    EncoderUnavailable, ///< no hardware H.264 encoder accepts the stream
    EncoderFailed,      ///< the encoder failed while running
    DecoderUnavailable, ///< viewer: no hardware H.264 decoder accepts the stream
    PresentationFailed, ///< viewer: the window or swap chain could not be created
};

/// The virtual monitor's place on the desktop, in desktop coordinates.
struct MonitorRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

/// What the capture loop saw over one reporting interval, for the log.
struct CaptureStats {
    std::uint32_t desktop_width = 0; ///< the virtual monitor's desktop as captured
    std::uint32_t desktop_height = 0;
    std::uint32_t images = 0;          ///< new desktop images Windows delivered
    std::uint32_t encoded = 0;         ///< frames given to the encoder
    std::uint32_t access_lost = 0;     ///< duplications Windows invalidated
    std::uint32_t reconnects = 0;      ///< duplications created again
    std::uint32_t slowest_step_ms = 0; ///< longest loop iteration without its wait for a frame
};

/// Receives the pipeline's output on the pipeline's own thread. Calls must return quickly.
class PipelineSink {
public:
    virtual ~PipelineSink() = default;
    /// One access unit in Annex B; keyframes start with SPS and PPS.
    virtual void on_access_unit(std::span<const std::uint8_t> annexb, bool keyframe,
                                std::uint64_t capture_time_us) = 0;
    virtual void on_cursor_shape(std::uint32_t shape_id, const CursorImage& image) = 0;
    /// `x`/`y` are the hotspot in stream pixels.
    virtual void on_cursor_position(bool visible, std::uint32_t shape_id, std::int32_t x,
                                    std::int32_t y, std::uint64_t capture_time_us) = 0;
    /// The pipeline stopped for good; no output follows.
    virtual void on_pipeline_failed(PipelineError error) = 0;
    /// Every few seconds while the pipeline runs.
    virtual void on_capture_stats(const CaptureStats& stats) = 0;
};

class StreamPipeline {
public:
    virtual ~StreamPipeline() = default;
    /// The next encoded frame is a keyframe; an unchanged screen is encoded again for it.
    virtual void request_keyframe() = 0;
    /// The link changed: encode at this average bitrate from the next frame. Never blocks.
    virtual void set_bitrate(std::uint32_t kbps) = 0;
    /// Paused pipelines encode black frames and hold the pointer back, so the viewer shows
    /// nothing of this PC. Capture keeps running and the session stays up. Never blocks.
    virtual void set_paused(bool paused) { (void)paused; }
    [[nodiscard]] virtual std::optional<MonitorRect> monitor_rect() const = 0;
};

class StreamPipelineFactory {
public:
    virtual ~StreamPipelineFactory() = default;
    /// Starts capturing the virtual monitor and encoding it; `sink` must outlive the pipeline.
    [[nodiscard]] virtual win::Expected<std::unique_ptr<StreamPipeline>, PipelineError>
    start(const StreamSettings& settings, PipelineSink& sink) = 0;
};

} // namespace osplay::engine
