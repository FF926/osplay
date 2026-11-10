/*
 * Osplay Windows engine — public C API consumed by the WPF shell through P/Invoke.
 *
 * Every function returns an osplay_engine_result; none throws or produces user-facing text.
 * Unless stated otherwise, functions block and may be called from any thread, but calls on the
 * same handle must not run concurrently.
 */
#ifndef OSPLAY_ENGINE_H
#define OSPLAY_ENGINE_H

#include <osplay/osplay.h>

#include <stddef.h>
#include <stdint.h>

#ifdef OSPLAY_ENGINE_BUILDING
#define OSPLAY_ENGINE_API __declspec(dllexport)
#else
#define OSPLAY_ENGINE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Stable values: the shell maps each one to a localized message. Never renumber. */
/* NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size): plain C for P/Invoke */
typedef enum osplay_engine_result {
    OSPLAY_ENGINE_OK = 0,
    OSPLAY_ENGINE_ERROR_INVALID_ARGUMENT = 1,
    OSPLAY_ENGINE_ERROR_HELPER_UNAVAILABLE = 2,
    OSPLAY_ENGINE_ERROR_HELPER_ACCESS_DENIED = 3,
    OSPLAY_ENGINE_ERROR_HELPER_NOT_TRUSTED = 4,
    OSPLAY_ENGINE_ERROR_PROTOCOL_MISMATCH = 5,
    OSPLAY_ENGINE_ERROR_CONNECTION_LOST = 6,
    OSPLAY_ENGINE_ERROR_VDD_NOT_INSTALLED = 7,
    OSPLAY_ENGINE_ERROR_DEVICE_CONTROL_FAILED = 8,
    OSPLAY_ENGINE_ERROR_CONFIG_WRITE_FAILED = 9,
    OSPLAY_ENGINE_ERROR_REJECTED = 10,
    OSPLAY_ENGINE_ERROR_INTERNAL = 11,
    OSPLAY_ENGINE_ERROR_MONITOR_NOT_FOUND = 12,
    OSPLAY_ENGINE_ERROR_MODE_CHANGE_FAILED = 13,
    OSPLAY_ENGINE_ERROR_HELPER_BUSY = 14,
    OSPLAY_ENGINE_ERROR_SESSION_FAILED = 15,      /* the core refused or ended the session */
    OSPLAY_ENGINE_ERROR_CAPTURE_FAILED = 16,      /* Desktop Duplication could not run */
    OSPLAY_ENGINE_ERROR_ENCODER_UNAVAILABLE = 17, /* no hardware H.264 encoder for the stream */
    OSPLAY_ENGINE_ERROR_ENCODER_FAILED = 18,
    OSPLAY_ENGINE_ERROR_DECODER_UNAVAILABLE = 19,   /* no hardware H.264 decoder for the stream */
    OSPLAY_ENGINE_ERROR_PRESENTATION_FAILED = 20,   /* the viewer window could not be shown */
    OSPLAY_ENGINE_ERROR_KEY_STORE_UNAVAILABLE = 21, /* %APPDATA%\Osplay cannot be read or written */
    OSPLAY_ENGINE_ERROR_KEY_STORE_DAMAGED = 22, /* a stored identity or peer list is unreadable */
    OSPLAY_ENGINE_ERROR_DISCOVERY_UNAVAILABLE = 23, /* the system DNS-SD service refused */
    OSPLAY_ENGINE_ERROR_PEER_TOO_OLD = 24, /* the other computer's Osplay lacks this feature */
    OSPLAY_ENGINE_RESULT_MAX_ENUM = 0x7FFFFFFF
} osplay_engine_result;
/* NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size) */

/* Version of the core library linked into the engine. Thread-safe. */
OSPLAY_ENGINE_API osplay_version osplay_engine_core_version(void);

/* --- Device identity and paired devices ---------------------------------------------------- */

/*
 * The identity key and the paired devices live in %APPDATA%\Osplay, encrypted with DPAPI for the
 * current user. The secret key never leaves the engine: host and viewer sessions started without
 * a secret key use this identity and answer trust queries from this list. Thread-safe.
 */
#define OSPLAY_ENGINE_NAME_SIZE 65 /* 64 bytes of UTF-8 and the NUL */

/* The public key and the device ID of this PC, creating the identity on first use. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_identity(uint8_t* out_public_key,
                                                              uint8_t* out_device_id);

typedef struct osplay_engine_peer {
    uint8_t public_key[OSPLAY_KEY_SIZE];
    char name_utf8[OSPLAY_ENGINE_NAME_SIZE];
    uint8_t revoked; /* 1: refused without asking */
    int64_t last_seen_unix;
} osplay_engine_peer;

/* Fills up to `capacity` peers and writes the number stored to `out_count`. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_peers(osplay_engine_peer* out_peers,
                                                           size_t capacity, size_t* out_count);

/* Revoked devices are refused from now on; removing one lets it pair again. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_peer_revoke(const uint8_t* public_key);
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_peer_remove(const uint8_t* public_key);

/* --- Discovery (Bonjour) -------------------------------------------------------------------- */

typedef struct osplay_engine_device {
    char label[OSPLAY_ENGINE_NAME_SIZE]; /* stable identifier, matches on_lost */
    osplay_advertisement advertisement;  /* name, device ID, roles, platform, protocol version */
    char address_utf8[64];               /* where to connect: IP literal or .local name */
    uint16_t port;
} osplay_engine_device;

/* Callbacks run on system thread pool threads, one at a time, and must return quickly. */
typedef struct osplay_engine_browser_callbacks {
    void* context;
    void (*on_found)(void* context, const osplay_engine_device* device); /* again on changes */
    void (*on_lost)(void* context, const char* label);
} osplay_engine_browser_callbacks;

typedef struct osplay_engine_browser osplay_engine_browser;       /* opaque */
typedef struct osplay_engine_advertiser osplay_engine_advertiser; /* opaque */

OSPLAY_ENGINE_API osplay_engine_result osplay_engine_browser_start(
    const osplay_engine_browser_callbacks* callbacks, osplay_engine_browser** out_browser);
/* No callback runs after this returns. Accepts NULL. */
OSPLAY_ENGINE_API void osplay_engine_browser_destroy(osplay_engine_browser* browser);

/* Advertises this PC (its identity's device ID, `name_utf8`, `roles`) on `port`. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_advertiser_start(
    const char* name_utf8, uint8_t roles, uint16_t port, osplay_engine_advertiser** out_advertiser);
OSPLAY_ENGINE_API void osplay_engine_advertiser_destroy(osplay_engine_advertiser* advertiser);

/* --- Virtual display (Windows host) --------------------------------------------------------- */

typedef struct osplay_virtual_display osplay_virtual_display; /* opaque */

typedef struct osplay_virtual_monitor_config {
    uint32_t width;              /* pixels, 640...7680 */
    uint32_t height;             /* pixels, 480...4320 */
    uint32_t refresh_millihertz; /* 24000...240000 */
    const char* gpu_name_utf8;   /* DXGI adapter description, or NULL to let the driver choose */
} osplay_virtual_monitor_config;

typedef struct osplay_virtual_monitor_status {
    uint8_t driver_installed;
    uint8_t monitor_present;
    uint8_t configured;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihertz;
} osplay_virtual_monitor_status;

/* Connects to the OsplayHelper service. The monitor lives at most as long as the handle. */
OSPLAY_ENGINE_API osplay_engine_result
osplay_virtual_display_open(osplay_virtual_display** out_display);

/* Adds `count` (1...4) virtual monitors and sets exactly these modes, replacing any previous
 * configuration. All of them name the same GPU. Blocks until the monitors have joined the desktop
 * (a few seconds at most). If a mode cannot be set, the monitors are removed again and the error
 * is returned. */
OSPLAY_ENGINE_API osplay_engine_result osplay_virtual_display_create_monitors(
    osplay_virtual_display* display, const osplay_virtual_monitor_config* configs, size_t count);

OSPLAY_ENGINE_API osplay_engine_result
osplay_virtual_display_remove_monitor(osplay_virtual_display* display);

OSPLAY_ENGINE_API osplay_engine_result osplay_virtual_display_status(
    osplay_virtual_display* display, osplay_virtual_monitor_status* out_status);

/* Removes the monitor (if present) and releases the handle. Accepts NULL. */
OSPLAY_ENGINE_API void osplay_virtual_display_close(osplay_virtual_display* display);

/* --- Host session (Windows shares its screen) ------------------------------------------------ */

/*
 * A host session: listens for one viewer, adds a virtual monitor in the mode of each viewer
 * monitor, captures them with Desktop Duplication, encodes them with the hardware H.264 encoder
 * and sends the pointer as cursor packets. It needs the OsplayHelper service. Several host
 * sessions of one process share four virtual monitors; a viewer gets the ones left and is turned
 * away (it sees OSPLAY_ERROR_PEER_BUSY) when none is.
 */
typedef struct osplay_engine_host osplay_engine_host; /* opaque */

typedef struct osplay_engine_host_config {
    /* NULL: the stored identity, and trust from the paired devices (pairings are stored).
     * Otherwise this key, and the caller answers on_trust_query (developer tools). */
    const uint8_t* secret_key;
    const char* device_name_utf8;    /* shown to the viewer */
    const char* dump_directory_utf8; /* debug: also write the stream as .h264 here, or NULL */
    /* Stream quality: target bits per pixel per frame, in thousandths (100 = 0.1); 0: default. */
    uint32_t millibits_per_pixel;
    /* TCP port to listen on, 0 for any; pass the previous session's
     * (osplay_engine_host_control_port) so the advertisement stays right. A taken port falls back
     * to any. */
    uint16_t control_port;
    /* Largest stream size: each viewer monitor's stream is fitted inside max_stream_width ×
     * max_stream_height (osplay_fit_stream_size) while its virtual monitor keeps the monitor's
     * mode. 0 × 0: streams at the monitor's own size. */
    uint32_t max_stream_width;
    uint32_t max_stream_height;
} osplay_engine_host_config;

/*
 * Callbacks run on engine threads and must return quickly; they must not call
 * osplay_engine_host_destroy. `on_state` is required, and `on_trust_query` when a secret key is
 * given (with the stored identity the engine answers it from the paired devices).
 */
typedef struct osplay_engine_host_callbacks {
    void* context;
    /* `reason` is OSPLAY_OK unless the core closed the session because of an error. */
    void (*on_state)(void* context, osplay_session_state state, osplay_result reason);
    osplay_trust (*on_trust_query)(void* context, const uint8_t* peer_public_key);
    void (*on_pairing_code)(void* context, uint32_t code, const uint8_t* peer_public_key);
    void (*on_paired)(void* context, const uint8_t* peer_public_key);
    /* `display_count` virtual monitors are streaming; the first one's place on the desktop in
     * desktop coordinates. */
    void (*on_streaming)(void* context, uint32_t display_count, int32_t left, int32_t top,
                         int32_t right, int32_t bottom);
    /* The engine ended the session; a closed state follows. */
    void (*on_failed)(void* context, osplay_engine_result reason);
    void (*on_stats)(void* context, const osplay_session_stats* stats);
    void (*on_log)(void* context, osplay_log_level level, const char* message);
    /* The other device introduced itself (before any pairing code). Not called for a viewer the
     * engine turns away because no virtual monitor is left for it. */
    void (*on_peer)(void* context, osplay_platform platform, const char* device_name_utf8);
    /* The other computer asks to reverse the roles: answer with
     * osplay_engine_host_answer_role_swap, not from this callback. `peer_device_id`
     * (OSPLAY_DEVICE_ID_SIZE bytes) names the computer to reconnect to after the swap. */
    void (*on_role_swap_request)(void* context, const uint8_t* peer_device_id);
    /* The answer to osplay_engine_host_request_role_swap; after OSPLAY_ROLE_SWAP_ACCEPTED the
     * session closes cleanly. */
    void (*on_role_swap_answer)(void* context, osplay_role_swap_answer answer,
                                const uint8_t* peer_device_id);
} osplay_engine_host_callbacks;

/* Connects to the helper service (once for every host session of this process) and starts
 * listening on an ephemeral TCP port. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_start(
    const osplay_engine_host_config* config, const osplay_engine_host_callbacks* callbacks,
    osplay_engine_host** out_host);

/* As _start, but reaches the iPad `usb_device_id` (from osplay_engine_usb_watcher_start) on the
 * USB cable through the Apple Mobile Device Service (Apple Devices or iTunes). The session closes
 * with OSPLAY_ERROR_NETWORK when the iPad is not waiting for a computer; config->control_port is
 * ignored. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_start_usb(
    const osplay_engine_host_config* config, const osplay_engine_host_callbacks* callbacks,
    uint32_t usb_device_id, osplay_engine_host** out_host);

/* Follows the iPads on a USB cable (osplay_usb_watcher_create in osplay.h): without the Apple
 * Mobile Device Service none are reported and the watcher keeps trying. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_usb_watcher_start(
    const osplay_usb_callbacks* callbacks, osplay_usb_watcher** out_watcher);

/* No callback runs after it returns. Accepts NULL. */
OSPLAY_ENGINE_API void osplay_engine_usb_watcher_destroy(osplay_usb_watcher* watcher);

/*
 * Android devices on a USB cable: while the watcher runs, every phone or tablet plugged in (or
 * already there) is switched to Osplay's accessory through UsbDk, and each one that comes back as
 * an accessory on WinUSB is reported. Without UsbDk nothing is switched. A device with USB
 * debugging on is not switched, since Windows would keep its accessory on the MTP driver, which
 * Osplay cannot open: it is reported with on_debugging_on so the shell can ask to turn debugging
 * off or use the network.
 */
typedef struct osplay_engine_android_watcher osplay_engine_android_watcher; /* opaque */

/* Callbacks run on the watcher's thread, one at a time, and must return quickly. */
typedef struct osplay_engine_android_callbacks {
    void* context;
    void (*on_attached)(void* context, uint32_t accessory_id);
    void (*on_detached)(void* context, uint32_t accessory_id);
    void (*on_debugging_on)(void* context);
    void (*on_log)(void* context, osplay_log_level level, const char* message);
} osplay_engine_android_callbacks;

OSPLAY_ENGINE_API osplay_engine_result osplay_engine_android_watcher_start(
    const osplay_engine_android_callbacks* callbacks, osplay_engine_android_watcher** out_watcher);

/* No callback runs after it returns. Accepts NULL. */
OSPLAY_ENGINE_API void
osplay_engine_android_watcher_destroy(osplay_engine_android_watcher* watcher);

/* As _start, but over the accessory `accessory_id` reported by `watcher`. The session waits for
 * Osplay on the device to open the accessory; config->control_port is ignored. INVALID_ARGUMENT
 * when the accessory is no longer attached. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_start_accessory(
    const osplay_engine_host_config* config, const osplay_engine_host_callbacks* callbacks,
    osplay_engine_android_watcher* watcher, uint32_t accessory_id, osplay_engine_host** out_host);

/* The TCP port to advertise. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_control_port(osplay_engine_host* host,
                                                                       uint16_t* out_port);

/* Answers on_pairing_code (`accepted` is 0 or 1). */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_confirm_pairing(osplay_engine_host* host,
                                                                          uint8_t accepted);

/* Asks the viewer to reverse the roles (streaming only); PEER_TOO_OLD when its Osplay cannot. */
OSPLAY_ENGINE_API osplay_engine_result
osplay_engine_host_request_role_swap(osplay_engine_host* host);

/* Answers on_role_swap_request: ACCEPTED closes the session, CANNOT_HOST or BUSY refuses. */
OSPLAY_ENGINE_API osplay_engine_result
osplay_engine_host_answer_role_swap(osplay_engine_host* host, osplay_role_swap_answer answer);

/* Ends the session with Bye without waiting; on_state reports CLOSED. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_stop(osplay_engine_host* host);

/* As _stop, but tells the viewer this PC is going to sleep, so it waits for it to come back. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_suspend(osplay_engine_host* host);

/* Pauses or resumes every connected viewer's streams (`paused` is 0 or 1): paused streams encode
   black frames and send no pointer, so the viewers show nothing of this PC. The session stays
   up, and a viewer that connects later starts paused too. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_host_set_paused(osplay_engine_host* host,
                                                                     int paused);

/* Stops everything, removes the virtual monitor and releases the handle. Accepts NULL. */
OSPLAY_ENGINE_API void osplay_engine_host_destroy(osplay_engine_host* host);

/* --- Viewer session (Windows shows a remote screen) ------------------------------------------- */

#define OSPLAY_ENGINE_MONITOR_NAME_SIZE 65 /* 64 bytes of UTF-8 and the NUL */

/* A physical monitor this PC can offer (never the Virtual Display Driver's). */
typedef struct osplay_engine_monitor {
    uint8_t monitor_id; /* by desktop position, left to right; valid until the layout changes */
    uint16_t width_px;
    uint16_t height_px;
    uint32_t refresh_millihertz;
    uint16_t physical_width_mm; /* 0 when unknown */
    uint16_t physical_height_mm;
    uint16_t scale_percent;
    int32_t left; /* place on the desktop, physical pixels */
    int32_t top;
    int32_t right;
    int32_t bottom;
    char name_utf8[OSPLAY_ENGINE_MONITOR_NAME_SIZE]; /* the monitor's own name, may be empty */
} osplay_engine_monitor;

/* Fills up to `capacity` monitors; `capacity` must be at least OSPLAY_MAX_MONITORS. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_list_monitors(
    osplay_engine_monitor* out_monitors, size_t capacity, size_t* out_count);

/*
 * A viewer session: connects to a host, offers monitors, and shows each stream full screen on the
 * monitor the host chose (hardware decoding, flip-model presentation). The global hotkey hides and
 * shows every stream; hidden streams are paused on the host. A monitor that disappears stops only
 * its own stream.
 */
typedef struct osplay_engine_viewer osplay_engine_viewer; /* opaque */

typedef struct osplay_engine_viewer_config {
    const uint8_t* secret_key;    /* as in osplay_engine_host_config */
    const char* device_name_utf8; /* shown to the host */
    const char* host_utf8;        /* the host's address */
    uint16_t port;                /* the host's control port */
    const uint8_t* monitor_ids;   /* ids from osplay_engine_list_monitors to offer, or NULL: all */
    size_t monitor_id_count;
    /* Hide/show hotkey: RegisterHotKey MOD_* bits and a virtual-key code; 0 and 0: default. */
    uint32_t hotkey_modifiers;
    uint32_t hotkey_key;
    /* (fork) Non-zero: present in a normal movable window instead of covering a monitor. */
    int windowed;
} osplay_engine_viewer_config;

/* Callbacks run on engine threads and must return quickly; they must not call
 * osplay_engine_viewer_destroy. `on_state` is required, and `on_trust_query` when a secret key
 * is given. */
typedef struct osplay_engine_viewer_callbacks {
    void* context;
    void (*on_state)(void* context, osplay_session_state state, osplay_result reason);
    osplay_trust (*on_trust_query)(void* context, const uint8_t* peer_public_key);
    void (*on_pairing_code)(void* context, uint32_t code, const uint8_t* peer_public_key);
    void (*on_paired)(void* context, const uint8_t* peer_public_key);
    /* The streams were hidden (0) or shown (1), by the hotkey or set_visible. */
    void (*on_visibility)(void* context, uint8_t visible);
    /* One stream could not be shown and was stopped; the session continues. */
    void (*on_stream_failed)(void* context, uint8_t stream_id, osplay_engine_result reason);
    void (*on_stats)(void* context, const osplay_session_stats* stats);
    void (*on_log)(void* context, osplay_log_level level, const char* message);
    /* The other device introduced itself (before any pairing code). */
    void (*on_peer)(void* context, osplay_platform platform, const char* device_name_utf8);
    /* The other computer asks to reverse the roles: answer with
     * osplay_engine_viewer_answer_role_swap, not from this callback. `peer_device_id`
     * (OSPLAY_DEVICE_ID_SIZE bytes) names the computer to reconnect to after the swap. */
    void (*on_role_swap_request)(void* context, const uint8_t* peer_device_id);
    /* The answer to osplay_engine_viewer_request_role_swap; after OSPLAY_ROLE_SWAP_ACCEPTED the
     * session closes cleanly. */
    void (*on_role_swap_answer)(void* context, osplay_role_swap_answer answer,
                                const uint8_t* peer_device_id);
    /* An offered monitor changed its mode (resolution, refresh rate, scaling): the session is
     * closing so the shell can connect again with the new mode. */
    void (*on_monitors_changed)(void* context);
} osplay_engine_viewer_callbacks;

typedef struct osplay_engine_stream_figures {
    uint8_t stream_id;
    uint64_t frames_presented;
    uint64_t decode_errors;
} osplay_engine_stream_figures;

/* Starts connecting. MONITOR_NOT_FOUND when none of the requested monitors is attached. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_viewer_start(
    const osplay_engine_viewer_config* config, const osplay_engine_viewer_callbacks* callbacks,
    osplay_engine_viewer** out_viewer);

/* Whether the hide/show hotkey could be registered (another program may own it). */
OSPLAY_ENGINE_API osplay_engine_result
osplay_engine_viewer_hotkey_registered(osplay_engine_viewer* viewer, uint8_t* out_registered);

/* Answers on_pairing_code (`accepted` is 0 or 1). */
OSPLAY_ENGINE_API osplay_engine_result
osplay_engine_viewer_confirm_pairing(osplay_engine_viewer* viewer, uint8_t accepted);

/* Hides (0) or shows (1) every stream, like the hotkey. */
OSPLAY_ENGINE_API osplay_engine_result
osplay_engine_viewer_set_visible(osplay_engine_viewer* viewer, uint8_t visible);

/* Presentation figures of every running stream; `capacity` must be at least OSPLAY_MAX_STREAMS. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_viewer_stream_stats(
    osplay_engine_viewer* viewer, osplay_engine_stream_figures* out_stats, size_t capacity,
    size_t* out_count);

/* Draws `text_utf8` (one line, at most 255 bytes) in a corner of the stream; NULL or empty hides
 * it. The shell formats the text (localized) from on_stats. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_viewer_set_overlay(
    osplay_engine_viewer* viewer, uint8_t stream_id, const char* text_utf8);

/* As osplay_engine_host_request_role_swap and _answer_role_swap, for a viewer session. */
OSPLAY_ENGINE_API osplay_engine_result
osplay_engine_viewer_request_role_swap(osplay_engine_viewer* viewer);
OSPLAY_ENGINE_API osplay_engine_result
osplay_engine_viewer_answer_role_swap(osplay_engine_viewer* viewer, osplay_role_swap_answer answer);

/* Ends the session with Bye without waiting; on_state reports CLOSED. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_viewer_stop(osplay_engine_viewer* viewer);

/* As _stop, but tells the host this PC is going to sleep. */
OSPLAY_ENGINE_API osplay_engine_result osplay_engine_viewer_suspend(osplay_engine_viewer* viewer);

/* Stops everything, closes the windows and releases the handle. Accepts NULL. */
OSPLAY_ENGINE_API void osplay_engine_viewer_destroy(osplay_engine_viewer* viewer);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OSPLAY_ENGINE_H */
