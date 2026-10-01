#include "ipc/unix_socket.hpp"
#include "util/parse_number.hpp"
#include "util/unique_fd.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

int open_descriptors() {
    int count = 0;
    for (int fd = 0; fd < 4096; ++fd) {
        if (::fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

bool socket_exception_cleanup() {
    const auto path = std::filesystem::temp_directory_path() / std::string(180, 'x');
    const int before = open_descriptors();
    for (int attempt = 0; attempt < 8; ++attempt) {
        bool client_failed = false;
        try { (void)llavon::ime::UnixSocketClient{}.connect(path); }
        catch (const std::runtime_error&) { client_failed = true; }
        bool server_failed = false;
        try { llavon::ime::UnixSocketServer server; server.bind_listen(path); }
        catch (const std::runtime_error&) { server_failed = true; }
        if (!client_failed || !server_failed) return false;
    }
    return before == open_descriptors();
}

bool descriptor_ownership() {
    using llavon::ime::UniqueFd;
    static_assert(!std::is_copy_constructible_v<UniqueFd>);
    static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
    int borrowed = -1;
    try {
        UniqueFd owner(::open("/dev/null", O_RDONLY));
        if (!owner.valid()) return false;
        borrowed = owner.get();
        UniqueFd moved(std::move(owner));
        if (owner.valid() || moved.get() != borrowed) return false;
        UniqueFd assigned;
        assigned = std::move(moved);
        if (moved.valid() || assigned.get() != borrowed) return false;
        errno = EIO;
        assigned.reset(assigned.get());
        if (errno != EIO || !assigned.valid()) return false;
        throw std::runtime_error("unwind descriptor owner");
    } catch (const std::runtime_error&) {
        return ::fcntl(borrowed, F_GETFD) < 0 && errno == EBADF;
    }
}

bool decimal_compatibility() {
    const std::array<std::string_view, 22> samples{
        "", " ", "0", "01", "+12", "  +12", "\t-12", "12tail", "12 ", "0x12",
        "2147483647", "2147483648", "-2147483648", "-2147483649", "9999999999999999999999",
        "+-1", "++1", "--1", "+ 1", "-0", "\n42", "\v42"};
    for (const auto sample : samples) {
        for (const bool complete : {false, true}) {
            std::optional<int> legacy;
            try {
                std::size_t consumed = 0;
                const int number = std::stoi(std::string(sample), &consumed);
                if (!complete || consumed == sample.size()) legacy = number;
            } catch (const std::exception&) {}
            if (llavon::ime::parse_decimal<int>(sample, complete) != legacy) {
                std::cerr << "decimal grammar changed: " << sample << '\n';
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main() {
    if (!socket_exception_cleanup() || !descriptor_ownership() || !decimal_compatibility()) {
        std::cerr << "resource/compatibility regression\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
