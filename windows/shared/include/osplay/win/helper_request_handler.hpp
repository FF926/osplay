#pragma once

#include <osplay/win/helper_protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace osplay::win::helper {

/// The Virtual Display Driver's devices, in a stable order. Each one drives one virtual monitor:
/// two monitors on one device make the whole desktop stutter (DECISIONS.md, 2026-10-25).
class VirtualMonitorDevices {
public:
    virtual ~VirtualMonitorDevices() = default;
    /// False while no device exists: the driver was never installed.
    [[nodiscard]] virtual bool is_installed() = 0;
    [[nodiscard]] virtual std::size_t count() = 0;
    /// Adds a device after the existing ones; it starts enabled. False when that failed.
    [[nodiscard]] virtual bool add() = 0;
    [[nodiscard]] virtual bool is_enabled(std::size_t index) = 0;
    /// Persistent across reboots. Returns false when the device could not be changed.
    [[nodiscard]] virtual bool set_enabled(std::size_t index, bool enabled) = 0;
};

/// Where the driver reads its settings from.
class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    [[nodiscard]] virtual bool write(const std::string& contents) = 0;
};

class HelperLog {
public:
    virtual ~HelperLog() = default;
    virtual void info(std::string_view message) = 0;
    virtual void warning(std::string_view message) = 0;
    virtual void error(std::string_view message) = 0;
};

/// Executes helper commands. One instance serves one connected client at a time; the virtual
/// monitors exist only while that client's session is active.
class RequestHandler {
public:
    RequestHandler(VirtualMonitorDevices& devices, SettingsStore& settings, HelperLog& log);

    /// Decodes, validates and executes one request message; always returns an encoded response.
    [[nodiscard]] Bytes handle(std::span<const std::uint8_t> message);
    [[nodiscard]] Response handle(const Request& request);

    /// Removes the virtual monitors and forgets the configuration. Called when the service starts
    /// (to clear monitors left behind by a crash) and whenever the client disconnects.
    void reset();

    /// As reset, but only sweeps when this client configured monitors: health checks and idle
    /// clients leave nothing behind, and the sweep must not delay the next client's connection.
    void reset_if_configured();

private:
    Response configure(const std::vector<MonitorConfig>& monitors);
    /// Adds the named devices that do not exist yet and enables them; the others are untouched.
    Response enable(std::uint8_t devices);
    /// Disables the named devices; the others are untouched.
    Response disable(std::uint8_t devices);
    Response status();
    /// Disables every enabled device from `first` on: how many, or nothing if one failed.
    std::optional<std::size_t> disable_from(std::size_t first);
    /// Bit i: device i is enabled (the first kMaxMonitors devices).
    [[nodiscard]] std::uint8_t enabled_devices();

    VirtualMonitorDevices& devices_;
    SettingsStore& settings_;
    HelperLog& log_;
    std::vector<MonitorConfig> monitors_; // empty until configured
};

[[nodiscard]] std::string_view to_string(DecodeError error);
[[nodiscard]] std::string_view to_string(Command command);

} // namespace osplay::win::helper
