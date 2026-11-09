#include "../capture/duplication_pipeline.hpp"
#include "../display/gdi_display_mode_applier.hpp"
#include "../display/pipe_helper_channel.hpp"
#include "../display/virtual_display_hub.hpp"
#include "../engine_tunables.hpp"
#include "../host/host_orchestrator.hpp"
#include "../usb/winusb_pipe.hpp"
#include "android_watcher.hpp"
#include "engine_results.hpp"
#include "session_trust.hpp"
#include "system_random.hpp"

#include <osplay/engine.h>
#include <osplay/osplay.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace {

using osplay::engine::guarded;
using osplay::engine::system_random;
using osplay::engine::to_result;

/// The orchestrator's view of the core session. Detached before the session is destroyed, so the
/// pipeline thread never reaches a dead session.
class CapiCorePort final : public osplay::engine::HostCorePort {
public:
    void attach(osplay_session* session) {
        const std::scoped_lock lock(mutex_);
        session_ = session;
    }
    osplay_session* detach() {
        const std::scoped_lock lock(mutex_);
        osplay_session* session = session_;
        session_ = nullptr;
        return session;
    }

    osplay_result configure_streams(std::span<const osplay_stream_config> streams) override {
        const std::scoped_lock lock(mutex_);
        return session_ == nullptr
                   ? OSPLAY_ERROR_INVALID_STATE
                   : osplay_session_configure_streams(session_, streams.data(), streams.size());
    }
    osplay_result push_access_unit(const osplay_access_unit& unit) override {
        const std::scoped_lock lock(mutex_);
        return session_ == nullptr ? OSPLAY_ERROR_INVALID_STATE
                                   : osplay_session_push_access_unit(session_, &unit);
    }
    osplay_result push_cursor_shape(const osplay_cursor_shape& shape) override {
        const std::scoped_lock lock(mutex_);
        return session_ == nullptr ? OSPLAY_ERROR_INVALID_STATE
                                   : osplay_session_push_cursor_shape(session_, &shape);
    }
    osplay_result push_cursor_position(const osplay_cursor_position& position) override {
        const std::scoped_lock lock(mutex_);
        return session_ == nullptr ? OSPLAY_ERROR_INVALID_STATE
                                   : osplay_session_push_cursor_position(session_, &position);
    }
    void close() override {
        const std::scoped_lock lock(mutex_);
        if (session_ != nullptr) {
            static_cast<void>(osplay_session_close(session_)); // fails only when already closing
        }
    }
    void close_for_sleep() {
        const std::scoped_lock lock(mutex_);
        if (session_ != nullptr) {
            static_cast<void>(osplay_session_close_for_sleep(session_)); // fails only when closing
        }
    }
    void refuse_busy() override {
        const std::scoped_lock lock(mutex_);
        if (session_ != nullptr) {
            static_cast<void>(osplay_session_refuse_busy(session_)); // fails only when closing
        }
    }
    [[nodiscard]] osplay_result request_role_swap() {
        const std::scoped_lock lock(mutex_);
        return session_ == nullptr ? OSPLAY_ERROR_INVALID_STATE
                                   : osplay_session_request_role_swap(session_);
    }
    [[nodiscard]] osplay_result answer_role_swap(osplay_role_swap_answer answer) {
        const std::scoped_lock lock(mutex_);
        return session_ == nullptr ? OSPLAY_ERROR_INVALID_STATE
                                   : osplay_session_answer_role_swap(session_, answer);
    }

private:
    std::mutex mutex_;
    osplay_session* session_ = nullptr;
};

/// The helper serves one client, so every host session of this process shares one connection; it
/// closes, and the helper removes every monitor, when the last session goes.
std::shared_ptr<osplay::engine::VirtualDisplayHub>
shared_hub(osplay::engine::DisplayError& failure) {
    static std::mutex mutex;
    static std::weak_ptr<osplay::engine::VirtualDisplayHub> current;
    const std::scoped_lock lock(mutex);
    if (auto hub = current.lock()) {
        return hub;
    }
    auto channel = osplay::engine::PipeHelperChannel::connect();
    if (!channel) {
        failure = channel.error();
        return nullptr;
    }
    auto hub = std::make_shared<osplay::engine::VirtualDisplayHub>(
        std::move(channel.value()), std::make_unique<osplay::engine::GdiDisplayModeApplier>());
    current = hub;
    return hub;
}

/// Forwards orchestrator events to the shell's callbacks.
class ShellObserver final : public osplay::engine::HostObserver {
public:
    explicit ShellObserver(const osplay_engine_host_callbacks& callbacks) : callbacks_(callbacks) {}

    void on_streaming(const osplay::engine::MonitorRect& first_monitor,
                      std::size_t display_count) override {
        if (callbacks_.on_streaming != nullptr) {
            callbacks_.on_streaming(callbacks_.context, static_cast<uint32_t>(display_count),
                                    first_monitor.left, first_monitor.top, first_monitor.right,
                                    first_monitor.bottom);
        }
    }

    void on_host_failed(osplay::engine::HostFailure failure, std::uint32_t detail) override {
        if (callbacks_.on_failed == nullptr) {
            return;
        }
        osplay_engine_result result = OSPLAY_ENGINE_ERROR_SESSION_FAILED;
        if (failure == osplay::engine::HostFailure::Display) {
            result = to_result(static_cast<osplay::engine::DisplayError>(detail));
        } else if (failure == osplay::engine::HostFailure::Pipeline) {
            result = to_result(static_cast<osplay::engine::PipelineError>(detail));
        }
        callbacks_.on_failed(callbacks_.context, result);
    }

    void on_capture_stats(std::uint8_t stream_id,
                          const osplay::engine::CaptureStats& stats) override {
        if (callbacks_.on_log == nullptr) {
            return;
        }
        const std::string message = std::format(
            "Capture {}: desktop {}x{}, {} images, {} encoded, access lost {}, reconnects {}, "
            "slowest step {} ms",
            stream_id, stats.desktop_width, stats.desktop_height, stats.images, stats.encoded,
            stats.access_lost, stats.reconnects, stats.slowest_step_ms);
        callbacks_.on_log(callbacks_.context, OSPLAY_LOG_INFO, message.c_str());
    }

private:
    osplay_engine_host_callbacks callbacks_;
};

std::unique_ptr<osplay::engine::StreamDump> make_dump(const char* directory_utf8) {
    if (directory_utf8 == nullptr || *directory_utf8 == '\0') {
        return nullptr;
    }
    const std::filesystem::path directory(
        std::u8string(reinterpret_cast<const char8_t*>(directory_utf8)));
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::make_unique<osplay::engine::StreamDump>(
        directory, std::format("osplay-{:%Y%m%d-%H%M%S}-stream0.h264", now));
}

} // namespace

struct osplay_engine_host {
    explicit osplay_engine_host(const osplay_engine_host_callbacks& shell_callbacks)
        : callbacks(shell_callbacks), observer(shell_callbacks) {}

    osplay_engine_host_callbacks callbacks;
    std::unique_ptr<osplay::engine::HubVirtualDisplays> displays;
    osplay::engine::DuplicationPipelineFactory pipelines;
    CapiCorePort port;
    ShellObserver observer;
    std::unique_ptr<osplay::engine::HostOrchestrator> orchestrator;
    osplay_session* session = nullptr;
    osplay::engine::SessionTrust trust; ///< answers trust when started with the stored identity
    /// Started on an Android accessory: the watcher that reported it, told when it is opened.
    std::weak_ptr<osplay::engine::usb::AndroidUsbWatcher> accessory_watcher;
    std::uint32_t accessory_id = 0;
};

namespace {

osplay_engine_host* host_of(void* context) {
    return static_cast<osplay_engine_host*>(context);
}

osplay_session_callbacks core_callbacks(osplay_engine_host* host) {
    osplay_session_callbacks callbacks{};
    callbacks.context = host;
    callbacks.on_state = [](void* context, osplay_session_state state, osplay_result reason) {
        auto* self = host_of(context);
        if (state == OSPLAY_SESSION_CLOSED) {
            self->orchestrator->on_session_closed();
        }
        self->callbacks.on_state(self->callbacks.context, state, reason);
    };
    callbacks.on_peer = [](void* context, const osplay_peer_info* peer) {
        auto* self = host_of(context);
        if (!self->orchestrator->on_peer()) {
            return; // turned away: other sessions use every virtual monitor
        }
        self->trust.introduced(peer->device_name);
        if (const auto watcher = self->accessory_watcher.lock()) {
            watcher->accessory_opened(self->accessory_id);
        }
        if (self->callbacks.on_peer != nullptr) {
            self->callbacks.on_peer(self->callbacks.context, peer->platform, peer->device_name);
        }
    };
    callbacks.on_trust_query = [](void* context, const uint8_t* key) {
        auto* self = host_of(context);
        return self->trust.stored() ? self->trust.trust(key)
                                    : self->callbacks.on_trust_query(self->callbacks.context, key);
    };
    callbacks.on_pairing_code = [](void* context, uint32_t code, const uint8_t* key) {
        auto* self = host_of(context);
        if (self->callbacks.on_pairing_code != nullptr) {
            self->callbacks.on_pairing_code(self->callbacks.context, code, key);
        }
    };
    callbacks.on_paired = [](void* context, const uint8_t* key) {
        auto* self = host_of(context);
        if (self->trust.stored()) {
            self->trust.paired(key);
            osplay::engine::SessionTrust::paired_stored(key);
        }
        if (self->callbacks.on_paired != nullptr) {
            self->callbacks.on_paired(self->callbacks.context, key);
        }
    };
    callbacks.on_viewer_monitors = [](void* context, const osplay_monitor* monitors, size_t count) {
        host_of(context)->orchestrator->on_viewer_monitors(std::span(monitors, count));
    };
    callbacks.on_keyframe_request = [](void* context, uint8_t stream_id) {
        host_of(context)->orchestrator->on_keyframe_request(stream_id);
    };
    callbacks.on_stream_stopped = [](void* context, uint8_t stream_id) {
        host_of(context)->orchestrator->on_stream_stopped(stream_id);
    };
    callbacks.on_bitrate_target = [](void* context, uint8_t stream_id, uint32_t kbps) {
        host_of(context)->orchestrator->on_bitrate_target(stream_id, kbps);
    };
    callbacks.on_stats = [](void* context, const osplay_session_stats* stats) {
        auto* self = host_of(context);
        if (self->callbacks.on_stats != nullptr) {
            self->callbacks.on_stats(self->callbacks.context, stats);
        }
    };
    callbacks.on_role_swap_request = [](void* context, const uint8_t* key) {
        auto* self = host_of(context);
        if (self->callbacks.on_role_swap_request != nullptr) {
            const osplay::engine::PeerDeviceId peer(key);
            self->callbacks.on_role_swap_request(self->callbacks.context, peer.bytes.data());
        }
    };
    callbacks.on_role_swap_answer = [](void* context, osplay_role_swap_answer answer,
                                       const uint8_t* key) {
        auto* self = host_of(context);
        if (self->callbacks.on_role_swap_answer != nullptr) {
            const osplay::engine::PeerDeviceId peer(key);
            self->callbacks.on_role_swap_answer(self->callbacks.context, answer, peer.bytes.data());
        }
    };
    callbacks.on_log = [](void* context, osplay_log_level level, const char* message) {
        auto* self = host_of(context);
        if (self->callbacks.on_log != nullptr) {
            self->callbacks.on_log(self->callbacks.context, level, message);
        }
    };
    return callbacks;
}

/// Every start function; `create` makes the core session (listening, over usbmuxd or an Android
/// accessory). `prepare` sets up the host before its session can call back.
template <typename Create>
osplay_engine_result start_host(const osplay_engine_host_config* config,
                                const osplay_engine_host_callbacks* callbacks,
                                osplay_engine_host** out_host, const Create& create,
                                const std::function<void(osplay_engine_host&)>& prepare = {}) {
    return guarded([&] {
        if (out_host == nullptr) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        *out_host = nullptr;
        if (config == nullptr || config->device_name_utf8 == nullptr || callbacks == nullptr ||
            callbacks->on_state == nullptr ||
            (config->secret_key != nullptr && callbacks->on_trust_query == nullptr)) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        osplay::engine::DisplayError hub_failure{};
        auto hub = shared_hub(hub_failure);
        if (!hub) {
            return to_result(hub_failure);
        }
        auto host = std::make_unique<osplay_engine_host>(*callbacks);
        osplay::engine::KeyStoreError store_failure{};
        auto secret = host->trust.secret_for(config->secret_key, store_failure);
        if (!secret) {
            return to_result(store_failure);
        }
        // Revoking or removing the peer ends the session (non-blocking Bye).
        host->trust.set_end_session([self = host.get()] { self->port.close(); });
        host->displays = std::make_unique<osplay::engine::HubVirtualDisplays>(std::move(hub));
        host->orchestrator = std::make_unique<osplay::engine::HostOrchestrator>(
            host->port, *host->displays, host->pipelines, host->observer,
            make_dump(config->dump_directory_utf8),
            config->millibits_per_pixel == 0 ? osplay::engine::kBitsPerPixel
                                             : config->millibits_per_pixel / 1000.0,
            osplay::engine::StreamBox{.width = config->max_stream_width,
                                      .height = config->max_stream_height});

        if (prepare) {
            prepare(*host);
        }
        const osplay_session_config session_config{
            .platform = OSPLAY_PLATFORM_WINDOWS,
            .device_name = config->device_name_utf8,
            .capabilities = {.codecs = OSPLAY_CODEC_H264_ANNEX_B,
                             .max_streams =
                                 static_cast<std::uint8_t>(osplay::engine::kMaxHostStreams),
                             .flags = 0,
                             .max_width = 7680,
                             .max_height = 4320,
                             .max_refresh_hz = 240},
            .monitors = nullptr,
            .monitor_count = 0,
            .control_port = config->control_port,
        };
        const osplay_random_source random{.fill = system_random, .context = nullptr};
        const osplay_session_callbacks session_callbacks = core_callbacks(host.get());
        osplay_session* session = nullptr;
        const osplay_result created =
            create(session_config, secret->data(), random, session_callbacks, &session);
        SecureZeroMemory(secret->data(), secret->size());
        if (created != OSPLAY_OK) {
            return created == OSPLAY_ERROR_INVALID_ARGUMENT ? OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT
                                                            : OSPLAY_ENGINE_ERROR_SESSION_FAILED;
        }
        host->session = session;
        host->port.attach(session);
        *out_host = host.release(); // ownership passes to the caller until _destroy
        return OSPLAY_ENGINE_OK;
    });
}

} // namespace

extern "C" {

osplay_engine_result osplay_engine_host_start(const osplay_engine_host_config* config,
                                              const osplay_engine_host_callbacks* callbacks,
                                              osplay_engine_host** out_host) {
    return start_host(config, callbacks, out_host,
                      [](const osplay_session_config& session_config, const uint8_t* secret,
                         const osplay_random_source& random,
                         const osplay_session_callbacks& session_callbacks,
                         osplay_session** session) {
                          return osplay_host_session_create(&session_config, secret, &random,
                                                            &session_callbacks, session);
                      });
}

osplay_engine_result osplay_engine_host_start_usb(const osplay_engine_host_config* config,
                                                  const osplay_engine_host_callbacks* callbacks,
                                                  uint32_t usb_device_id,
                                                  osplay_engine_host** out_host) {
    return start_host(config, callbacks, out_host,
                      [usb_device_id](const osplay_session_config& session_config,
                                      const uint8_t* secret, const osplay_random_source& random,
                                      const osplay_session_callbacks& session_callbacks,
                                      osplay_session** session) {
                          return osplay_host_session_create_usb(&session_config, secret, &random,
                                                                &session_callbacks, usb_device_id,
                                                                OSPLAY_USB_PORT, session);
                      });
}

osplay_engine_result osplay_engine_host_start_accessory(
    const osplay_engine_host_config* config, const osplay_engine_host_callbacks* callbacks,
    osplay_engine_android_watcher* watcher, uint32_t accessory_id, osplay_engine_host** out_host) {
    const auto path =
        watcher == nullptr ? std::nullopt : watcher->watcher->interface_path(accessory_id);
    if (!path) {
        if (out_host != nullptr) {
            *out_host = nullptr;
        }
        return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
    }
    return start_host(
        config, callbacks, out_host,
        [&path](const osplay_session_config& session_config, const uint8_t* secret,
                const osplay_random_source& random,
                const osplay_session_callbacks& session_callbacks, osplay_session** session) {
            // Opened last, so nothing before the core can fail while holding it.
            auto pipe = osplay::engine::usb::WinUsbPipe::open(*path);
            if (!pipe) {
                return OSPLAY_ERROR_NETWORK;
            }
            const osplay_byte_pipe byte_pipe =
                osplay::engine::usb::WinUsbPipe::to_byte_pipe(std::move(pipe));
            // The core releases the pipe, also when this fails.
            return osplay_host_session_create_accessory(&session_config, secret, &random,
                                                        &session_callbacks, &byte_pipe, session);
        },
        [watcher, accessory_id](osplay_engine_host& host) {
            host.accessory_watcher = watcher->watcher;
            host.accessory_id = accessory_id;
        });
}

osplay_engine_result osplay_engine_usb_watcher_start(const osplay_usb_callbacks* callbacks,
                                                     osplay_usb_watcher** out_watcher) {
    return guarded([&] {
        switch (osplay_usb_watcher_create(callbacks, out_watcher)) {
        case OSPLAY_OK:
            return OSPLAY_ENGINE_OK;
        case OSPLAY_ERROR_INVALID_ARGUMENT:
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        default:
            return OSPLAY_ENGINE_ERROR_INTERNAL;
        }
    });
}

void osplay_engine_usb_watcher_destroy(osplay_usb_watcher* watcher) {
    osplay_usb_watcher_destroy(watcher);
}

osplay_engine_result osplay_engine_host_control_port(osplay_engine_host* host, uint16_t* out_port) {
    return guarded([&] {
        if (host == nullptr || out_port == nullptr) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        return osplay_session_control_port(host->session, out_port) == OSPLAY_OK
                   ? OSPLAY_ENGINE_OK
                   : OSPLAY_ENGINE_ERROR_SESSION_FAILED;
    });
}

osplay_engine_result osplay_engine_host_confirm_pairing(osplay_engine_host* host,
                                                        uint8_t accepted) {
    return guarded([&] {
        if (host == nullptr || accepted > 1) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        return osplay_session_confirm_pairing(host->session, accepted) == OSPLAY_OK
                   ? OSPLAY_ENGINE_OK
                   : OSPLAY_ENGINE_ERROR_SESSION_FAILED;
    });
}

osplay_engine_result osplay_engine_host_request_role_swap(osplay_engine_host* host) {
    return guarded([&] {
        if (host == nullptr) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        return osplay::engine::role_swap_result(host->port.request_role_swap());
    });
}

osplay_engine_result osplay_engine_host_answer_role_swap(osplay_engine_host* host,
                                                         osplay_role_swap_answer answer) {
    return guarded([&] {
        if (host == nullptr) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        return osplay::engine::role_swap_result(host->port.answer_role_swap(answer));
    });
}

osplay_engine_result osplay_engine_host_stop(osplay_engine_host* host) {
    return guarded([&] {
        if (host == nullptr) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        host->port.close();
        return OSPLAY_ENGINE_OK;
    });
}

osplay_engine_result osplay_engine_host_suspend(osplay_engine_host* host) {
    return guarded([&] {
        if (host == nullptr) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        host->port.close_for_sleep();
        return OSPLAY_ENGINE_OK;
    });
}

osplay_engine_result osplay_engine_host_set_paused(osplay_engine_host* host, int paused) {
    return guarded([&] {
        if (host == nullptr || host->orchestrator == nullptr) {
            return OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT;
        }
        host->orchestrator->set_paused(paused != 0);
        return OSPLAY_ENGINE_OK;
    });
}

void osplay_engine_host_destroy(osplay_engine_host* host) {
    // Taking ownership back from the caller.
    const std::unique_ptr<osplay_engine_host> owned(host);
    if (!owned) {
        return;
    }
    static_cast<void>(guarded([&] {
        // Order matters: no pushes into a dying session, no callbacks into a dead orchestrator.
        osplay_session* session = owned->port.detach();
        osplay_session_destroy(session); // joins the core's I/O thread
        owned->orchestrator.reset();     // stops capture and removes the virtual monitor
        owned->displays.reset();
        return OSPLAY_ENGINE_OK;
    }));
}

} // extern "C"
