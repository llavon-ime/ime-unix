#include "protocol.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <type_traits>

namespace llavon::ime::protocol {
ProtocolError::~ProtocolError() = default;

namespace {

bool valid_utf8(const std::string& value);

[[noreturn]] void fail(const std::string& message) { throw ProtocolError(message); }
void u8(ByteVector& out, std::uint8_t value) { out.push_back(value); }
void u16(ByteVector& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void u32(ByteVector& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void u64(ByteVector& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
template <typename Array>
void id(ByteVector& out, const Array& value) { out.insert(out.end(), value.begin(), value.end()); }

std::uint32_t checked(std::size_t size, const char* field, std::uint32_t limit = kMaxFramePayloadBytes) {
    if (size > limit || size > std::numeric_limits<std::uint32_t>::max())
        fail(std::string("protocol field is too large: ") + field);
    return static_cast<std::uint32_t>(size);
}
void text8(ByteVector& out, const std::string& value, const char* field) {
    if (!valid_utf8(value)) fail(std::string("invalid UTF-8: ") + field);
    u32(out, checked(value.size(), field, kMaxUtf8StringBytes));
    out.insert(out.end(), value.begin(), value.end());
}
void text16(ByteVector& out, std::u16string_view value, const char* field, std::uint32_t limit) {
    if (!valid_utf16(value)) fail(std::string("invalid UTF-16: ") + field);
    u32(out, checked(value.size(), field, limit));
    for (const auto unit : value) u16(out, static_cast<std::uint16_t>(unit));
}
void scalar(ByteVector& out, char32_t value, const char* field) {
    if (!valid_scalar(value)) fail(std::string("invalid Unicode scalar: ") + field);
    u32(out, static_cast<std::uint32_t>(value));
}
void type(ByteVector& out, MessageType value) { u8(out, static_cast<std::uint8_t>(value)); }

ByteVector make_frame(ByteVector payload) {
    if (payload.size() > kMaxFramePayloadBytes) fail("protocol frame is too large");
    ByteVector result;
    result.reserve(4 + payload.size());
    u32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

class Reader {
public:
    explicit Reader(const ByteVector& frame) : bytes_(frame), end_(frame.size()) {
        const auto length = read_u32();
        if (length > kMaxFramePayloadBytes || frame.size() != 4U + static_cast<std::size_t>(length))
            fail("protocol frame length mismatch");
    }
    std::uint8_t read_u8() { require(1); return bytes_[offset_++]; }
    std::uint16_t read_u16() {
        require(2);
        const auto result = static_cast<std::uint16_t>(bytes_[offset_]) |
                            (static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8U);
        offset_ += 2;
        return static_cast<std::uint16_t>(result);
    }
    std::uint32_t read_u32() {
        require(4);
        std::uint32_t result = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) result |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        return result;
    }
    std::uint64_t read_u64() {
        require(8);
        std::uint64_t result = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) result |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        return result;
    }
    SessionId read_id() {
        require(16);
        SessionId result{};
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), 16, result.begin());
        offset_ += 16;
        return result;
    }
    std::size_t remaining() const noexcept { return offset_ <= end_ ? end_ - offset_ : 0; }
    std::string read_utf8(const char* field) {
        const auto size = read_u32();
        if (size > kMaxUtf8StringBytes) fail(std::string("UTF-8 field is too large: ") + field);
        require(size);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        if (!valid_utf8(result)) fail(std::string("invalid UTF-8: ") + field);
        return result;
    }
    std::u16string read_utf16(const char* field, std::uint32_t limit) {
        const auto count = read_u32();
        if (count > limit || static_cast<std::size_t>(count) > remaining() / 2U)
            fail(std::string("invalid UTF-16 length: ") + field);
        std::u16string result;
        result.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) result.push_back(static_cast<char16_t>(read_u16()));
        if (!valid_utf16(result)) fail(std::string("invalid UTF-16: ") + field);
        return result;
    }
    char32_t read_scalar(const char* field) {
        const auto result = static_cast<char32_t>(read_u32());
        if (!valid_scalar(result)) fail(std::string("invalid Unicode scalar: ") + field);
        return result;
    }
    void done() const { if (offset_ != end_) fail("protocol frame has trailing bytes"); }

private:
    void require(std::size_t size) const { if (size > remaining()) fail("truncated protocol frame"); }
    const ByteVector& bytes_;
    std::size_t offset_ = 0;
    std::size_t end_ = 0;
};

MessageType read_type(Reader& reader) {
    const auto raw = reader.read_u8();
    if (raw < 1 || raw > static_cast<std::uint8_t>(MessageType::DiscardCommit))
        fail("unknown protocol message type: " + std::to_string(raw));
    return static_cast<MessageType>(raw);
}
void count_ok(const Reader& reader, std::uint32_t count, std::size_t minimum, const char* field) {
    if (count > kMaxRepeatedFields || (minimum && static_cast<std::size_t>(count) > reader.remaining() / minimum))
        fail(std::string("too many protocol entries: ") + field);
}
void append_padding(ByteVector& out, const PaddingEntry& entry) {
    u8(out, entry.chosen() ? 1 : 0);
    if (entry.chosen()) {
        if (entry.chosen_char() == 0) fail("chosen padding has no character");
        scalar(out, entry.chosen_char(), "chosen character");
    } else text16(out, entry.bopomofo(), "bopomofo", kMaxBopomofoCodeUnits);
}
std::vector<PaddingEntry> read_padding(Reader& reader) {
    const auto count = reader.read_u32();
    count_ok(reader, count, 1, "padding");
    std::vector<PaddingEntry> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PaddingEntry entry;
        const auto kind = reader.read_u8();
        if (kind == 0) entry = PaddingEntry(reader.read_utf16("bopomofo", kMaxBopomofoCodeUnits));
        else if (kind == 1) entry = PaddingEntry(reader.read_scalar("chosen character"));
        else fail("unknown padding entry kind");
        if (entry.chosen() && entry.chosen_char() == 0) fail("chosen padding has no character");
        result.push_back(std::move(entry));
    }
    return result;
}
void append_candidates(ByteVector& out, const std::vector<std::vector<char32_t>>& lists) {
    u32(out, checked(lists.size(), "candidate segments", kMaxRepeatedFields));
    for (const auto& list : lists) {
        u32(out, checked(list.size(), "candidate list", kMaxRepeatedFields));
        for (const auto candidate : list) {
            if (candidate == 0) fail("candidate list contains zero");
            scalar(out, candidate, "candidate");
        }
    }
}
std::vector<std::vector<char32_t>> read_candidates(Reader& reader) {
    const auto segments = reader.read_u32();
    count_ok(reader, segments, 4, "candidate segments");
    std::vector<std::vector<char32_t>> result;
    result.reserve(segments);
    for (std::uint32_t i = 0; i < segments; ++i) {
        const auto count = reader.read_u32();
        count_ok(reader, count, 4, "candidate list");
        std::vector<char32_t> list;
        list.reserve(count);
        for (std::uint32_t j = 0; j < count; ++j) list.push_back(reader.read_scalar("candidate"));
        result.push_back(std::move(list));
    }
    return result;
}
void append_session_status(ByteVector& out, const SessionStatus& value) {
    id(out, value.session_id); u8(out, value.active); u8(out, value.closing); u8(out, value.engine_loaded); u64(out, value.last_request_id);
}
SessionStatus read_session_status(Reader& reader) {
    SessionStatus value;
    value.session_id = reader.read_id(); value.active = reader.read_u8() != 0; value.closing = reader.read_u8() != 0;
    value.engine_loaded = reader.read_u8() != 0; value.last_request_id = reader.read_u64(); return value;
}

bool valid_utf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        std::size_t length = 0; std::uint32_t cp = 0;
        if (first <= 0x7f) { ++i; continue; }
        if (first >= 0xc2 && first <= 0xdf) { length = 2; cp = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { length = 3; cp = first & 0xf; }
        else if (first >= 0xf0 && first <= 0xf4) { length = 4; cp = first & 7; }
        else return false;
        if (i + length > value.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (!valid_scalar(static_cast<char32_t>(cp)) || (length == 3 && cp < 0x800) ||
            (length == 4 && cp < 0x10000)) return false;
        i += length;
    }
    return true;
}

}  // namespace

bool valid_scalar(char32_t value) noexcept {
    const auto cp = static_cast<std::uint32_t>(value);
    return cp <= 0x10ffffU && !(cp >= 0xd800U && cp <= 0xdfffU);
}
bool valid_utf16(std::u16string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto unit = static_cast<std::uint16_t>(value[i]);
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (++i >= value.size()) return false;
            const auto low = static_cast<std::uint16_t>(value[i]);
            if (low < 0xdc00 || low > 0xdfff) return false;
        } else if (unit >= 0xdc00 && unit <= 0xdfff) return false;
    }
    return true;
}
bool is_zero(const SessionId& id) noexcept { return std::all_of(id.begin(), id.end(), [](auto value) { return value == 0; }); }

MessageType message_type(const Message& message) {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, OpenSessionRequest> || std::is_same_v<T, OpenSessionResponse>) return MessageType::OpenSession;
        else if constexpr (std::is_same_v<T, PredictRequest>) return MessageType::Predict;
        else if constexpr (std::is_same_v<T, Prediction>) return MessageType::Prediction;
        else if constexpr (std::is_same_v<T, CloseSessionRequest> || std::is_same_v<T, CloseSessionResponse>) return MessageType::CloseSession;
        else if constexpr (std::is_same_v<T, StatusRequest> || std::is_same_v<T, StatusResponse>) return MessageType::Status;
        else if constexpr (std::is_same_v<T, ShutdownRequest> || std::is_same_v<T, ShutdownResponse>) return MessageType::Shutdown;
        else if constexpr (std::is_same_v<T, RecordCommitRequest> || std::is_same_v<T, RecordCommitResponse>) return MessageType::RecordCommit;
        else if constexpr (std::is_same_v<T, DiscardCommitRequest> || std::is_same_v<T, DiscardCommitResponse>) return MessageType::DiscardCommit;
        else return MessageType::Error;
    }, message);
}

ByteVector encode(const Message& message) {
    ByteVector payload;
    std::visit([&payload](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, OpenSessionRequest>) { type(payload, MessageType::OpenSession); u8(payload, 0); }
        else if constexpr (std::is_same_v<T, OpenSessionResponse>) { type(payload, MessageType::OpenSession); u8(payload, 1); id(payload, value.session_id); id(payload, value.service_epoch); }
        else if constexpr (std::is_same_v<T, PredictRequest>) {
            if (is_zero(value.session_id) || value.request_id == 0) fail("predict correlation is invalid");
            type(payload, MessageType::Predict); id(payload, value.session_id); u64(payload, value.request_id); u64(payload, value.buffer_revision);
            text16(payload, value.context, "context", kMaxContextCodeUnits); u32(payload, checked(value.padding.size(), "padding", kMaxRepeatedFields));
            for (const auto& entry : value.padding) append_padding(payload, entry);
        } else if constexpr (std::is_same_v<T, Prediction>) {
            if (is_zero(value.session_id) || value.request_id == 0) fail("prediction correlation is invalid");
            type(payload, MessageType::Prediction); id(payload, value.session_id); u64(payload, value.request_id); u64(payload, value.buffer_revision); append_candidates(payload, value.candidates);
        } else if constexpr (std::is_same_v<T, CloseSessionRequest>) { if (is_zero(value.session_id)) fail("close has no session id"); type(payload, MessageType::CloseSession); u8(payload, 0); id(payload, value.session_id); }
        else if constexpr (std::is_same_v<T, CloseSessionResponse>) { if (is_zero(value.session_id)) fail("close response has no session id"); type(payload, MessageType::CloseSession); u8(payload, 1); id(payload, value.session_id); u8(payload, value.closed); }
        else if constexpr (std::is_same_v<T, StatusRequest>) { type(payload, MessageType::Status); u8(payload, 0); u8(payload, value.session_id.has_value()); if (value.session_id) id(payload, *value.session_id); }
        else if constexpr (std::is_same_v<T, StatusResponse>) { type(payload, MessageType::Status); u8(payload, 1); id(payload, value.service_epoch); u8(payload, value.shutting_down); u8(payload, value.model_loaded); u32(payload, value.active_sessions); u32(payload, value.max_sessions); u8(payload, value.session.has_value()); if (value.session) append_session_status(payload, *value.session); }
        else if constexpr (std::is_same_v<T, ShutdownRequest>) { type(payload, MessageType::Shutdown); u8(payload, 0); }
        else if constexpr (std::is_same_v<T, ShutdownResponse>) { type(payload, MessageType::Shutdown); u8(payload, 1); u8(payload, value.accepted); }
        else if constexpr (std::is_same_v<T, RecordCommitRequest>) {
            if (is_zero(value.event_id) || is_zero(value.source_id) || value.entries.empty() || value.entries.size() > 1024 ||
                value.answer.empty()) fail("invalid commit sample");
            type(payload, MessageType::RecordCommit); u8(payload, 0); id(payload, value.event_id); id(payload, value.source_id);
            text16(payload, value.context, "commit context", 4096);
            text16(payload, value.answer, "commit answer", 1024);
            u32(payload, checked(value.entries.size(), "commit entries", 1024));
            for (const auto& entry : value.entries) {
                if (entry.literal != entry.reading.empty()) fail("commit literal reading mismatch");
                text16(payload, entry.reading, "commit reading", 64);
                if (entry.character == 0) fail("empty commit character");
                scalar(payload, entry.character, "commit character"); u8(payload, entry.manually_selected);
                u8(payload, entry.literal);
            }
        }
        else if constexpr (std::is_same_v<T, RecordCommitResponse>) {
            type(payload, MessageType::RecordCommit); u8(payload, 1); id(payload, value.event_id); u8(payload, value.stored);
        }
        else if constexpr (std::is_same_v<T, DiscardCommitRequest>) {
            if (is_zero(value.event_id)) fail("discard has no event id");
            type(payload, MessageType::DiscardCommit); u8(payload, 0); id(payload, value.event_id);
        }
        else if constexpr (std::is_same_v<T, DiscardCommitResponse>) {
            if (is_zero(value.event_id)) fail("discard response has no event id");
            type(payload, MessageType::DiscardCommit); u8(payload, 1); id(payload, value.event_id); u8(payload, value.discarded);
        }
        else if constexpr (std::is_same_v<T, Error>) { const auto raw = static_cast<std::uint8_t>(value.code); if (raw < 1 || raw > 9) fail("unknown error code"); type(payload, MessageType::Error); u8(payload, raw); id(payload, value.session_id); u64(payload, value.request_id); u64(payload, value.buffer_revision); text8(payload, value.message, "error message"); }
    }, message);
    return make_frame(std::move(payload));
}

Message decode(const ByteVector& frame) {
    Reader reader(frame);
    switch (read_type(reader)) {
        case MessageType::OpenSession: { const auto kind = reader.read_u8(); if (kind == 0) { reader.done(); return OpenSessionRequest{}; } if (kind == 1) { OpenSessionResponse value; value.session_id = reader.read_id(); value.service_epoch = reader.read_id(); reader.done(); return value; } fail("unknown OpenSession payload kind"); }
        case MessageType::Predict: { PredictRequest value; value.session_id = reader.read_id(); value.request_id = reader.read_u64(); value.buffer_revision = reader.read_u64(); if (value.request_id == 0) fail("predict has no request id"); value.context = reader.read_utf16("context", kMaxContextCodeUnits); value.padding = read_padding(reader); reader.done(); return value; }
        case MessageType::Prediction: { Prediction value; value.session_id = reader.read_id(); value.request_id = reader.read_u64(); value.buffer_revision = reader.read_u64(); if (value.request_id == 0) fail("prediction has no request id"); value.candidates = read_candidates(reader); reader.done(); return value; }
        case MessageType::CloseSession: { const auto kind = reader.read_u8(); if (kind == 0) { CloseSessionRequest value{reader.read_id()}; if (is_zero(value.session_id)) fail("close has no session id"); reader.done(); return value; } if (kind == 1) { CloseSessionResponse value; value.session_id = reader.read_id(); value.closed = reader.read_u8() != 0; if (is_zero(value.session_id)) fail("close response has no session id"); reader.done(); return value; } fail("unknown CloseSession payload kind"); }
        case MessageType::Status: { const auto kind = reader.read_u8(); if (kind == 0) { StatusRequest value; if (reader.read_u8() != 0) value.session_id = reader.read_id(); reader.done(); return value; } if (kind == 1) { StatusResponse value; value.service_epoch = reader.read_id(); value.shutting_down = reader.read_u8() != 0; value.model_loaded = reader.read_u8() != 0; value.active_sessions = reader.read_u32(); value.max_sessions = reader.read_u32(); if (reader.read_u8() != 0) value.session = read_session_status(reader); reader.done(); return value; } fail("unknown Status payload kind"); }
        case MessageType::Shutdown: { const auto kind = reader.read_u8(); if (kind == 0) { reader.done(); return ShutdownRequest{}; } if (kind == 1) { ShutdownResponse value{reader.read_u8() != 0}; reader.done(); return value; } fail("unknown Shutdown payload kind"); }
        case MessageType::RecordCommit: {
            const auto kind = reader.read_u8();
            if (kind == 0) {
                RecordCommitRequest value; value.event_id = reader.read_id(); value.source_id = reader.read_id();
                if (is_zero(value.event_id) || is_zero(value.source_id)) fail("commit has no event or source id");
                value.context = reader.read_utf16("commit context", 4096);
                value.answer = reader.read_utf16("commit answer", 1024);
                const auto count = reader.read_u32(); count_ok(reader, count, 10, "commit entries");
                if (count == 0 || count > 1024 || value.answer.empty()) fail("invalid commit sample");
                for (std::uint32_t i = 0; i < count; ++i) {
                    CommitEntry entry; entry.reading = reader.read_utf16("commit reading", 64);
                    entry.character = reader.read_scalar("commit character");
                    const auto manual = reader.read_u8();
                    const auto literal = reader.read_u8();
                    if (entry.character == 0 || manual > 1 || literal > 1) fail("invalid commit entry");
                    entry.manually_selected = manual != 0;
                    entry.literal = literal != 0;
                    if (entry.literal != entry.reading.empty()) fail("invalid commit literal");
                    value.entries.push_back(std::move(entry));
                }
                reader.done(); return value;
            }
            if (kind == 1) {
                RecordCommitResponse value{reader.read_id(), reader.read_u8() != 0}; reader.done(); return value;
            }
            fail("unknown RecordCommit payload kind");
        }
        case MessageType::DiscardCommit: {
            const auto kind = reader.read_u8();
            if (kind == 0) {
                DiscardCommitRequest value{reader.read_id()};
                if (is_zero(value.event_id)) fail("discard has no event id");
                reader.done(); return value;
            }
            if (kind == 1) {
                DiscardCommitResponse value{reader.read_id(), reader.read_u8() != 0};
                if (is_zero(value.event_id)) fail("discard response has no event id");
                reader.done(); return value;
            }
            fail("unknown DiscardCommit payload kind");
        }
        case MessageType::Error: { const auto raw = reader.read_u8(); if (raw < 1 || raw > 9) fail("unknown protocol error code"); Error value; value.code = static_cast<ErrorCode>(raw); value.session_id = reader.read_id(); value.request_id = reader.read_u64(); value.buffer_revision = reader.read_u64(); value.message = reader.read_utf8("error message"); reader.done(); return value; }
    }
    fail("unreachable protocol message type");
}
MessageType decode_message_type(const ByteVector& frame) { Reader reader(frame); return read_type(reader); }
const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::UnknownSession: return "UNKNOWN_SESSION";
        case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
        case ErrorCode::ProtocolError: return "PROTOCOL_ERROR";
        case ErrorCode::Unauthorized: return "UNAUTHORIZED";
        case ErrorCode::SessionBusy: return "SESSION_BUSY";
        case ErrorCode::OutOfOrder: return "OUT_OF_ORDER";
        case ErrorCode::ResourceExhausted: return "RESOURCE_EXHAUSTED";
        case ErrorCode::ModelError: return "MODEL_ERROR";
        case ErrorCode::ServiceShuttingDown: return "SERVICE_SHUTTING_DOWN";
    }
    return "UNKNOWN";
}

}  // namespace llavon::ime::protocol
