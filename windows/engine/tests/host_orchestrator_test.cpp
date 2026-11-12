#include "host/host_orchestrator.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace {

using osplay::engine::CaptureStats;
using osplay::engine::CursorImage;
using osplay::engine::DisplayError;
using osplay::engine::HostCorePort;
using osplay::engine::HostFailure;
using osplay::engine::HostObserver;
using osplay::engine::HostOrchestrator;
using osplay::engine::kBitsPerPixel;
using osplay::engine::MonitorRect;
using osplay::engine::PipelineError;
using osplay::engine::PipelineSink;
using osplay::engine::StreamBox;
using osplay::engine::StreamDump;
using osplay::engine::StreamPipeline;
using osplay::engine::StreamPipelineFactory;
using osplay::engine::StreamSettings;
using osplay::engine::VirtualDisplayProvider;
using osplay::engine::VirtualMonitorConfig;
using osplay::engine::VirtualMonitorStatus;
namespace win = osplay::win;

class FakeCore final : public HostCorePort {
public:
    std::vector<osplay_stream_config> configured;
    std::vector<std::vector<std::uint8_t>> units;
    std::vector<std::uint32_t> shapes;
    std::vector<osplay_cursor_position> positions;
    bool closed = false;
    bool refused_busy = false;
    osplay_result configure_result = OSPLAY_OK;

    osplay_result configure_streams(std::span<const osplay_stream_config> streams) override {
        const std::scoped_lock lock(mutex);
        configured.assign(streams.begin(), streams.end());
        return configure_result;
    }
    osplay_result push_access_unit(const osplay_access_unit& unit) override {
        const std::scoped_lock lock(mutex);
        units.emplace_back(unit.data, unit.data + unit.length);
        return OSPLAY_OK;
    }
    osplay_result push_cursor_shape(const osplay_cursor_shape& shape) override {
        const std::scoped_lock lock(mutex);
        shapes.push_back(shape.shape_id);
        return OSPLAY_OK;
    }
    osplay_result push_cursor_position(const osplay_cursor_position& position) override {
        const std::scoped_lock lock(mutex);
        positions.push_back(position);
        return OSPLAY_OK;
    }
    void close() override {
        const std::scoped_lock lock(mutex);
        closed = true;
    }
    void refuse_busy() override {
        const std::scoped_lock lock(mutex);
        refused_busy = true;
    }
    std::mutex mutex;
};

class FakeDisplays final : public VirtualDisplayProvider {
public:
    std::vector<VirtualMonitorConfig> created; // every monitor of every call, in order
    std::vector<std::size_t> batches;          // monitors per call
    int removed = 0;
    std::optional<DisplayError> create_error;
    std::size_t available = 4; // what other sessions left

    win::Expected<std::vector<std::wstring>, DisplayError>
    create_monitors(std::span<const VirtualMonitorConfig> configs) override {
        if (create_error) {
            return win::Expected<std::vector<std::wstring>, DisplayError>::failure(*create_error);
        }
        std::vector<std::wstring> names;
        for (const VirtualMonitorConfig& config : configs) {
            created.push_back(config);
            names.push_back(L"\\\\.\\DISPLAY" + std::to_wstring(10 + names.size()));
        }
        batches.push_back(configs.size());
        return names;
    }
    win::Status<DisplayError> remove_monitors() override {
        ++removed;
        return win::Status<DisplayError>::ok();
    }
    win::Expected<VirtualMonitorStatus, DisplayError> status() override {
        return VirtualMonitorStatus{};
    }
    [[nodiscard]] std::size_t available_monitors() const override { return available; }
};

class FakePipeline final : public StreamPipeline {
public:
    FakePipeline(int& keyframe_requests, std::vector<std::uint32_t>& bitrates)
        : keyframe_requests_(&keyframe_requests), bitrates_(&bitrates) {}
    void request_keyframe() override { ++*keyframe_requests_; }
    void set_bitrate(std::uint32_t kbps) override { bitrates_->push_back(kbps); }
    [[nodiscard]] std::optional<MonitorRect> monitor_rect() const override {
        return MonitorRect{.left = 1920, .top = 0, .right = 4480, .bottom = 1664};
    }

private:
    int* keyframe_requests_;
    std::vector<std::uint32_t>* bitrates_;
};

class FakePipelines final : public StreamPipelineFactory {
public:
    std::vector<StreamSettings> started;
    PipelineSink* sink = nullptr;     // the last pipeline's
    std::vector<PipelineSink*> sinks; // every pipeline's, in start order
    int keyframe_requests = 0;
    std::vector<std::uint32_t> bitrates;
    int alive = 0;
    std::optional<PipelineError> start_error;

    win::Expected<std::unique_ptr<StreamPipeline>, PipelineError>
    start(const StreamSettings& settings, PipelineSink& output) override {
        if (start_error) {
            return win::Expected<std::unique_ptr<StreamPipeline>, PipelineError>::failure(
                *start_error);
        }
        started.push_back(settings);
        sink = &output;
        sinks.push_back(&output);
        return std::unique_ptr<StreamPipeline>(
            std::make_unique<FakePipeline>(keyframe_requests, bitrates));
    }
};

class RecordingObserver final : public HostObserver {
public:
    std::optional<MonitorRect> streaming;
    std::size_t streaming_displays = 0;
    std::optional<HostFailure> failure;
    std::uint32_t detail = 0;
    std::vector<std::pair<std::uint8_t, CaptureStats>> capture_stats;

    void on_streaming(const MonitorRect& first_monitor, std::size_t display_count) override {
        streaming = first_monitor;
        streaming_displays = display_count;
    }
    void on_host_failed(HostFailure host_failure, std::uint32_t failure_detail) override {
        failure = host_failure;
        detail = failure_detail;
    }
    void on_capture_stats(std::uint8_t stream_id, const CaptureStats& stats) override {
        capture_stats.emplace_back(stream_id, stats);
    }
};

osplay_monitor monitor(std::uint8_t id = 4) {
    return {.monitor_id = id,
            .width_px = 2560,
            .height_px = 1664,
            .refresh_millihertz = 60'000,
            .physical_width_mm = 302,
            .physical_height_mm = 196,
            .scale_percent = 200,
            .name = "Built-in Retina Display"};
}

struct Harness {
    FakeCore core;
    FakeDisplays displays;
    FakePipelines pipelines;
    RecordingObserver observer;
};

TEST(HostOrchestrator, TheViewerMonitorBecomesTheVirtualMonitorAndTheStream) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor(4)};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();

    ASSERT_EQ(h.displays.created.size(), 1U);
    EXPECT_EQ(h.displays.created[0].width, 2560U);
    EXPECT_EQ(h.displays.created[0].height, 1664U);
    EXPECT_EQ(h.displays.created[0].refresh_millihertz, 60'000U);
    ASSERT_EQ(h.pipelines.started.size(), 1U);
    EXPECT_EQ(h.pipelines.started[0].bitrate_kbps, 25'559U);
    ASSERT_EQ(h.core.configured.size(), 1U);
    EXPECT_EQ(h.core.configured[0].monitor_id, 4);
    EXPECT_EQ(h.core.configured[0].cursor_mode, OSPLAY_CURSOR_SEPARATE);
    ASSERT_TRUE(h.observer.streaming.has_value());
    EXPECT_EQ(h.observer.streaming->left, 1920);
    EXPECT_EQ(h.observer.streaming_displays, 1U);
}

TEST(HostOrchestrator, AViewerGetsOnlyTheMonitorsOtherSessionsLeft) {
    Harness h;
    h.displays.available = 1;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor(1), monitor(2)};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();

    EXPECT_EQ(h.displays.created.size(), 1U);
    ASSERT_EQ(h.core.configured.size(), 1U);
    EXPECT_EQ(h.core.configured[0].monitor_id, 1);
    EXPECT_EQ(h.observer.streaming_displays, 1U);
    EXPECT_FALSE(h.core.refused_busy);
}

TEST(HostOrchestrator, WithEveryMonitorTakenTheViewerIsTurnedAwayBeforePairing) {
    Harness h;
    h.displays.available = 0;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);

    EXPECT_FALSE(orchestrator.on_peer());
    EXPECT_TRUE(h.core.refused_busy);
    EXPECT_FALSE(h.observer.failure.has_value()) << "a busy host is no failure";
}

TEST(HostOrchestrator, AViewerArrivingWithMonitorsLeftIsWelcome) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    EXPECT_TRUE(orchestrator.on_peer());
    EXPECT_FALSE(h.core.refused_busy);
}

TEST(HostOrchestrator, MonitorsTakenMeanwhileTurnTheViewerAwayToo) {
    Harness h;
    h.displays.create_error = DisplayError::NoMonitorLeft;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor()};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();

    EXPECT_TRUE(h.core.refused_busy);
    EXPECT_FALSE(h.observer.failure.has_value());
    EXPECT_TRUE(h.pipelines.started.empty());
    EXPECT_TRUE(h.core.configured.empty());
}

TEST(HostOrchestrator, TheStreamIsFittedToTheBoxWhileTheVirtualMonitorKeepsItsMode) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer, nullptr,
                                  kBitsPerPixel, StreamBox{.width = 1920, .height = 1080});
    const std::vector<osplay_monitor> monitors{monitor(4)}; // 2560 × 1664
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();

    ASSERT_EQ(h.displays.created.size(), 1U);
    EXPECT_EQ(h.displays.created[0].width, 2560U) << "the virtual monitor in the viewer's mode";
    EXPECT_EQ(h.displays.created[0].height, 1664U);
    ASSERT_EQ(h.core.configured.size(), 1U);
    EXPECT_EQ(h.core.configured[0].width, 1660U) << "1080 high, the monitor's aspect ratio";
    EXPECT_EQ(h.core.configured[0].height, 1080U);
    ASSERT_EQ(h.pipelines.started.size(), 1U);
    EXPECT_EQ(h.pipelines.started[0].width, 1660U) << "captured at monitor size, encoded at this";
    EXPECT_EQ(h.pipelines.started[0].bitrate_kbps,
              HostOrchestrator::stream_for(monitor(4), kBitsPerPixel, {1920, 1080}).bitrate_kbps);
    EXPECT_LT(h.pipelines.started[0].bitrate_kbps, 25'559U) << "less than at the monitor's size";
}

TEST(HostOrchestrator, EveryViewerMonitorGetsItsOwnVirtualMonitorAndStream) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    osplay_monitor small = monitor(9);
    small.width_px = 1920;
    small.height_px = 1080;
    const std::vector<osplay_monitor> monitors{monitor(4), small};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();

    EXPECT_EQ(h.displays.batches, (std::vector<std::size_t>{2}));
    ASSERT_EQ(h.displays.created.size(), 2U);
    EXPECT_EQ(h.displays.created[1].width, 1920U);
    ASSERT_EQ(h.pipelines.started.size(), 2U);
    EXPECT_EQ(h.pipelines.started[0].gdi_name, L"\\\\.\\DISPLAY10");
    EXPECT_EQ(h.pipelines.started[1].gdi_name, L"\\\\.\\DISPLAY11");
    EXPECT_EQ(h.pipelines.started[1].width, 1920U);
    ASSERT_EQ(h.core.configured.size(), 2U);
    EXPECT_EQ(h.core.configured[0].stream_id, 0);
    EXPECT_EQ(h.core.configured[0].monitor_id, 4);
    EXPECT_EQ(h.core.configured[1].stream_id, 1);
    EXPECT_EQ(h.core.configured[1].monitor_id, 9);

    // Each pipeline's output carries its own stream ID.
    const std::vector<std::uint8_t> frame{0, 0, 0, 1, 0x41};
    h.pipelines.sinks[1]->on_cursor_position(true, 1, 5, 5, 1);
    h.pipelines.sinks[0]->on_cursor_position(true, 1, 6, 6, 2);
    ASSERT_EQ(h.core.positions.size(), 2U);
    EXPECT_EQ(h.core.positions[0].stream_id, 1);
    EXPECT_EQ(h.core.positions[1].stream_id, 0);
}

TEST(HostOrchestrator, AtMostFourMonitorsAreMade) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor(0), monitor(1), monitor(2), monitor(3),
                                               monitor(4)};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();
    EXPECT_EQ(h.displays.created.size(), 4U);
    EXPECT_EQ(h.core.configured.size(), 4U);
}

TEST(HostOrchestrator, StoppingOneOfTwoStreamsMakesTheOtherMonitorAgain) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    osplay_monitor small = monitor(9);
    small.width_px = 1920;
    small.height_px = 1080;
    const std::vector<osplay_monitor> monitors{monitor(4), small};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.on_stream_stopped(0);
    orchestrator.wait_idle();
    EXPECT_EQ(h.displays.batches, (std::vector<std::size_t>{2, 1}));
    EXPECT_EQ(h.displays.created.back().width, 1920U) << "the remaining stream's monitor";
    EXPECT_EQ(h.displays.removed, 0);
    ASSERT_EQ(h.pipelines.started.size(), 3U);
    EXPECT_EQ(h.pipelines.started.back().width, 1920U);
    orchestrator.on_keyframe_request(1);
    orchestrator.wait_idle();
    EXPECT_EQ(h.pipelines.keyframe_requests, 1) << "stream 1 keeps its ID";
    orchestrator.on_stream_stopped(1);
    orchestrator.wait_idle();
    EXPECT_EQ(h.displays.removed, 1);
    EXPECT_FALSE(h.core.closed);
}

TEST(HostOrchestrator, AHighRefreshMonitorKeepsItsRate) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    osplay_monitor fast = monitor(0);
    fast.refresh_millihertz = 120'000;
    orchestrator.on_viewer_monitors(std::vector<osplay_monitor>{fast});
    orchestrator.wait_idle();

    ASSERT_EQ(h.displays.created.size(), 1U);
    EXPECT_EQ(h.displays.created[0].refresh_millihertz, 120'000U);
    ASSERT_EQ(h.core.configured.size(), 1U);
    EXPECT_EQ(h.core.configured[0].refresh_millihertz, 120'000U);
}

TEST(HostOrchestrator, ARateAboveTheCapIsClamped) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    osplay_monitor fast = monitor(0);
    fast.refresh_millihertz = 240'000;
    orchestrator.on_viewer_monitors(std::vector<osplay_monitor>{fast});
    orchestrator.wait_idle();

    ASSERT_EQ(h.core.configured.size(), 1U);
    EXPECT_EQ(h.core.configured[0].refresh_millihertz, 120'000U);
}

TEST(HostOrchestrator, PipelineOutputReachesTheCoreAndTheDump) {
    Harness h;
    const auto directory = std::filesystem::temp_directory_path() / "osplay-orchestrator-test";
    std::filesystem::remove_all(directory);
    auto dump = std::make_unique<StreamDump>(directory, "stream.h264");
    const auto path = dump->path();
    {
        HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer, std::move(dump));
        const std::vector<osplay_monitor> monitors{monitor()};
        orchestrator.on_viewer_monitors(monitors);
        orchestrator.wait_idle();
        ASSERT_NE(h.pipelines.sink, nullptr);

        const std::vector<std::uint8_t> keyframe{0, 0, 0, 1, 0x67, 0, 0, 0, 1, 0x65};
        h.pipelines.sink->on_access_unit(keyframe, true, 42);
        h.pipelines.sink->on_cursor_shape(
            7, CursorImage{
                   .width = 1, .height = 1, .hotspot_x = 0, .hotspot_y = 0, .bgra = {1, 2, 3, 4}});
        h.pipelines.sink->on_cursor_position(true, 7, 100, 200, 43);

        orchestrator.on_keyframe_request(0);
        orchestrator.on_keyframe_request(3); // not a stream of this host
        orchestrator.wait_idle();
        EXPECT_EQ(h.pipelines.keyframe_requests, 1);
    }
    ASSERT_EQ(h.core.units.size(), 1U);
    EXPECT_EQ(h.core.shapes, (std::vector<std::uint32_t>{7}));
    ASSERT_EQ(h.core.positions.size(), 1U);
    EXPECT_EQ(h.core.positions[0].y, 200);
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> dumped((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
    EXPECT_EQ(dumped, h.core.units[0]);
    EXPECT_EQ(h.displays.removed, 1) << "the monitor is removed when the orchestrator ends";
}

TEST(HostOrchestrator, ASessionCloseRemovesTheMonitor) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor()};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.on_session_closed();
    orchestrator.wait_idle();
    EXPECT_EQ(h.displays.removed, 1);
    EXPECT_FALSE(h.core.closed);
}

TEST(HostOrchestrator, AStoppedStreamRemovesTheMonitorButKeepsTheSession) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor()};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.on_stream_stopped(1); // not this host's stream
    orchestrator.wait_idle();
    EXPECT_EQ(h.displays.removed, 0);
    orchestrator.on_stream_stopped(0);
    orchestrator.wait_idle();
    EXPECT_EQ(h.displays.removed, 1);
    EXPECT_FALSE(h.core.closed);
}

TEST(HostOrchestrator, ADisplayFailureClosesTheSessionWithTheReason) {
    Harness h;
    h.displays.create_error = DisplayError::HelperUnavailable;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor()};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();
    EXPECT_EQ(h.observer.failure, HostFailure::Display);
    EXPECT_EQ(h.observer.detail, static_cast<std::uint32_t>(DisplayError::HelperUnavailable));
    EXPECT_TRUE(h.core.closed);
    EXPECT_TRUE(h.pipelines.started.empty());
}

TEST(HostOrchestrator, APipelineFailureRemovesTheMonitorAndClosesTheSession) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor()};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();
    h.pipelines.sink->on_pipeline_failed(PipelineError::CaptureFailed);
    orchestrator.wait_idle();
    EXPECT_EQ(h.observer.failure, HostFailure::Pipeline);
    EXPECT_EQ(h.observer.detail, static_cast<std::uint32_t>(PipelineError::CaptureFailed));
    EXPECT_EQ(h.displays.removed, 1);
    EXPECT_TRUE(h.core.closed);
}

TEST(HostOrchestrator, CaptureStatsReachTheObserverWithTheirStream) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor(1), monitor(2)};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();
    h.pipelines.sinks.at(1)->on_capture_stats({.desktop_width = 2560,
                                               .desktop_height = 1664,
                                               .images = 7,
                                               .encoded = 7,
                                               .access_lost = 3,
                                               .reconnects = 3,
                                               .slowest_step_ms = 2});
    ASSERT_EQ(h.observer.capture_stats.size(), 1U);
    EXPECT_EQ(h.observer.capture_stats.front().first, 1);
    EXPECT_EQ(h.observer.capture_stats.front().second.images, 7U);
    EXPECT_EQ(h.observer.capture_stats.front().second.access_lost, 3U);
}

TEST(HostOrchestrator, AnEncoderThatCannotStartEndsTheSession) {
    Harness h;
    h.pipelines.start_error = PipelineError::EncoderUnavailable;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor()};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.wait_idle();
    EXPECT_EQ(h.observer.failure, HostFailure::Pipeline);
    EXPECT_EQ(h.displays.removed, 1);
    EXPECT_TRUE(h.core.closed);
}

TEST(HostOrchestrator, TheCoresBitrateTargetReachesThePipeline) {
    Harness h;
    HostOrchestrator orchestrator(h.core, h.displays, h.pipelines, h.observer);
    const std::vector<osplay_monitor> monitors{monitor()};
    orchestrator.on_viewer_monitors(monitors);
    orchestrator.on_bitrate_target(0, 12'000);
    orchestrator.on_bitrate_target(5, 9'000); // not this host's stream
    orchestrator.wait_idle();
    EXPECT_EQ(h.pipelines.bitrates, (std::vector<std::uint32_t>{12'000}));
}

TEST(HostOrchestrator, BitratesStayWithinLimits) {
    auto small = monitor();
    small.width_px = 640;
    small.height_px = 480;
    small.refresh_millihertz = 24'000;
    EXPECT_EQ(HostOrchestrator::stream_for(small).bitrate_kbps, 2'000U);
}

TEST(HostOrchestrator, TheQualityPresetScalesTheBitrateWithinItsRange) {
    EXPECT_EQ(HostOrchestrator::stream_for(monitor(), 0.06).bitrate_kbps, 15'335U);
    EXPECT_EQ(HostOrchestrator::stream_for(monitor(), 0.16).bitrate_kbps, 40'894U);
    // Out-of-range presets are clamped to 0.3 bits per pixel.
    EXPECT_EQ(HostOrchestrator::stream_for(monitor(), 1.0).bitrate_kbps, 76'677U);
}

} // namespace
