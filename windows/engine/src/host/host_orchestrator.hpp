#pragma once

// Host session orchestration in the engine (DEVPLAN Phase 8, Phase 13b): when the viewer offers
// its monitors, add one virtual monitor per viewer monitor in its mode, start capture and encoding
// for each, configure the streams, and feed access units and the cursor to the core. Mirrors
// HostSession on macOS.

#include "display/virtual_display_provider.hpp"
#include "engine_tunables.hpp"
#include "host/stream_dump.hpp"
#include "host/stream_pipeline.hpp"

#include <osplay/osplay.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace osplay::engine {

/// The part of a core host session the orchestrator drives; the C API session in the engine.
class HostCorePort {
public:
    virtual ~HostCorePort() = default;
    virtual osplay_result configure_streams(std::span<const osplay_stream_config> streams) = 0;
    virtual osplay_result push_access_unit(const osplay_access_unit& unit) = 0;
    virtual osplay_result push_cursor_shape(const osplay_cursor_shape& shape) = 0;
    virtual osplay_result push_cursor_position(const osplay_cursor_position& position) = 0;
    /// Ends the session with Bye; the core reports the close later.
    virtual void close() = 0;
    /// Turns the viewer away: no virtual monitor is left for it (the viewer sees PEER_BUSY).
    virtual void refuse_busy() = 0;
};

/// Why the orchestrator ended a session.
enum class HostFailure : std::uint8_t {
    Display,  ///< the virtual monitor could not be added
    Pipeline, ///< capture or encoding failed
    Session,  ///< the core refused the stream configuration
};

class HostObserver {
public:
    virtual ~HostObserver() = default;
    /// Called on the orchestrator's worker thread: `display_count` virtual monitors stream, the
    /// first of them at `first_monitor`.
    virtual void on_streaming(const MonitorRect& first_monitor, std::size_t display_count) = 0;
    virtual void on_host_failed(HostFailure failure, std::uint32_t detail) = 0;
    /// Called on the stream's pipeline thread; must return quickly.
    virtual void on_capture_stats(std::uint8_t stream_id, const CaptureStats& stats) = 0;
};

/// The largest stream size (DEVPLAN Phase 13c); 0 × 0 streams at the monitor's own size.
struct StreamBox {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class HostOrchestrator final {
public:
    HostOrchestrator(HostCorePort& core, VirtualDisplayProvider& displays,
                     StreamPipelineFactory& pipelines, HostObserver& observer,
                     std::unique_ptr<StreamDump> dump = nullptr,
                     double bits_per_pixel = kBitsPerPixel, StreamBox stream_box = {});
    ~HostOrchestrator();
    HostOrchestrator(const HostOrchestrator&) = delete;
    HostOrchestrator& operator=(const HostOrchestrator&) = delete;
    HostOrchestrator(HostOrchestrator&&) = delete;
    HostOrchestrator& operator=(HostOrchestrator&&) = delete;

    /// The viewer introduced itself, before any pairing: false when no virtual monitor is left
    /// for it, and the viewer has been turned away (other sessions use them all). Never blocks.
    bool on_peer();
    // Core events, from the core's I/O thread; handled on the orchestrator's worker thread so the
    // I/O thread never waits for the monitor or the encoder.
    void on_viewer_monitors(std::span<const osplay_monitor> monitors);
    void on_keyframe_request(std::uint8_t stream_id);
    /// The core's bitrate target for the stream (adapted from receiver reports).
    void on_bitrate_target(std::uint8_t stream_id, std::uint32_t kbps);
    /// The viewer stopped a stream (e.g. its monitor was unplugged): that virtual monitor goes, the
    /// others are made again without it; the session stays up.
    void on_stream_stopped(std::uint8_t stream_id);
    /// The session ended: stop capturing and remove the monitors.
    void on_session_closed();

    /// Pauses or resumes every stream: paused pipelines encode black and hold the pointer back,
    /// so the viewers show nothing of this PC. A viewer that connects later starts paused too.
    void set_paused(bool paused);

    /// Blocks until queued work has run (tests).
    void wait_idle();

    /// The stream for a viewer monitor: fitted inside `box`, with a bitrate for the stream's own
    /// pixels. `bits_per_pixel` is clamped to kMinBitsPerPixel...kMaxBitsPerPixel.
    static osplay_stream_config stream_for(const osplay_monitor& monitor,
                                           double bits_per_pixel = kBitsPerPixel,
                                           StreamBox box = {});

private:
    class StreamOutput;

    /// One stream: its configuration, the sink tagging the pipeline's output with its ID, and the
    /// pipeline capturing its virtual monitor (declared last, so it stops before the sink goes).
    struct Stream {
        Stream(const osplay_stream_config& stream_config, const osplay_monitor& viewer_monitor,
               HostOrchestrator& owner);

        osplay_stream_config config;
        /// The viewer's monitor: its virtual monitor keeps this mode while the stream may be
        /// smaller.
        osplay_monitor monitor;
        std::unique_ptr<StreamOutput> output;
        std::unique_ptr<StreamPipeline> pipeline;
    };

    void post(std::function<void()> task);
    void run();
    void start(const std::vector<osplay_monitor>& monitors);
    /// Makes one virtual monitor per stream and (re)starts every pipeline on its monitor.
    bool open_streams();
    void stop_stream(std::uint8_t stream_id);
    void stop_streaming();
    void fail(HostFailure failure, std::uint32_t detail);

    // From the pipelines' threads, through StreamOutput.
    void on_access_unit(std::uint8_t stream_id, std::span<const std::uint8_t> annexb, bool keyframe,
                        std::uint64_t capture_time_us);
    void on_cursor_shape(std::uint8_t stream_id, std::uint32_t shape_id, const CursorImage& image);
    void on_cursor_position(std::uint8_t stream_id, bool visible, std::uint32_t shape_id,
                            std::int32_t x, std::int32_t y, std::uint64_t capture_time_us);
    void on_pipeline_failed(PipelineError error);
    void on_capture_stats(std::uint8_t stream_id, const CaptureStats& stats);

    HostCorePort* core_;               // never null; outlives the orchestrator
    VirtualDisplayProvider* displays_; // never null
    StreamPipelineFactory* pipelines_; // never null
    HostObserver* observer_;           // never null
    std::unique_ptr<StreamDump> dump_;
    double bits_per_pixel_;
    StreamBox stream_box_;

    std::map<std::uint8_t, Stream> streams_; // worker thread only
    bool monitors_added_ = false;            // worker thread only
    std::atomic<bool> failed_{false};
    std::atomic<bool> paused_{false}; // applied to every pipeline, also ones started later

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    bool busy_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace osplay::engine
