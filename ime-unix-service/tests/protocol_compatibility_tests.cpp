#include "pipe/protocol.hpp"
#include "protocol/protocol.hpp"

#include <cstdlib>
#include <iostream>

namespace client = llavon::ime::protocol;
namespace server = ime::unix_service::protocol;

int main() {
    client::PredictRequest request;
    for (std::size_t i = 0; i < request.session_id.size(); ++i)
        request.session_id[i] = static_cast<std::uint8_t>(i + 1);
    request.request_id = 0x0102030405060708ULL;
    request.buffer_revision = 0x1112131415161718ULL;
    request.context = u"你";
    request.padding = {client::PaddingEntry(U'好'), client::PaddingEntry(std::u16string(u"ㄋㄧˇ"))};
    // Frozen pre-refactor wire format: both tag-1 and tag-0 padding positions.
    const client::ByteVector golden{
        59, 0, 0, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        8, 7, 6, 5, 4, 3, 2, 1, 24, 23, 22, 21, 20, 19, 18, 17, 1, 0, 0, 0,
        96, 79, 2, 0, 0, 0, 1, 125, 89, 0, 0, 0, 3, 0, 0, 0, 11, 49, 39, 49, 199, 2};
    if (client::encode(request) != golden) return EXIT_FAILURE;
    const auto decoded = std::get<server::PredictRequest>(server::decode(golden));
    if (decoded.context != request.context || decoded.request_id != request.request_id ||
        decoded.buffer_revision != request.buffer_revision || decoded.padding.size() != 2 ||
        !decoded.padding[0].chosen() || decoded.padding[0].chosen_char() != U'好' ||
        decoded.padding[1].chosen() || decoded.padding[1].bopomofo() != u"ㄋㄧˇ" ||
        server::encode(decoded) != golden) return EXIT_FAILURE;
    const auto returned = std::get<client::PredictRequest>(client::decode(server::encode(decoded)));
    if (returned.session_id != request.session_id) return EXIT_FAILURE;
    for (const char32_t invalid : {char32_t{0}, char32_t{0xd800}, char32_t{0x110000}}) {
        request.padding = {client::PaddingEntry(invalid)};
        auto bad = decoded;
        bad.padding = request.padding;
        bool client_rejected = false, server_rejected = false;
        try { (void)client::encode(request); } catch (const client::ProtocolError&) { client_rejected = true; }
        try { (void)server::encode(bad); } catch (const server::ProtocolError&) { server_rejected = true; }
        if (!client_rejected || !server_rejected) return EXIT_FAILURE;
    }
    std::cout << "client/service golden wire compatibility passed\n";
    return EXIT_SUCCESS;
}
