#pragma once

#include <chrono>
#include <filesystem>
#include <span>
#include <cstdint>

namespace llavon::ime::socket_io {

using Deadline = std::chrono::steady_clock::time_point;

// Nonblocking sockets with one absolute deadline for the entire operation,
// including fragmented frames. These never renew the deadline on progress.
int connect(const std::filesystem::path& path, Deadline deadline);
bool write_all(int fd, std::span<const std::uint8_t> bytes, Deadline deadline);
bool read_all(int fd, std::span<std::uint8_t> bytes, Deadline deadline);

}  // namespace llavon::ime::socket_io
