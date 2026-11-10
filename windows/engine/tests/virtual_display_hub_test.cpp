#include "../src/display/virtual_display_hub.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using osplay::engine::DisplayError;
using osplay::engine::DisplayModeApplier;
using osplay::engine::HelperChannel;
using osplay::engine::HubVirtualDisplays;
using osplay::engine::VirtualDisplayHub;
using osplay::engine::VirtualMonitorConfig;
namespace helper = osplay::win::helper;
namespace win = osplay::win;

/// Records requests and answers each with the next queued reply, or Ok when none is queued.
class FakeChannel final : public HelperChannel {
public:
    std::vector<helper::Request> requests;
    std::deque<win::Expected<helper::Bytes, DisplayError>> replies;

    win::Expected<helper::Bytes, DisplayError>
    transact(std::span<const std::uint8_t> request) override {
        const auto decoded = helper::decode_request(request);
        EXPECT_TRUE(decoded.has_value());
        if (decoded) {
            requests.push_back(decoded.value());
        }
        if (replies.empty()) {
            return helper::encode(helper::Response{.result = helper::ResultCode::Ok, .status = {}});
        }
        auto reply = replies.front();
        replies.pop_front();
        return reply;
    }

    void reply(helper::ResultCode code, helper::MonitorStatus status = {}) {
        replies.emplace_back(helper::encode(helper::Response{.result = code, .status = status}));
    }

    [[nodiscard]] std::vector<helper::Command> commands() const {
        std::vector<helper::Command> seen;
        for (const helper::Request& request : requests) {
            seen.push_back(request.command);
        }
        return seen;
    }
};

/// The desktop: every Enable adds one monitor per device, named after the count so far.
class FakeModeApplier final : public DisplayModeApplier {
public:
    std::vector<std::wstring> monitors;
    std::vector<std::vector<std::wstring>> existing_seen;
    std::vector<VirtualMonitorConfig> applied;
    std::optional<DisplayError> error;

    win::Expected<std::vector<std::wstring>, DisplayError> current_monitors() override {
        return monitors;
    }

    win::Expected<std::vector<std::wstring>, DisplayError>
    apply(std::span<const VirtualMonitorConfig> configs,
          std::span<const std::wstring> existing) override {
        existing_seen.emplace_back(existing.begin(), existing.end());
        applied.assign(configs.begin(), configs.end());
        if (error) {
            return win::Expected<std::vector<std::wstring>, DisplayError>::failure(*error);
        }
        std::vector<std::wstring> names;
        for (std::size_t i = 0; i < configs.size(); ++i) {
            names.push_back(L"DISPLAY" + std::to_wstring(monitors.size() + 1));
            monitors.push_back(names.back());
        }
        return names;
    }
};

struct Fixture {
    FakeChannel* channel = nullptr;
    FakeModeApplier* applier = nullptr;
    std::shared_ptr<VirtualDisplayHub> hub;

    Fixture() {
        auto owned_channel = std::make_unique<FakeChannel>();
        auto owned_applier = std::make_unique<FakeModeApplier>();
        channel = owned_channel.get();
        applier = owned_applier.get();
        hub =
            std::make_shared<VirtualDisplayHub>(std::move(owned_channel), std::move(owned_applier));
    }
};

VirtualMonitorConfig config(std::uint32_t width = 2560) {
    return {.width = width, .height = 1440, .refresh_millihertz = 60'000, .gpu_name = ""};
}

TEST(VirtualDisplayHub, CreateConfiguresThenEnablesItsDevicesOneByOne) {
    Fixture f;
    HubVirtualDisplays session(f.hub);

    const auto names = session.create_monitors(std::vector{config(), config(1920)});

    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names.value(), (std::vector<std::wstring>{L"DISPLAY1", L"DISPLAY2"}));
    ASSERT_EQ(f.channel->requests.size(), 3U);
    EXPECT_EQ(f.channel->requests[0].command, helper::Command::Configure);
    EXPECT_EQ(f.channel->requests[0].monitors, (std::vector{config(), config(1920)}));
    // One device per Enable request: devices enabled together share a driver process until
    // their next start, and the second monitor would then offer only the first one's modes (#30).
    EXPECT_EQ(f.channel->requests[1].command, helper::Command::Enable);
    EXPECT_EQ(f.channel->requests[1].devices, 0b01);
    EXPECT_EQ(f.channel->requests[2].command, helper::Command::Enable);
    EXPECT_EQ(f.channel->requests[2].devices, 0b10);
    EXPECT_EQ(f.applier->applied, (std::vector{config(), config(1920)}));
}

TEST(VirtualDisplayHub, ASecondSessionGetsFreeDevicesAndLeavesTheFirstAlone) {
    Fixture f;
    HubVirtualDisplays first(f.hub);
    HubVirtualDisplays second(f.hub);
    ASSERT_TRUE(first.create_monitors(std::vector{config(), config()}).has_value());
    f.channel->requests.clear();

    const auto names = second.create_monitors(std::vector{config(1920)});

    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names.value(), std::vector<std::wstring>{L"DISPLAY3"});
    EXPECT_EQ(f.channel->commands(),
              (std::vector{helper::Command::Configure, helper::Command::Enable}))
        << "no Disable: the first session's monitors keep streaming";
    EXPECT_EQ(f.channel->requests[0].monitors, (std::vector{config(), config(), config(1920)}))
        << "every mode in use, for any device that starts";
    EXPECT_EQ(f.channel->requests[1].devices, 0b100);
    EXPECT_EQ(f.applier->existing_seen.back(),
              (std::vector<std::wstring>{L"DISPLAY1", L"DISPLAY2"}));
}

TEST(VirtualDisplayHub, ReplacingASessionsMonitorsTouchesOnlyItsDevices) {
    Fixture f;
    HubVirtualDisplays first(f.hub);
    HubVirtualDisplays second(f.hub);
    ASSERT_TRUE(first.create_monitors(std::vector{config(), config()}).has_value());
    ASSERT_TRUE(second.create_monitors(std::vector{config(1920)}).has_value());
    f.channel->requests.clear();

    ASSERT_TRUE(first.create_monitors(std::vector{config(1280)}).has_value());

    ASSERT_EQ(f.channel->requests.size(), 3U);
    EXPECT_EQ(f.channel->requests[0].command, helper::Command::Disable);
    EXPECT_EQ(f.channel->requests[0].devices, 0b011);
    EXPECT_EQ(f.channel->requests[1].monitors, (std::vector{config(1280), config(1920)}));
    EXPECT_EQ(f.channel->requests[2].command, helper::Command::Enable);
    EXPECT_EQ(f.channel->requests[2].devices, 0b001);
}

TEST(VirtualDisplayHub, RemovingFreesOnlyThatSessionsDevices) {
    Fixture f;
    HubVirtualDisplays first(f.hub);
    HubVirtualDisplays second(f.hub);
    ASSERT_TRUE(first.create_monitors(std::vector{config(), config()}).has_value());
    ASSERT_TRUE(second.create_monitors(std::vector{config()}).has_value());
    EXPECT_EQ(second.available_monitors(), 2U);
    f.channel->requests.clear();

    ASSERT_TRUE(first.remove_monitors().has_value());

    ASSERT_EQ(f.channel->requests.size(), 1U);
    EXPECT_EQ(f.channel->requests[0].command, helper::Command::Disable);
    EXPECT_EQ(f.channel->requests[0].devices, 0b011);
    EXPECT_EQ(second.available_monitors(), 4U);
    EXPECT_EQ(first.available_monitors(), 3U);
}

TEST(VirtualDisplayHub, NoMonitorIsLeftWhileOthersUseThemAll) {
    Fixture f;
    HubVirtualDisplays first(f.hub);
    HubVirtualDisplays second(f.hub);
    ASSERT_TRUE(first.create_monitors(std::vector(helper::kMaxMonitors, config())).has_value());
    f.channel->requests.clear();

    EXPECT_EQ(second.available_monitors(), 0U);
    const auto result = second.create_monitors(std::vector{config()});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisplayError::NoMonitorLeft);
    EXPECT_TRUE(f.channel->requests.empty());
}

TEST(VirtualDisplayHub, AClosingSessionRemovesItsMonitors) {
    Fixture f;
    {
        HubVirtualDisplays session(f.hub);
        ASSERT_TRUE(session.create_monitors(std::vector{config()}).has_value());
        f.channel->requests.clear();
    }
    ASSERT_EQ(f.channel->requests.size(), 1U);
    EXPECT_EQ(f.channel->requests[0].command, helper::Command::Disable);
    EXPECT_EQ(f.hub->free_monitors(), helper::kMaxMonitors);
}

TEST(VirtualDisplayHub, ModeFailureRemovesTheNewMonitorsOnly) {
    Fixture f;
    HubVirtualDisplays first(f.hub);
    HubVirtualDisplays second(f.hub);
    ASSERT_TRUE(first.create_monitors(std::vector{config()}).has_value());
    f.channel->requests.clear();
    f.applier->error = DisplayError::ModeChangeFailed;

    const auto result = second.create_monitors(std::vector{config()});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisplayError::ModeChangeFailed);
    ASSERT_EQ(f.channel->requests.size(), 3U);
    EXPECT_EQ(f.channel->requests[2].command, helper::Command::Disable);
    EXPECT_EQ(f.channel->requests[2].devices, 0b10);
    EXPECT_EQ(f.hub->free_monitors(), helper::kMaxMonitors - 1);
}

TEST(VirtualDisplayHub, EnableFailureSkipsTheModeChange) {
    Fixture f;
    HubVirtualDisplays session(f.hub);
    f.channel->reply(helper::ResultCode::Ok);
    f.channel->reply(helper::ResultCode::DeviceControlFailed);

    const auto result = session.create_monitors(std::vector{config()});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisplayError::DeviceControlFailed);
    EXPECT_TRUE(f.applier->applied.empty());
    EXPECT_EQ(f.hub->free_monitors(), helper::kMaxMonitors);
}

TEST(VirtualDisplayHub, InvalidConfigNeverReachesTheHelper) {
    Fixture f;
    HubVirtualDisplays session(f.hub);
    VirtualMonitorConfig bad = config();
    bad.refresh_millihertz = 1;

    const auto result = session.create_monitors(std::vector{bad});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisplayError::InvalidArgument);
    EXPECT_TRUE(f.channel->requests.empty());
}

TEST(VirtualDisplayHub, ConfigureFailureStopsBeforeEnable) {
    Fixture f;
    HubVirtualDisplays session(f.hub);
    f.channel->reply(helper::ResultCode::ConfigWriteFailed);

    const auto result = session.create_monitors(std::vector{config()});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisplayError::ConfigWriteFailed);
    EXPECT_EQ(f.channel->requests.size(), 1U);
    EXPECT_EQ(f.hub->free_monitors(), helper::kMaxMonitors);
}

TEST(VirtualDisplayHub, MapsHelperResultCodes) {
    EXPECT_EQ(osplay::engine::to_display_error(helper::ResultCode::VddNotInstalled),
              DisplayError::VddNotInstalled);
    EXPECT_EQ(osplay::engine::to_display_error(helper::ResultCode::DeviceControlFailed),
              DisplayError::DeviceControlFailed);
    EXPECT_EQ(osplay::engine::to_display_error(helper::ResultCode::UnsupportedVersion),
              DisplayError::ProtocolMismatch);
    EXPECT_EQ(osplay::engine::to_display_error(helper::ResultCode::ValueOutOfRange),
              DisplayError::InvalidArgument);
    EXPECT_EQ(osplay::engine::to_display_error(helper::ResultCode::NotConfigured),
              DisplayError::Rejected);
}

TEST(VirtualDisplayHub, TransportErrorsPassThrough) {
    Fixture f;
    HubVirtualDisplays session(f.hub);
    ASSERT_TRUE(session.create_monitors(std::vector{config()}).has_value());
    f.channel->replies.emplace_back(
        win::Expected<helper::Bytes, DisplayError>::failure(DisplayError::ConnectionLost));

    const auto result = session.remove_monitors();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisplayError::ConnectionLost);
}

TEST(VirtualDisplayHub, GarbledResponsesAreProtocolMismatches) {
    Fixture f;
    HubVirtualDisplays session(f.hub);
    f.channel->replies.emplace_back(helper::Bytes{0x01, 0x02, 0x03});

    const auto result = session.status();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisplayError::ProtocolMismatch);
}

TEST(VirtualDisplayHub, StatusReturnsHelperState) {
    Fixture f;
    HubVirtualDisplays session(f.hub);
    const helper::MonitorStatus state{.vdd_installed = true,
                                      .enabled_devices = 0b1,
                                      .configured = true,
                                      .width = 2560,
                                      .height = 1664,
                                      .refresh_millihertz = 60'000};
    f.channel->reply(helper::ResultCode::Ok, state);

    const auto result = session.status();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), state);
}

} // namespace
