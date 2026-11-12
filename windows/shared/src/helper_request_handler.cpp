#include <osplay/win/helper_request_handler.hpp>
#include <osplay/win/vdd_settings.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace osplay::win::helper {
namespace {

Response with(ResultCode result) {
    return Response{.result = result, .status = {}};
}

/// Whether bit `index` of a device mask is set.
bool named(std::uint8_t devices, std::size_t index) {
    return ((static_cast<unsigned>(devices) >> index) & 1U) != 0;
}

} // namespace

RequestHandler::RequestHandler(VirtualMonitorDevices& devices, SettingsStore& settings,
                               HelperLog& log)
    : devices_(devices), settings_(settings), log_(log) {}

Bytes RequestHandler::handle(std::span<const std::uint8_t> message) {
    const auto request = decode_request(message);
    if (!request) {
        log_.warning(std::string("Rejected request: ") + std::string(to_string(request.error())));
        return encode(with(result_for(request.error())));
    }
    return encode(handle(request.value()));
}

Response RequestHandler::handle(const Request& request) {
    switch (request.command) {
    case Command::Configure:
        return configure(request.monitors);
    case Command::Enable:
        return enable(request.devices);
    case Command::Disable:
        return disable(request.devices);
    case Command::Status:
        return status();
    }
    log_.warning("Rejected request: unknown command");
    return with(ResultCode::UnknownCommand);
}

void RequestHandler::reset() {
    monitors_.clear();
    if (!devices_.is_installed()) {
        return;
    }
    if (const auto removed = disable_from(0); !removed) {
        log_.error("Could not disable the virtual display devices during reset");
    } else if (*removed > 0) {
        log_.info("Virtual monitors removed (reset): " + std::to_string(*removed));
    }
}

void RequestHandler::reset_if_configured() {
    if (monitors_.empty()) {
        return;
    }
    reset();
}

Response RequestHandler::configure(const std::vector<MonitorConfig>& monitors) {
    // decode_request already validates; requests built in-process are checked here.
    if (!is_valid(monitors)) {
        log_.warning("Rejected configure: value out of range");
        return with(ResultCode::ValueOutOfRange);
    }
    if (!settings_.write(render_vdd_settings(monitors))) {
        log_.error("Could not write the virtual display driver settings");
        return with(ResultCode::ConfigWriteFailed);
    }
    monitors_ = monitors;
    std::string modes;
    for (const MonitorConfig& monitor : monitors) {
        modes += (modes.empty() ? "" : ", ") + std::to_string(monitor.width) + "x" +
                 std::to_string(monitor.height) + " @ " +
                 std::to_string(monitor.refresh_millihertz) + " mHz";
    }
    log_.info("Configured " + std::to_string(monitors.size()) + " monitor(s): " + modes);

    // Every device reads the file when it starts; running monitors keep theirs (other sessions'
    // monitors must not restart).
    return with(ResultCode::Ok);
}

Response RequestHandler::enable(std::uint8_t devices) {
    // (fork) A missing device node is not final: add() builds a fresh one from the driver package
    // in the store, so a cleanup that removed every node heals on the next connection instead of
    // failing forever with "the virtual display driver is not installed".
    if (!devices_.is_installed() && !devices_.add()) {
        return with(ResultCode::VddNotInstalled);
    }
    if (monitors_.empty()) {
        return with(ResultCode::NotConfigured);
    }
    if (devices == 0 || (devices & ~kAllDevices) != 0) {
        return with(ResultCode::ValueOutOfRange);
    }
    // A device for each named one; a new device starts at once with the current settings. Never
    // more additions than devices are missing, even if a new device does not show up.
    const auto needed = static_cast<std::size_t>(std::bit_width(static_cast<unsigned>(devices)));
    for (std::size_t present = devices_.count(); present < needed; ++present) {
        if (!devices_.add()) {
            log_.error("Could not add a virtual display device");
            return with(ResultCode::DeviceControlFailed);
        }
        log_.info("Virtual display device added: " + std::to_string(present + 1));
    }
    if (devices_.count() < needed) {
        log_.error("Added virtual display devices did not appear");
        return with(ResultCode::DeviceControlFailed);
    }
    std::size_t added = 0;
    for (std::size_t index = 0; index < needed; ++index) {
        if (!named(devices, index) || devices_.is_enabled(index)) {
            continue;
        }
        if (!devices_.set_enabled(index, true)) {
            log_.error("Could not enable virtual display device " + std::to_string(index));
            return with(ResultCode::DeviceControlFailed);
        }
        ++added;
    }
    if (added > 0) {
        log_.info("Virtual monitors added: " + std::to_string(added));
    }
    return with(ResultCode::Ok);
}

Response RequestHandler::disable(std::uint8_t devices) {
    if (!devices_.is_installed()) {
        return with(ResultCode::VddNotInstalled);
    }
    if (devices == 0 || (devices & ~kAllDevices) != 0) {
        return with(ResultCode::ValueOutOfRange);
    }
    std::size_t removed = 0;
    for (std::size_t index = 0; index < std::min(devices_.count(), kMaxMonitors); ++index) {
        if (!named(devices, index) || !devices_.is_enabled(index)) {
            continue;
        }
        if (!devices_.set_enabled(index, false)) {
            log_.error("Could not disable virtual display device " + std::to_string(index));
            return with(ResultCode::DeviceControlFailed);
        }
        ++removed;
    }
    if (removed > 0) {
        log_.info("Virtual monitors removed: " + std::to_string(removed));
    }
    return with(ResultCode::Ok);
}

Response RequestHandler::status() {
    Response response = with(ResultCode::Ok);
    response.status.vdd_installed = devices_.is_installed();
    response.status.enabled_devices = response.status.vdd_installed ? enabled_devices() : 0;
    response.status.configured = !monitors_.empty();
    response.status.monitor_count = static_cast<std::uint8_t>(monitors_.size());
    if (!monitors_.empty()) {
        response.status.width = monitors_.front().width;
        response.status.height = monitors_.front().height;
        response.status.refresh_millihertz = monitors_.front().refresh_millihertz;
    }
    return response;
}

std::optional<std::size_t> RequestHandler::disable_from(std::size_t first) {
    std::size_t removed = 0;
    for (std::size_t index = first; index < devices_.count(); ++index) {
        if (!devices_.is_enabled(index)) {
            continue;
        }
        if (!devices_.set_enabled(index, false)) {
            return std::nullopt;
        }
        ++removed;
    }
    return removed;
}

std::uint8_t RequestHandler::enabled_devices() {
    std::uint8_t enabled = 0;
    for (std::size_t index = 0; index < std::min(devices_.count(), kMaxMonitors); ++index) {
        if (devices_.is_enabled(index)) {
            enabled |= static_cast<std::uint8_t>(1U << index);
        }
    }
    return enabled;
}

std::string_view to_string(DecodeError error) {
    switch (error) {
    case DecodeError::Truncated:
        return "truncated";
    case DecodeError::TooLarge:
        return "too large";
    case DecodeError::BadMagic:
        return "bad magic";
    case DecodeError::UnsupportedVersion:
        return "unsupported version";
    case DecodeError::UnknownCommand:
        return "unknown command";
    case DecodeError::LengthMismatch:
        return "length mismatch";
    case DecodeError::ValueOutOfRange:
        return "value out of range";
    case DecodeError::InvalidGpuName:
        return "invalid GPU name";
    }
    return "unknown error";
}

std::string_view to_string(Command command) {
    switch (command) {
    case Command::Configure:
        return "configure";
    case Command::Enable:
        return "enable";
    case Command::Disable:
        return "disable";
    case Command::Status:
        return "status";
    }
    return "unknown";
}

} // namespace osplay::win::helper
