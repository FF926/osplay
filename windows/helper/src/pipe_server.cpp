#include "pipe_server.hpp"

#include "console_user.hpp"
#include "helper_constants.hpp"

#include <osplay/win/helper_protocol.hpp>

#include <sddl.h>

#include <array>
#include <memory>
#include <span>
#include <string>

namespace osplay::helper {
namespace {

namespace protocol = win::helper;

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct LocalFreer {
    void operator()(void* memory) const noexcept { LocalFree(memory); }
};

} // namespace

PipeServer::PipeServer(protocol::RequestHandler& handler, protocol::HelperLog& log,
                       HANDLE stop_event, HANDLE session_changed_event)
    : handler_(handler), log_(log), stop_event_(stop_event),
      session_changed_event_(session_changed_event) {}

void PipeServer::run() {
    while (WaitForSingleObject(stop_event_, 0) == WAIT_TIMEOUT) {
        serve_one_client();
    }
}

void PipeServer::serve_one_client() {
    const auto console_sid = console_user_sid();
    if (!console_sid) {
        WaitForSingleObject(stop_event_, kNoConsoleUserRetryMilliseconds);
        return;
    }
    ResetEvent(session_changed_event_);

    // Full access for SYSTEM, read/write for the console user, nobody else; P blocks inheritance.
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + *console_sid + L")";
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &raw_descriptor, nullptr)) {
        log_.error("Cannot build the pipe security descriptor, error " +
                   std::to_string(GetLastError()));
        WaitForSingleObject(stop_event_, kNoConsoleUserRetryMilliseconds);
        return;
    }
    const std::unique_ptr<void, LocalFreer> descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                   .lpSecurityDescriptor = descriptor.get(),
                                   .bInheritHandle = FALSE};

    // FILE_FLAG_FIRST_PIPE_INSTANCE fails if another process already owns the name (squatting).
    const UniqueHandle pipe(CreateNamedPipeW(
        protocol::kPipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        static_cast<DWORD>(protocol::kMaxMessageSize),
        static_cast<DWORD>(protocol::kMaxMessageSize), 0, &attributes));
    if (pipe.get() == INVALID_HANDLE_VALUE) {
        log_.error("Cannot create the helper pipe, error " + std::to_string(GetLastError()));
        WaitForSingleObject(stop_event_, kNoConsoleUserRetryMilliseconds);
        return;
    }

    const UniqueHandle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED overlapped{};
    overlapped.hEvent = io_event.get();
    DWORD ignored = 0;
    if (!ConnectNamedPipe(pipe.get(), &overlapped)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            if (wait_for(pipe.get(), overlapped, ignored, true) != Wait::Completed) {
                return;
            }
        } else if (error != ERROR_PIPE_CONNECTED) {
            log_.error("ConnectNamedPipe failed, error " + std::to_string(error));
            return;
        }
    }

    if (const ClientCheck check = check_pipe_client(pipe.get(), *console_sid); !check.accepted) {
        log_.warning("Refused pipe client: " + check.reason);
        DisconnectNamedPipe(pipe.get());
        return;
    }

    log_.info("Client connected");
    serve(pipe.get());
    handler_.reset_if_configured();
    DisconnectNamedPipe(pipe.get());
    log_.info("Client disconnected");
}

void PipeServer::serve(HANDLE pipe) {
    const UniqueHandle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::array<std::uint8_t, protocol::kMaxMessageSize> buffer{};

    while (true) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = io_event.get();
        DWORD received = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &received,
                      &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_MORE_DATA) {
                log_.warning("Rejected request: message larger than the protocol allows");
                return;
            }
            if (error != ERROR_IO_PENDING) {
                return; // client went away
            }
            const Wait result = wait_for(pipe, overlapped, received, true);
            if (result == Wait::SessionChanged) {
                log_.info("Console session changed; ending the client session");
                return;
            }
            if (result != Wait::Completed) {
                return;
            }
        }

        const protocol::Bytes response =
            handler_.handle(std::span<const std::uint8_t>(buffer.data(), received));

        OVERLAPPED write_overlapped{};
        write_overlapped.hEvent = io_event.get();
        DWORD written = 0;
        if (!WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written,
                       &write_overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING ||
                wait_for(pipe, write_overlapped, written, false) != Wait::Completed) {
                return;
            }
        }
    }
}

PipeServer::Wait PipeServer::wait_for(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred,
                                      bool watch_session) {
    const std::array<HANDLE, 3> handles{overlapped.hEvent, stop_event_, session_changed_event_};
    const DWORD count = watch_session ? 3 : 2;
    const DWORD signaled = WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE);

    if (signaled == WAIT_OBJECT_0) {
        if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
            return Wait::Completed;
        }
        if (GetLastError() == ERROR_MORE_DATA) {
            log_.warning("Rejected request: message larger than the protocol allows");
        }
        return Wait::Failed;
    }

    // Stop or session change: cancel the pending operation and wait for the cancellation, so the
    // OVERLAPPED structure is not released while the kernel still references it.
    CancelIoEx(pipe, &overlapped);
    GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
    if (signaled == WAIT_OBJECT_0 + 1) {
        return Wait::Stopped;
    }
    if (signaled == WAIT_OBJECT_0 + 2) {
        return Wait::SessionChanged;
    }
    return Wait::Failed;
}

} // namespace osplay::helper
