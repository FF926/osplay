#include "virtual_display_hub.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace osplay::engine {

namespace helper = win::helper;

namespace {

std::uint8_t device_bit(std::size_t index) {
    return static_cast<std::uint8_t>(1U << index);
}

} // namespace

VirtualDisplayHub::VirtualDisplayHub(std::unique_ptr<HelperChannel> channel,
                                     std::unique_ptr<DisplayModeApplier> mode_applier)
    : channel_(std::move(channel)), mode_applier_(std::move(mode_applier)) {}

win::Expected<std::vector<std::wstring>, DisplayError>
VirtualDisplayHub::create_monitors(const void* owner,
                                   std::span<const VirtualMonitorConfig> configs) {
    using Result = win::Expected<std::vector<std::wstring>, DisplayError>;
    const std::vector<VirtualMonitorConfig> monitors(configs.begin(), configs.end());
    if (!helper::is_valid(monitors)) {
        return Result::failure(DisplayError::InvalidArgument);
    }
    const std::scoped_lock lock(mutex_);
    if (const auto released = release(owner); !released) {
        return Result::failure(released.error());
    }
    std::vector<std::size_t> picked;
    for (std::size_t index = 0; index < slots_.size() && picked.size() < monitors.size(); ++index) {
        if (slots_.at(index).owner == nullptr) {
            picked.push_back(index);
        }
    }
    if (picked.size() < monitors.size()) {
        return Result::failure(DisplayError::NoMonitorLeft);
    }
    for (std::size_t i = 0; i < picked.size(); ++i) {
        slots_.at(picked[i]) = Slot{.owner = owner, .config = monitors[i]};
    }
    used_ = static_cast<std::size_t>(
        std::ranges::count_if(slots_, [](const Slot& slot) { return slot.owner != nullptr; }));

    // Every device reads the same settings when it starts, so they list every mode in use.
    if (const auto configured =
            send_expecting_ok({.command = helper::Command::Configure, .monitors = modes_in_use()});
        !configured) {
        forget(owner);
        return Result::failure(configured.error());
    }
    // The new monitors are the ones that were not there before their devices started.
    const auto before = mode_applier_->current_monitors();
    if (!before) {
        forget(owner);
        return Result::failure(before.error());
    }
    // Devices are enabled ONE PER REQUEST (issue #30): devices enabled together start in one
    // shared driver process until their next start, and the second monitor then offers only the
    // first one's modes. One request per device gives each new device a fresh process of its own.
    for (std::size_t i = 0; i < picked.size(); ++i) {
        if (const auto enabled = send_expecting_ok({.command = helper::Command::Enable,
                                                    .monitors = {},
                                                    .devices = device_bit(picked[i])});
            !enabled) {
            forget(owner);
            return Result::failure(enabled.error());
        }
    }
    // Windows gives a new monitor its own default mode; the exact modes are set from the user
    // session.
    auto applied = mode_applier_->apply(monitors, before.value());
    if (!applied) {
        // Monitors with the wrong size are worse than none: remove them and report why.
        static_cast<void>(release(owner));
        return Result::failure(applied.error());
    }
    return applied;
}

win::Status<DisplayError> VirtualDisplayHub::remove_monitors(const void* owner) {
    const std::scoped_lock lock(mutex_);
    return release(owner);
}

win::Expected<VirtualMonitorStatus, DisplayError> VirtualDisplayHub::status() {
    using Result = win::Expected<VirtualMonitorStatus, DisplayError>;
    const std::scoped_lock lock(mutex_);
    const auto response = send({.command = helper::Command::Status, .monitors = {}});
    if (!response) {
        return Result::failure(response.error());
    }
    if (response.value().result != helper::ResultCode::Ok) {
        return Result::failure(to_display_error(response.value().result));
    }
    return response.value().status;
}

std::size_t VirtualDisplayHub::free_monitors() const {
    return slots_.size() - used_.load();
}

win::Status<DisplayError> VirtualDisplayHub::release(const void* owner) {
    const std::uint8_t devices = devices_of(owner);
    forget(owner);
    if (devices == 0) {
        return win::Status<DisplayError>::ok();
    }
    return send_expecting_ok(
        {.command = helper::Command::Disable, .monitors = {}, .devices = devices});
}

void VirtualDisplayHub::forget(const void* owner) {
    for (Slot& slot : slots_) {
        if (slot.owner == owner) {
            slot = Slot{};
        }
    }
    used_ = static_cast<std::size_t>(
        std::ranges::count_if(slots_, [](const Slot& slot) { return slot.owner != nullptr; }));
}

std::uint8_t VirtualDisplayHub::devices_of(const void* owner) const {
    std::uint8_t devices = 0;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_.at(index).owner == owner) {
            devices |= device_bit(index);
        }
    }
    return devices;
}

std::vector<VirtualMonitorConfig> VirtualDisplayHub::modes_in_use() const {
    std::vector<VirtualMonitorConfig> modes;
    for (const Slot& slot : slots_) {
        if (slot.owner != nullptr) {
            modes.push_back(slot.config);
        }
    }
    return modes;
}

win::Expected<helper::Response, DisplayError>
VirtualDisplayHub::send(const helper::Request& request) {
    using Result = win::Expected<helper::Response, DisplayError>;
    const helper::Bytes encoded = helper::encode(request);
    const auto reply = channel_->transact(encoded);
    if (!reply) {
        return Result::failure(reply.error());
    }
    const auto response = helper::decode_response(reply.value());
    if (!response) {
        return Result::failure(DisplayError::ProtocolMismatch);
    }
    return response.value();
}

win::Status<DisplayError> VirtualDisplayHub::send_expecting_ok(const helper::Request& request) {
    const auto response = send(request);
    if (!response) {
        return win::Status<DisplayError>::failure(response.error());
    }
    if (response.value().result != helper::ResultCode::Ok) {
        return win::Status<DisplayError>::failure(to_display_error(response.value().result));
    }
    return win::Status<DisplayError>::ok();
}

HubVirtualDisplays::HubVirtualDisplays(std::shared_ptr<VirtualDisplayHub> hub)
    : hub_(std::move(hub)) {}

HubVirtualDisplays::~HubVirtualDisplays() {
    try {
        // A helper that cannot be reached removes the monitors itself when the last session
        // closes its connection.
        static_cast<void>(hub_->remove_monitors(this));
    } catch (...) {
        // Only allocation or locking can throw here, and a session that cannot let its monitors
        // go is unrecoverable; this makes the implicit terminate explicit.
        std::terminate();
    }
}

win::Expected<std::vector<std::wstring>, DisplayError>
HubVirtualDisplays::create_monitors(std::span<const VirtualMonitorConfig> configs) {
    auto created = hub_->create_monitors(this, configs);
    own_ = created ? configs.size() : 0;
    return created;
}

win::Status<DisplayError> HubVirtualDisplays::remove_monitors() {
    own_ = 0;
    return hub_->remove_monitors(this);
}

win::Expected<VirtualMonitorStatus, DisplayError> HubVirtualDisplays::status() {
    return hub_->status();
}

std::size_t HubVirtualDisplays::available_monitors() const {
    return hub_->free_monitors() + own_.load();
}

DisplayError to_display_error(helper::ResultCode code) {
    switch (code) {
    case helper::ResultCode::VddNotInstalled:
        return DisplayError::VddNotInstalled;
    case helper::ResultCode::DeviceControlFailed:
        return DisplayError::DeviceControlFailed;
    case helper::ResultCode::ConfigWriteFailed:
        return DisplayError::ConfigWriteFailed;
    case helper::ResultCode::UnsupportedVersion:
        return DisplayError::ProtocolMismatch;
    case helper::ResultCode::ValueOutOfRange:
        return DisplayError::InvalidArgument;
    case helper::ResultCode::Ok:
    case helper::ResultCode::MalformedRequest:
    case helper::ResultCode::UnknownCommand:
    case helper::ResultCode::NotConfigured:
        return DisplayError::Rejected;
    }
    return DisplayError::Rejected;
}

} // namespace osplay::engine
