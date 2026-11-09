#include "host/host_orchestrator.hpp"

#include "engine_tunables.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace osplay::engine {

/// Hands one pipeline's output to the orchestrator with its stream ID.
class HostOrchestrator::StreamOutput final : public PipelineSink {
public:
    StreamOutput(HostOrchestrator& owner, std::uint8_t stream_id)
        : owner_(&owner), stream_id_(stream_id) {}

    void on_access_unit(std::span<const std::uint8_t> annexb, bool keyframe,
                        std::uint64_t capture_time_us) override {
        owner_->on_access_unit(stream_id_, annexb, keyframe, capture_time_us);
    }
    void on_cursor_shape(std::uint32_t shape_id, const CursorImage& image) override {
        owner_->on_cursor_shape(stream_id_, shape_id, image);
    }
    void on_cursor_position(bool visible, std::uint32_t shape_id, std::int32_t x, std::int32_t y,
                            std::uint64_t capture_time_us) override {
        owner_->on_cursor_position(stream_id_, visible, shape_id, x, y, capture_time_us);
    }
    void on_pipeline_failed(PipelineError error) override { owner_->on_pipeline_failed(error); }
    void on_capture_stats(const CaptureStats& stats) override {
        owner_->on_capture_stats(stream_id_, stats);
    }

private:
    HostOrchestrator* owner_; // never null; owns this output
    std::uint8_t stream_id_;
};

HostOrchestrator::Stream::Stream(const osplay_stream_config& stream_config,
                                 const osplay_monitor& viewer_monitor, HostOrchestrator& owner)
    : config(stream_config), monitor(viewer_monitor),
      output(std::make_unique<StreamOutput>(owner, stream_config.stream_id)) {}

HostOrchestrator::HostOrchestrator(HostCorePort& core, VirtualDisplayProvider& displays,
                                   StreamPipelineFactory& pipelines, HostObserver& observer,
                                   std::unique_ptr<StreamDump> dump, double bits_per_pixel,
                                   StreamBox stream_box)
    : core_(&core), displays_(&displays), pipelines_(&pipelines), observer_(&observer),
      dump_(std::move(dump)), bits_per_pixel_(bits_per_pixel), stream_box_(stream_box),
      worker_([this] { run(); }) {}

HostOrchestrator::~HostOrchestrator() {
    try {
        post([this] { stop_streaming(); });
        {
            const std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        worker_.join();
    } catch (...) {
        // Only allocation, locking or joining can throw here, and an orchestrator that cannot
        // stop its worker is unrecoverable; this makes the implicit terminate explicit.
        std::terminate();
    }
}

bool HostOrchestrator::on_peer() {
    if (displays_->available_monitors() > 0) {
        return true;
    }
    core_->refuse_busy();
    return false;
}

void HostOrchestrator::on_viewer_monitors(std::span<const osplay_monitor> monitors) {
    if (monitors.empty()) {
        return;
    }
    std::vector<osplay_monitor> offered;
    for (osplay_monitor monitor : monitors.first(std::min(monitors.size(), kMaxHostStreams))) {
        // Copied: the core's strings are valid only during its callback.
        monitor.name = nullptr;
        // High refresh streams come after the MVP (DEVPLAN); a 120 Hz monitor gets a 60 Hz stream.
        monitor.refresh_millihertz =
            std::min(monitor.refresh_millihertz, kMaxStreamRefreshMillihertz);
        offered.push_back(monitor);
    }
    post([this, offered = std::move(offered)] { start(offered); });
}

void HostOrchestrator::on_keyframe_request(std::uint8_t stream_id) {
    post([this, stream_id] {
        if (const auto it = streams_.find(stream_id); it != streams_.end() && it->second.pipeline) {
            it->second.pipeline->request_keyframe();
        }
    });
}

void HostOrchestrator::on_bitrate_target(std::uint8_t stream_id, std::uint32_t kbps) {
    post([this, stream_id, kbps] {
        if (const auto it = streams_.find(stream_id); it != streams_.end() && it->second.pipeline) {
            it->second.pipeline->set_bitrate(kbps);
        }
    });
}

void HostOrchestrator::on_stream_stopped(std::uint8_t stream_id) {
    post([this, stream_id] { stop_stream(stream_id); });
}

void HostOrchestrator::on_session_closed() {
    post([this] { stop_streaming(); });
}

void HostOrchestrator::set_paused(bool paused) {
    // The flag also covers viewers that connect later: open_streams applies it to new pipelines.
    paused_.store(paused, std::memory_order_relaxed);
    post([this, paused] {
        for (auto& [stream_id, stream] : streams_) {
            if (stream.pipeline) {
                stream.pipeline->set_paused(paused);
            }
        }
    });
}

void HostOrchestrator::wait_idle() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return tasks_.empty() && !busy_; });
}

osplay_stream_config HostOrchestrator::stream_for(const osplay_monitor& monitor,
                                                  double bits_per_pixel, StreamBox box) {
    std::uint32_t width = monitor.width_px;
    std::uint32_t height = monitor.height_px;
    // Cannot fail for a monitor the core accepted (its sides are at least 320 × 240).
    static_cast<void>(osplay_fit_stream_size(monitor.width_px, monitor.height_px, box.width,
                                             box.height, &width, &height));
    const double pixels_per_second =
        static_cast<double>(width) * height * monitor.refresh_millihertz / 1000.0;
    const double bits = std::clamp(bits_per_pixel, kMinBitsPerPixel, kMaxBitsPerPixel);
    const auto kbps = static_cast<std::uint32_t>(pixels_per_second * bits / 1000.0);
    return osplay_stream_config{
        .stream_id = 0,
        .monitor_id = monitor.monitor_id,
        .width = static_cast<std::uint16_t>(width),
        .height = static_cast<std::uint16_t>(height),
        .refresh_millihertz = monitor.refresh_millihertz,
        .profile = OSPLAY_H264_HIGH,
        // Desktop Duplication delivers the pointer apart from the image (DEVPLAN).
        .cursor_mode = OSPLAY_CURSOR_SEPARATE,
        .bitrate_kbps = std::clamp(kbps, kMinBitrateKbps, kMaxBitrateKbps),
    };
}

void HostOrchestrator::post(std::function<void()> task) {
    {
        const std::scoped_lock lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    changed_.notify_all();
}

void HostOrchestrator::run() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                return; // stopping, nothing left to do
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
            busy_ = true;
        }
        task();
        {
            const std::scoped_lock lock(mutex_);
            busy_ = false;
        }
        changed_.notify_all();
    }
}

void HostOrchestrator::start(const std::vector<osplay_monitor>& monitors) {
    if (!streams_.empty() || failed_) {
        return; // the viewer's monitors arrive once per session
    }
    // Other sessions may use some of the virtual monitors: this viewer gets what is left.
    const std::size_t granted = std::min(monitors.size(), displays_->available_monitors());
    if (granted == 0) {
        core_->refuse_busy();
        return;
    }
    std::vector<osplay_stream_config> configs;
    for (std::size_t index = 0; index < granted; ++index) {
        osplay_stream_config config = stream_for(monitors[index], bits_per_pixel_, stream_box_);
        config.stream_id = static_cast<std::uint8_t>(index);
        configs.push_back(config);
        streams_.try_emplace(config.stream_id, config, monitors[index], *this);
    }
    if (!open_streams()) {
        return;
    }
    if (const osplay_result configured = core_->configure_streams(configs);
        configured != OSPLAY_OK) {
        fail(HostFailure::Session, static_cast<std::uint32_t>(configured));
        return;
    }
    observer_->on_streaming(
        streams_.begin()->second.pipeline->monitor_rect().value_or(MonitorRect{}), streams_.size());
}

bool HostOrchestrator::open_streams() {
    std::vector<VirtualMonitorConfig> monitors;
    for (auto& [stream_id, stream] : streams_) {
        stream.pipeline.reset(); // its monitor is about to be made again
        // The virtual monitor in the viewer monitor's mode; the pipeline scales it to the stream.
        monitors.push_back({.width = stream.monitor.width_px,
                            .height = stream.monitor.height_px,
                            .refresh_millihertz = stream.config.refresh_millihertz,
                            .gpu_name = {}});
    }
    const auto created = displays_->create_monitors(monitors);
    if (!created && created.error() == DisplayError::NoMonitorLeft) {
        // Another session took the last monitors meanwhile: as busy as before pairing.
        streams_.clear();
        core_->refuse_busy();
        return false;
    }
    if (!created) {
        fail(HostFailure::Display, static_cast<std::uint32_t>(created.error()));
        return false;
    }
    monitors_added_ = true;
    std::size_t index = 0;
    for (auto& [stream_id, stream] : streams_) {
        auto pipeline = pipelines_->start({.width = stream.config.width,
                                           .height = stream.config.height,
                                           .refresh_millihertz = stream.config.refresh_millihertz,
                                           .bitrate_kbps = stream.config.bitrate_kbps,
                                           .gdi_name = created.value().at(index++)},
                                          *stream.output);
        if (!pipeline) {
            fail(HostFailure::Pipeline, static_cast<std::uint32_t>(pipeline.error()));
            return false;
        }
        stream.pipeline = std::move(pipeline.value());
        stream.pipeline->set_paused(paused_.load(std::memory_order_relaxed));
    }
    return true;
}

void HostOrchestrator::stop_stream(std::uint8_t stream_id) {
    if (streams_.erase(stream_id) == 0) {
        return;
    }
    if (streams_.empty()) {
        stop_streaming();
        return;
    }
    // The remaining monitors are made again (their devices restart to read the new settings), and
    // their captures start over on them (each begins with a keyframe).
    static_cast<void>(open_streams());
}

void HostOrchestrator::stop_streaming() {
    streams_.clear();
    if (monitors_added_) {
        monitors_added_ = false;
        // Removal failures leave nothing to undo here; the helper removes the monitors anyway
        // when the engine's pipe closes.
        static_cast<void>(displays_->remove_monitors());
    }
}

void HostOrchestrator::fail(HostFailure failure, std::uint32_t detail) {
    if (failed_.exchange(true)) {
        return;
    }
    stop_streaming();
    observer_->on_host_failed(failure, detail);
    core_->close();
}

void HostOrchestrator::on_access_unit(std::uint8_t stream_id, std::span<const std::uint8_t> annexb,
                                      bool keyframe, std::uint64_t capture_time_us) {
    const osplay_access_unit unit{.stream_id = stream_id,
                                  .keyframe = keyframe ? std::uint8_t{1} : std::uint8_t{0},
                                  .codec_config = keyframe ? std::uint8_t{1} : std::uint8_t{0},
                                  .frame_id = 0,
                                  .capture_time_us = capture_time_us,
                                  .data = annexb.data(),
                                  .length = annexb.size()};
    // Fails only while the session is closing; the frame is of no use then.
    static_cast<void>(core_->push_access_unit(unit));
    if (dump_ && stream_id == 0) {
        dump_->write(annexb);
    }
}

void HostOrchestrator::on_cursor_shape(std::uint8_t stream_id, std::uint32_t shape_id,
                                       const CursorImage& image) {
    const osplay_cursor_shape shape{.stream_id = stream_id,
                                    .shape_id = shape_id,
                                    .width = image.width,
                                    .height = image.height,
                                    .hotspot_x = image.hotspot_x,
                                    .hotspot_y = image.hotspot_y,
                                    .bgra = image.bgra.data(),
                                    .length = image.bgra.size()};
    static_cast<void>(core_->push_cursor_shape(shape));
}

void HostOrchestrator::on_cursor_position(std::uint8_t stream_id, bool visible,
                                          std::uint32_t shape_id, std::int32_t x, std::int32_t y,
                                          std::uint64_t capture_time_us) {
    const osplay_cursor_position position{.stream_id = stream_id,
                                          .visible = visible ? std::uint8_t{1} : std::uint8_t{0},
                                          .shape_id = shape_id,
                                          .x = x,
                                          .y = y,
                                          .capture_time_us = capture_time_us};
    static_cast<void>(core_->push_cursor_position(position));
}

void HostOrchestrator::on_pipeline_failed(PipelineError error) {
    post([this, error] { fail(HostFailure::Pipeline, static_cast<std::uint32_t>(error)); });
}

void HostOrchestrator::on_capture_stats(std::uint8_t stream_id, const CaptureStats& stats) {
    observer_->on_capture_stats(stream_id, stats);
}

} // namespace osplay::engine
