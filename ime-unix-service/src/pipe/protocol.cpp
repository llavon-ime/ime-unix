#include "protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace ime::unix_service::protocol {

namespace {

bool valid_utf8(const std::string& value);

[[noreturn]] void fail(const char* message) {
    throw ProtocolError(message);
}

void append_u8(ByteVector& out, std::uint8_t value) {
    out.push_back(value);
}

void append_u16(ByteVector& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(ByteVector& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(ByteVector& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_id(ByteVector& out, const std::array<std::uint8_t, 16>& id) {
    out.insert(out.end(), id.begin(), id.end());
}

std::uint32_t checked_size(std::size_t size, const char* field, std::uint32_t limit = kMaxFramePayloadBytes) {
    if (size > limit || size > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError(std::string("protocol field is too large: ") + field);
    }
    return static_cast<std::uint32_t>(size);
}

void append_utf8(ByteVector& out, const std::string& value, const char* field) {
    if (!valid_utf8(value)) throw ProtocolError(std::string("invalid UTF-8: ") + field);
    const auto size = checked_size(value.size(), field, kMaxUtf8StringBytes);
    append_u32(out, size);
    out.insert(out.end(), value.begin(), value.end());
}

void append_utf16(ByteVector& out, std::u16string_view value, const char* field, std::uint32_t limit) {
    if (!valid_utf16(value)) throw ProtocolError(std::string("invalid UTF-16: ") + field);
    const auto count = checked_size(value.size(), field, limit);
    append_u32(out, count);
    for (const char16_t unit : value) append_u16(out, static_cast<std::uint16_t>(unit));
}

void append_scalar(ByteVector& out, char32_t value, const char* field) {
    if (!valid_scalar(value)) throw ProtocolError(std::string("invalid Unicode scalar: ") + field);
    append_u32(out, static_cast<std::uint32_t>(value));
}

void append_padding(ByteVector& out, const PaddingEntry& entry) {
    append_u8(out, entry.chosen() ? 1U : 0U);
    if (entry.chosen()) {
        if (entry.chosen_char() == 0) throw ProtocolError("chosen padding has no character");
        append_scalar(out, entry.chosen_char(), "chosen character");
    } else {
        append_utf16(out, entry.bopomofo(), "bopomofo", kMaxBopomofoCodeUnits);
    }
}

void append_candidates(ByteVector& out, const std::vector<std::vector<char32_t>>& candidates) {
    const auto segment_count = checked_size(candidates.size(), "candidate segments", kMaxRepeatedFields);
    append_u32(out, segment_count);
    for (const auto& list : candidates) {
        const auto candidate_count = checked_size(list.size(), "candidate list", kMaxRepeatedFields);
        append_u32(out, candidate_count);
        for (const auto candidate : list) {
            if (candidate == 0) throw ProtocolError("candidate list contains zero");
            append_scalar(out, candidate, "candidate");
        }
    }
}

void append_type(ByteVector& payload, MessageType type) {
    append_u8(payload, static_cast<std::uint8_t>(type));
}

ByteVector frame(ByteVector payload) {
    if (payload.size() > kMaxFramePayloadBytes) throw ProtocolError("protocol frame is too large");
    ByteVector result;
    result.reserve(sizeof(std::uint32_t) + payload.size());
    append_u32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

class Reader {
public:
    explicit Reader(const ByteVector& frame) : bytes_(frame) {
        payload_end_ = bytes_.size();
        const auto length = u32();
        if (length > kMaxFramePayloadBytes) fail("protocol frame is too large");
        if (bytes_.size() != sizeof(std::uint32_t) + static_cast<std::size_t>(length)) {
            fail("protocol frame length mismatch");
        }
    }

    std::uint8_t u8() {
        require(1);
        return bytes_[offset_++];
    }

    std::uint16_t u16() {
        require(2);
        const auto value = static_cast<std::uint16_t>(bytes_[offset_]) |
                           (static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8U);
        offset_ += 2;
        return value;
    }

    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        }
        return value;
    }

    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        }
        return value;
    }

    SessionId id() {
        require(16);
        SessionId result{};
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), result.size(), result.begin());
        offset_ += result.size();
        return result;
    }

    std::string utf8(const char* field) {
        const auto size = u32();
        if (size > kMaxUtf8StringBytes) throw ProtocolError(std::string("UTF-8 field is too large: ") + field);
        require(size);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        if (!valid_utf8(result)) throw ProtocolError(std::string("invalid UTF-8: ") + field);
        return result;
    }

    std::u16string utf16(const char* field, std::uint32_t limit) {
        const auto count = u32();
        if (count > limit) throw ProtocolError(std::string("UTF-16 field is too large: ") + field);
        if (count > remaining() / 2U) throw ProtocolError(std::string("truncated UTF-16 field: ") + field);
        std::u16string result;
        result.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) result.push_back(static_cast<char16_t>(u16()));
        if (!valid_utf16(result)) throw ProtocolError(std::string("invalid UTF-16: ") + field);
        return result;
    }

    char32_t scalar(const char* field) {
        const auto value = static_cast<char32_t>(u32());
        if (!valid_scalar(value)) throw ProtocolError(std::string("invalid Unicode scalar: ") + field);
        return value;
    }

    std::size_t remaining() const noexcept {
        return offset_ <= payload_end_ ? payload_end_ - offset_ : 0;
    }

    void require_done() const {
        if (offset_ != payload_end_) fail("protocol frame has trailing bytes");
    }

private:
    void require(std::size_t size) const {
        if (offset_ > payload_end_ || size > payload_end_ - offset_) fail("truncated protocol frame");
    }

    const ByteVector& bytes_;
    std::size_t offset_ = 0;
    std::size_t payload_end_ = 0;
};

MessageType read_type(Reader& reader) {
    const auto raw = reader.u8();
    if (raw < static_cast<std::uint8_t>(MessageType::OpenSession) ||
        raw > static_cast<std::uint8_t>(MessageType::DiscardCommit)) {
        throw ProtocolError("unknown protocol message type: " + std::to_string(raw));
    }
    return static_cast<MessageType>(raw);
}

void require_count(Reader& reader, std::uint32_t count, std::size_t minimum_bytes, const char* field) {
    if (count > kMaxRepeatedFields ||
        (minimum_bytes != 0 && static_cast<std::size_t>(count) > reader.remaining() / minimum_bytes)) {
        throw ProtocolError(std::string("too many protocol entries: ") + field);
    }
}

std::vector<PaddingEntry> read_padding(Reader& reader) {
    const auto count = reader.u32();
    require_count(reader, count, 1, "padding");
    std::vector<PaddingEntry> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PaddingEntry entry;
        const auto kind = reader.u8();
        if (kind == 0) {
            entry = PaddingEntry(reader.utf16("bopomofo", kMaxBopomofoCodeUnits));
        } else if (kind == 1) {
            entry = PaddingEntry(reader.scalar("chosen character"));
            if (entry.chosen_char() == 0) throw ProtocolError("chosen padding has no character");
        } else {
            throw ProtocolError("unknown padding entry kind");
        }
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<std::vector<char32_t>> read_candidates(Reader& reader) {
    const auto segments = reader.u32();
    require_count(reader, segments, 4, "candidate segments");
    std::vector<std::vector<char32_t>> result;
    result.reserve(segments);
    for (std::uint32_t i = 0; i < segments; ++i) {
        const auto count = reader.u32();
        require_count(reader, count, 4, "candidate list");
        std::vector<char32_t> candidates;
        candidates.reserve(count);
        for (std::uint32_t j = 0; j < count; ++j) candidates.push_back(reader.scalar("candidate"));
        result.push_back(std::move(candidates));
    }
    return result;
}

void append_session_status(ByteVector& payload, const SessionStatus& status) {
    append_id(payload, status.session_id);
    append_u8(payload, status.active ? 1U : 0U);
    append_u8(payload, status.closing ? 1U : 0U);
    append_u8(payload, status.engine_loaded ? 1U : 0U);
    append_u64(payload, status.last_request_id);
}

SessionStatus read_session_status(Reader& reader) {
    SessionStatus status;
    status.session_id = reader.id();
    status.active = reader.u8() != 0;
    status.closing = reader.u8() != 0;
    status.engine_loaded = reader.u8() != 0;
    status.last_request_id = reader.u64();
    return status;
}

bool valid_utf8(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const auto first = static_cast<unsigned char>(value[i]);
        std::uint32_t codepoint = 0;
        std::size_t length = 0;
        if (first <= 0x7fU) {
            ++i;
            continue;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            codepoint = first & 0x1fU;
            length = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            codepoint = first & 0x0fU;
            length = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            codepoint = first & 0x07U;
            length = 4;
        } else {
            return false;
        }
        if (i + length > value.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto continuation = static_cast<unsigned char>(value[i + j]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if (!valid_scalar(static_cast<char32_t>(codepoint))) return false;
        if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U)) return false;
        if (length == 3 && codepoint >= 0xd800U && codepoint <= 0xdfffU) return false;
        i += length;
    }
    return true;
}

}  // namespace

bool valid_scalar(char32_t value) noexcept {
    const auto raw = static_cast<std::uint32_t>(value);
    return raw <= 0x10ffffU && !(raw >= 0xd800U && raw <= 0xdfffU);
}

bool valid_utf16(std::u16string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto unit = static_cast<std::uint16_t>(value[i]);
        if (unit >= 0xd800U && unit <= 0xdbffU) {
            if (i + 1 >= value.size()) return false;
            const auto next = static_cast<std::uint16_t>(value[++i]);
            if (next < 0xdc00U || next > 0xdfffU) return false;
        } else if (unit >= 0xdc00U && unit <= 0xdfffU) {
            return false;
        }
    }
    return true;
}

bool is_zero(const SessionId& id) noexcept {
    return std::all_of(id.begin(), id.end(), [](std::uint8_t byte) { return byte == 0; });
}

MessageType message_type(const Message& message) {
    return std::visit(
        [](const auto& value) -> MessageType {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, OpenSessionRequest> || std::is_same_v<T, OpenSessionResponse>)
                return MessageType::OpenSession;
            else if constexpr (std::is_same_v<T, PredictRequest>)
                return MessageType::Predict;
            else if constexpr (std::is_same_v<T, Prediction>)
                return MessageType::Prediction;
            else if constexpr (std::is_same_v<T, CloseSessionRequest> || std::is_same_v<T, CloseSessionResponse>)
                return MessageType::CloseSession;
            else if constexpr (std::is_same_v<T, StatusRequest> || std::is_same_v<T, StatusResponse>)
                return MessageType::Status;
            else if constexpr (std::is_same_v<T, ShutdownRequest> || std::is_same_v<T, ShutdownResponse>)
                return MessageType::Shutdown;
            else if constexpr (std::is_same_v<T, RecordCommitRequest> || std::is_same_v<T, RecordCommitResponse>)
                return MessageType::RecordCommit;
            else if constexpr (std::is_same_v<T, DiscardCommitRequest> || std::is_same_v<T, DiscardCommitResponse>)
                return MessageType::DiscardCommit;
            else
                return MessageType::Error;
        },
        message);
}

ByteVector encode(const Message& message) {
    ByteVector payload;
    std::visit(
        [&payload](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, OpenSessionRequest>) {
                append_type(payload, MessageType::OpenSession);
                append_u8(payload, 0);
            } else if constexpr (std::is_same_v<T, OpenSessionResponse>) {
                append_type(payload, MessageType::OpenSession);
                append_u8(payload, 1);
                append_id(payload, value.session_id);
                append_id(payload, value.service_epoch);
            } else if constexpr (std::is_same_v<T, PredictRequest>) {
                if (is_zero(value.session_id)) throw ProtocolError("predict has no session id");
                if (value.request_id == 0) throw ProtocolError("predict has no request id");
                append_type(payload, MessageType::Predict);
                append_id(payload, value.session_id);
                append_u64(payload, value.request_id);
                append_u64(payload, value.buffer_revision);
                append_utf16(payload, value.context, "context", kMaxContextCodeUnits);
                const auto count = checked_size(value.padding.size(), "padding", kMaxRepeatedFields);
                append_u32(payload, count);
                for (const auto& entry : value.padding) append_padding(payload, entry);
            } else if constexpr (std::is_same_v<T, Prediction>) {
                if (is_zero(value.session_id)) throw ProtocolError("prediction has no session id");
                if (value.request_id == 0) throw ProtocolError("prediction has no request id");
                append_type(payload, MessageType::Prediction);
                append_id(payload, value.session_id);
                append_u64(payload, value.request_id);
                append_u64(payload, value.buffer_revision);
                append_candidates(payload, value.candidates);
            } else if constexpr (std::is_same_v<T, CloseSessionRequest>) {
                if (is_zero(value.session_id)) throw ProtocolError("close has no session id");
                append_type(payload, MessageType::CloseSession);
                append_u8(payload, 0);
                append_id(payload, value.session_id);
            } else if constexpr (std::is_same_v<T, CloseSessionResponse>) {
                if (is_zero(value.session_id)) throw ProtocolError("close response has no session id");
                append_type(payload, MessageType::CloseSession);
                append_u8(payload, 1);
                append_id(payload, value.session_id);
                append_u8(payload, value.closed ? 1U : 0U);
            } else if constexpr (std::is_same_v<T, StatusRequest>) {
                append_type(payload, MessageType::Status);
                append_u8(payload, 0);
                append_u8(payload, value.session_id ? 1U : 0U);
                if (value.session_id) append_id(payload, *value.session_id);
            } else if constexpr (std::is_same_v<T, StatusResponse>) {
                append_type(payload, MessageType::Status);
                append_u8(payload, 1);
                append_id(payload, value.service_epoch);
                append_u8(payload, value.shutting_down ? 1U : 0U);
                append_u8(payload, value.model_loaded ? 1U : 0U);
                append_u32(payload, value.active_sessions);
                append_u32(payload, value.max_sessions);
                append_u8(payload, value.session ? 1U : 0U);
                if (value.session) append_session_status(payload, *value.session);
            } else if constexpr (std::is_same_v<T, ShutdownRequest>) {
                append_type(payload, MessageType::Shutdown);
                append_u8(payload, 0);
            } else if constexpr (std::is_same_v<T, ShutdownResponse>) {
                append_type(payload, MessageType::Shutdown);
                append_u8(payload, 1);
                append_u8(payload, value.accepted ? 1U : 0U);
            } else if constexpr (std::is_same_v<T, RecordCommitRequest>) {
                if (is_zero(value.event_id) || is_zero(value.source_id) || value.entries.empty() || value.entries.size() > 1024 ||
                    value.answer.empty()) throw ProtocolError("invalid commit sample");
                append_type(payload, MessageType::RecordCommit); append_u8(payload, 0);
                append_id(payload, value.event_id);
                append_id(payload, value.source_id);
                append_utf16(payload, value.context, "commit context", 4096);
                append_utf16(payload, value.answer, "commit answer", 1024);
                append_u32(payload, checked_size(value.entries.size(), "commit entries", 1024));
                for (const auto& entry : value.entries) {
                    if (entry.literal != entry.reading.empty())
                        throw ProtocolError("commit literal reading mismatch");
                    append_utf16(payload, entry.reading, "commit reading", 64);
                    if (entry.character == 0) throw ProtocolError("empty commit character");
                    append_scalar(payload, entry.character, "commit character");
                    append_u8(payload, entry.manually_selected ? 1U : 0U);
                    append_u8(payload, entry.literal ? 1U : 0U);
                }
            } else if constexpr (std::is_same_v<T, RecordCommitResponse>) {
                append_type(payload, MessageType::RecordCommit); append_u8(payload, 1);
                append_id(payload, value.event_id); append_u8(payload, value.stored ? 1U : 0U);
            } else if constexpr (std::is_same_v<T, DiscardCommitRequest>) {
                if (is_zero(value.event_id)) throw ProtocolError("discard has no event id");
                append_type(payload, MessageType::DiscardCommit); append_u8(payload, 0);
                append_id(payload, value.event_id);
            } else if constexpr (std::is_same_v<T, DiscardCommitResponse>) {
                if (is_zero(value.event_id)) throw ProtocolError("discard response has no event id");
                append_type(payload, MessageType::DiscardCommit); append_u8(payload, 1);
                append_id(payload, value.event_id); append_u8(payload, value.discarded ? 1U : 0U);
            } else if constexpr (std::is_same_v<T, Error>) {
                if (static_cast<std::uint8_t>(value.code) < 1 || static_cast<std::uint8_t>(value.code) > 9)
                    throw ProtocolError("unknown error code");
                append_type(payload, MessageType::Error);
                append_u8(payload, static_cast<std::uint8_t>(value.code));
                append_id(payload, value.session_id);
                append_u64(payload, value.request_id);
                append_u64(payload, value.buffer_revision);
                append_utf8(payload, value.message, "error message");
            }
        },
        message);
    return frame(std::move(payload));
}

Message decode(const ByteVector& frame_bytes) {
    Reader reader(frame_bytes);
    const auto type = read_type(reader);
    switch (type) {
        case MessageType::OpenSession: {
            const auto kind = reader.u8();
            if (kind == 0) {
                reader.require_done();
                return OpenSessionRequest{};
            }
            if (kind == 1) {
                OpenSessionResponse response;
                response.session_id = reader.id();
                response.service_epoch = reader.id();
                reader.require_done();
                return response;
            }
            throw ProtocolError("unknown OpenSession payload kind");
        }
        case MessageType::Predict: {
            PredictRequest request;
            request.session_id = reader.id();
            request.request_id = reader.u64();
            request.buffer_revision = reader.u64();
            if (request.request_id == 0) throw ProtocolError("predict has no request id");
            request.context = reader.utf16("context", kMaxContextCodeUnits);
            request.padding = read_padding(reader);
            reader.require_done();
            return request;
        }
        case MessageType::Prediction: {
            Prediction response;
            response.session_id = reader.id();
            response.request_id = reader.u64();
            response.buffer_revision = reader.u64();
            if (response.request_id == 0) throw ProtocolError("prediction has no request id");
            response.candidates = read_candidates(reader);
            reader.require_done();
            return response;
        }
        case MessageType::CloseSession: {
            const auto kind = reader.u8();
            if (kind == 0) {
                CloseSessionRequest request{reader.id()};
                if (is_zero(request.session_id)) throw ProtocolError("close has no session id");
                reader.require_done();
                return request;
            }
            if (kind == 1) {
                CloseSessionResponse response;
                response.session_id = reader.id();
                response.closed = reader.u8() != 0;
                if (is_zero(response.session_id)) throw ProtocolError("close response has no session id");
                reader.require_done();
                return response;
            }
            throw ProtocolError("unknown CloseSession payload kind");
        }
        case MessageType::Status: {
            const auto kind = reader.u8();
            if (kind == 0) {
                StatusRequest request;
                if (reader.u8() != 0) request.session_id = reader.id();
                reader.require_done();
                return request;
            }
            if (kind == 1) {
                StatusResponse response;
                response.service_epoch = reader.id();
                response.shutting_down = reader.u8() != 0;
                response.model_loaded = reader.u8() != 0;
                response.active_sessions = reader.u32();
                response.max_sessions = reader.u32();
                if (reader.u8() != 0) response.session = read_session_status(reader);
                reader.require_done();
                return response;
            }
            throw ProtocolError("unknown Status payload kind");
        }
        case MessageType::Shutdown: {
            const auto kind = reader.u8();
            if (kind == 0) {
                reader.require_done();
                return ShutdownRequest{};
            }
            if (kind == 1) {
                ShutdownResponse response{reader.u8() != 0};
                reader.require_done();
                return response;
            }
            throw ProtocolError("unknown Shutdown payload kind");
        }
        case MessageType::RecordCommit: {
            const auto kind = reader.u8();
            if (kind == 0) {
                RecordCommitRequest request; request.event_id = reader.id(); request.source_id = reader.id();
                if (is_zero(request.event_id) || is_zero(request.source_id)) throw ProtocolError("commit has no event or source id");
                request.context = reader.utf16("commit context", 4096);
                request.answer = reader.utf16("commit answer", 1024);
                const auto count = reader.u32();
                if (count == 0 || count > 1024 || request.answer.empty())
                    throw ProtocolError("invalid commit sample");
                for (std::uint32_t i = 0; i < count; ++i) {
                    CommitEntry entry; entry.reading = reader.utf16("commit reading", 64);
                    entry.character = reader.scalar("commit character");
                    const auto manual = reader.u8();
                    const auto literal = reader.u8();
                    if (entry.character == 0 || manual > 1 || literal > 1)
                        throw ProtocolError("invalid commit entry");
                    entry.manually_selected = manual != 0;
                    entry.literal = literal != 0;
                    if (entry.literal != entry.reading.empty())
                        throw ProtocolError("invalid commit literal");
                    request.entries.push_back(std::move(entry));
                }
                reader.require_done(); return request;
            }
            if (kind == 1) {
                RecordCommitResponse response{reader.id(), reader.u8() != 0};
                reader.require_done(); return response;
            }
            throw ProtocolError("unknown RecordCommit payload kind");
        }
        case MessageType::DiscardCommit: {
            const auto kind = reader.u8();
            if (kind == 0) {
                DiscardCommitRequest request{reader.id()};
                if (is_zero(request.event_id)) throw ProtocolError("discard has no event id");
                reader.require_done(); return request;
            }
            if (kind == 1) {
                DiscardCommitResponse response{reader.id(), reader.u8() != 0};
                if (is_zero(response.event_id)) throw ProtocolError("discard response has no event id");
                reader.require_done(); return response;
            }
            throw ProtocolError("unknown DiscardCommit payload kind");
        }
        case MessageType::Error: {
            const auto raw_code = reader.u8();
            if (raw_code < 1 || raw_code > 9) throw ProtocolError("unknown protocol error code");
            Error error;
            error.code = static_cast<ErrorCode>(raw_code);
            error.session_id = reader.id();
            error.request_id = reader.u64();
            error.buffer_revision = reader.u64();
            error.message = reader.utf8("error message");
            reader.require_done();
            return error;
        }
    }
    throw ProtocolError("unreachable protocol message type");
}

MessageType decode_message_type(const ByteVector& frame_bytes) {
    Reader reader(frame_bytes);
    return read_type(reader);
}

const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::UnknownSession:
            return "UNKNOWN_SESSION";
        case ErrorCode::InvalidArgument:
            return "INVALID_ARGUMENT";
        case ErrorCode::ProtocolError:
            return "PROTOCOL_ERROR";
        case ErrorCode::Unauthorized:
            return "UNAUTHORIZED";
        case ErrorCode::SessionBusy:
            return "SESSION_BUSY";
        case ErrorCode::OutOfOrder:
            return "OUT_OF_ORDER";
        case ErrorCode::ResourceExhausted:
            return "RESOURCE_EXHAUSTED";
        case ErrorCode::ModelError:
            return "MODEL_ERROR";
        case ErrorCode::ServiceShuttingDown:
            return "SERVICE_SHUTTING_DOWN";
    }
    return "UNKNOWN";
}

}  // namespace ime::unix_service::protocol
