#pragma once

// StreamPipeline on Windows: Desktop Duplication of the virtual monitor, GPU BGRA → NV12, the
// Media Foundation encoder, and the pointer from the duplication's frame information (DEVPLAN
// Phase 8). Everything runs on one thread that owns the D3D11 device and the COM objects.

#include "host/stream_pipeline.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace osplay::engine {

class DuplicationPipeline final : public StreamPipeline {
public:
    /// Starts the pipeline thread and waits until capture and the encoder are running.
    [[nodiscard]] static win::Expected<std::unique_ptr<StreamPipeline>, PipelineError>
    start(const StreamSettings& settings, PipelineSink& sink);

    ~DuplicationPipeline() override;
    DuplicationPipeline(const DuplicationPipeline&) = delete;
    DuplicationPipeline& operator=(const DuplicationPipeline&) = delete;
    DuplicationPipeline(DuplicationPipeline&&) = delete;
    DuplicationPipeline& operator=(DuplicationPipeline&&) = delete;

    void request_keyframe() override { keyframe_requested_ = true; }
    void set_bitrate(std::uint32_t kbps) override { requested_bitrate_kbps_ = kbps; }
    void set_paused(bool paused) override {
        // Resuming sends a keyframe, so the viewer's first picture after a pause is complete.
        if (paused_.exchange(paused, std::memory_order_relaxed) != paused && !paused) {
            keyframe_requested_ = true;
        }
    }
    [[nodiscard]] std::optional<MonitorRect> monitor_rect() const override { return rect_; }

private:
    struct Worker;

    DuplicationPipeline() = default;

    std::atomic<bool> keyframe_requested_{true};
    std::atomic<std::uint32_t> requested_bitrate_kbps_{0}; // 0: no change waiting
    std::atomic<bool> paused_{false};
    std::atomic<bool> stopping_{false};
    std::optional<MonitorRect> rect_;
    std::thread thread_;
};

/// Creates DuplicationPipelines.
class DuplicationPipelineFactory final : public StreamPipelineFactory {
public:
    win::Expected<std::unique_ptr<StreamPipeline>, PipelineError>
    start(const StreamSettings& settings, PipelineSink& sink) override {
        return DuplicationPipeline::start(settings, sink);
    }
};

} // namespace osplay::engine
