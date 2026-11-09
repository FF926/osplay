#include "pipe_helper_channel.hpp"

#include <osplay/win/helper_protocol.hpp>

namespace osplay::engine {
namespace {

constexpr DWORD kBusyWaitMilliseconds = 2000;
constexpr DWORD kInstanceRecreationMilliseconds = 100;
constexpr int kConnectAttempts = 30; // up to ~3 s while the helper recovers between clients

DisplayError error_for_open_failure(DWORD error) {
    switch (error) {
    case ERROR_ACCESS_DENIED:
        return DisplayError::HelperAccessDenied;
    case ERROR_PIPE_BUSY:
    case ERROR_SEM_TIMEOUT: // WaitNamedPipeW gave up: another client still holds the only instance
        return DisplayError::HelperBusy;
    default:
        return DisplayError::HelperUnavailable;
    }
}

} // namespace

win::Expected<std::unique_ptr<PipeHelperChannel>, DisplayError> PipeHelperChannel::connect() {
    using Result = win::Expected<std::unique_ptr<PipeHelperChannel>, DisplayError>;

    // The helper serves one client at a time; a busy pipe gets bounded waits, because the other
    // client may only be closing. A session in progress keeps it busy for much longer. Between
    // clients the helper has no listening instance for a moment (it recreates one as soon as a
    // client leaves), so a connect right after another client's disconnect — the role swap's
    // health check and the new host session are such a pair — lands in that gap and reports
    // ERROR_FILE_NOT_FOUND. Retry until the instance is back instead of giving up (#41).
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD last_error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kConnectAttempts && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        // SECURITY_IDENTIFICATION lets the helper check who we are without acting as us.
        pipe = CreateFileW(win::helper::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        last_error = GetLastError();
        if (last_error == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(win::helper::kPipeName, kBusyWaitMilliseconds)) {
                last_error = GetLastError();
                break;
            }
        } else if (last_error == ERROR_FILE_NOT_FOUND && attempt + 1 < kConnectAttempts) {
            Sleep(kInstanceRecreationMilliseconds);
        } else {
            break;
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        return Result::failure(error_for_open_failure(last_error));
    }

    auto channel = std::make_unique<PipeHelperChannel>(ConstructionToken{}, pipe);

    ULONG server_session = 1;
    if (!GetNamedPipeServerSessionId(pipe, &server_session) || server_session != 0) {
        return Result::failure(DisplayError::HelperNotTrusted);
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        return Result::failure(DisplayError::HelperUnavailable);
    }
    return channel;
}

PipeHelperChannel::~PipeHelperChannel() {
    CloseHandle(pipe_);
}

win::Expected<win::helper::Bytes, DisplayError>
PipeHelperChannel::transact(std::span<const std::uint8_t> request) {
    using Result = win::Expected<win::helper::Bytes, DisplayError>;
    // TransactNamedPipe takes a non-const input buffer, so send a copy.
    win::helper::Bytes outgoing(request.begin(), request.end());
    win::helper::Bytes response(win::helper::kMaxMessageSize);
    DWORD received = 0;
    if (!TransactNamedPipe(pipe_, outgoing.data(), static_cast<DWORD>(outgoing.size()),
                           response.data(), static_cast<DWORD>(response.size()), &received,
                           nullptr)) {
        const DWORD error = GetLastError();
        return Result::failure(error == ERROR_MORE_DATA ? DisplayError::ProtocolMismatch
                                                        : DisplayError::ConnectionLost);
    }
    response.resize(received);
    return response;
}

} // namespace osplay::engine
