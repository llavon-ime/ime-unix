#pragma once
#include "llavon_protocol/padding_entry.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace llavon::ime::protocol {

inline constexpr std::uint32_t kMaxFramePayloadBytes = 1024U * 1024U;
inline constexpr std::uint32_t kMaxRepeatedFields = 65536U;
inline constexpr std::uint32_t kMaxContextCodeUnits = 65536U;
inline constexpr std::uint32_t kMaxBopomofoCodeUnits = 4096U;
inline constexpr std::uint32_t kMaxUtf8StringBytes = 256U * 1024U;

using ByteVector = std::vector<std::uint8_t>;
using SessionId = std::array<std::uint8_t, 16>;
using ServiceEpoch = std::array<std::uint8_t, 16>;

enum class MessageType : std::uint8_t {
    OpenSession = 1,
    Predict = 2,
    Prediction = 3,
    CloseSession = 4,
    Status = 5,
    Shutdown = 6,
    Error = 7,
    RecordCommit = 8,
    DiscardCommit = 9,
};

enum class ErrorCode : std::uint8_t {
    UnknownSession = 1,
    InvalidArgument = 2,
    ProtocolError = 3,
    Unauthorized = 4,
    SessionBusy = 5,
    OutOfOrder = 6,
    ResourceExhausted = 7,
    ModelError = 8,
    ServiceShuttingDown = 9,
};

struct CommitEntry {
    std::u16string reading;
    char32_t character = 0;
    bool manually_selected = false;
    // A literal (non-Bopomofo) position typed as-is: it carries no reading and
    // only provides context, never a training target.
    bool literal = false;
};
struct RecordCommitRequest {
    SessionId event_id{};
    // Stable for one input context, even across prediction-service reconnects.
    SessionId source_id{};
    std::u16string context;
    std::u16string answer;
    std::vector<CommitEntry> entries;
};
struct RecordCommitResponse { SessionId event_id{}; bool stored = false; };
// A commit that the user immediately corrected with Backspace; the service
// drops the staged commit instead of writing it.
struct DiscardCommitRequest { SessionId event_id{}; };
struct DiscardCommitResponse { SessionId event_id{}; bool discarded = false; };
struct OpenSessionRequest {};
struct OpenSessionResponse { SessionId session_id{}; ServiceEpoch service_epoch{}; };
struct PredictRequest {
    SessionId session_id{};
    std::uint64_t request_id = 0;
    std::uint64_t buffer_revision = 0;
    std::u16string context;
    std::vector<PaddingEntry> padding;
};
struct Prediction {
    SessionId session_id{};
    std::uint64_t request_id = 0;
    std::uint64_t buffer_revision = 0;
    std::vector<std::vector<char32_t>> candidates;
};
struct CloseSessionRequest { SessionId session_id{}; };
struct CloseSessionResponse { SessionId session_id{}; bool closed = false; };
struct StatusRequest { std::optional<SessionId> session_id; };
struct SessionStatus {
    SessionId session_id{};
    bool active = false;
    bool closing = false;
    bool engine_loaded = false;
    std::uint64_t last_request_id = 0;
};
struct StatusResponse {
    ServiceEpoch service_epoch{};
    bool shutting_down = false;
    bool model_loaded = false;
    std::uint32_t active_sessions = 0;
    std::uint32_t max_sessions = 0;
    std::optional<SessionStatus> session;
};
struct ShutdownRequest {};
struct ShutdownResponse { bool accepted = false; };
struct Error {
    ErrorCode code = ErrorCode::ProtocolError;
    SessionId session_id{};
    std::uint64_t request_id = 0;
    std::uint64_t buffer_revision = 0;
    std::string message;
};

using Message = std::variant<OpenSessionRequest, OpenSessionResponse, PredictRequest, Prediction,
                             CloseSessionRequest, CloseSessionResponse, StatusRequest, StatusResponse,
                             ShutdownRequest, ShutdownResponse, Error, RecordCommitRequest, RecordCommitResponse,
                             DiscardCommitRequest, DiscardCommitResponse>;

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& message) : std::runtime_error(message) {}
    ~ProtocolError() override;
};

ByteVector encode(const Message& message);
Message decode(const ByteVector& frame);
MessageType message_type(const Message& message);
MessageType decode_message_type(const ByteVector& frame);
const char* error_code_name(ErrorCode code) noexcept;
bool valid_scalar(char32_t value) noexcept;
bool valid_utf16(std::u16string_view value) noexcept;
bool is_zero(const SessionId& id) noexcept;

}  // namespace llavon::ime::protocol
