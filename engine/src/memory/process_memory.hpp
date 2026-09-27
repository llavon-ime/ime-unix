#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace llavon::ime::memory {

struct Process { pid_t pid = 0; std::uint64_t start_time = 0; };
struct Discovery { std::vector<Process> processes; std::string status; };
struct Capture { std::optional<std::u16string> prefix; std::string status; std::size_t bytes_read = 0; };

std::u16string make_marker();
Discovery discover(std::string_view program, std::stop_token cancel);
Capture capture(const std::vector<Process>& processes, std::u16string_view baseline,
                std::size_t cursor, std::u16string_view marker, std::stop_token cancel);

} // namespace llavon::ime::memory
