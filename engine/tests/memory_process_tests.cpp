#include "memory/process_memory.hpp"
#include "text/utf.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
using namespace llavon::ime;
namespace {
constexpr std::u16string_view document = u"獨立程序😀前文與後文";
constexpr std::size_t cursor = 9;
void check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
void transfer(int fd, void* bytes, std::size_t size, bool writing) {
    auto* data = static_cast<char*>(bytes);
    while (size) {
        const auto count = writing ? ::write(fd, data, size) : ::read(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        check(count > 0, "pipe transfer failed");
        data += count;
        size -= static_cast<std::size_t>(count);
    }
}
class Child {
public:
    explicit Child(std::string mode) {
        int response[2], command[2];
        check(::pipe(response) == 0, "response pipe");
        if (::pipe(command) != 0) { ::close(response[0]); ::close(response[1]); throw std::runtime_error("command pipe"); }
        const auto input = std::to_string(command[0]);
        const auto output = std::to_string(response[1]);
        std::array<char*, 6> args{const_cast<char*>("llavon_ime_memory_tests"), mode.data(),
                                 const_cast<char*>(input.c_str()), const_cast<char*>(output.c_str()), nullptr};
        const int result = ::posix_spawn(&pid_, "/proc/self/exe", nullptr, nullptr, args.data(), environ);
        ::close(command[0]);
        ::close(response[1]);
        input_ = response[0];
        output_ = command[1];
        if (result) { ::close(input_); ::close(output_); throw std::system_error(result, std::generic_category()); }
        marker.resize(128);
        transfer(input_, marker.data(), marker.size() * sizeof(char16_t), false);
    }
    ~Child() {
        // Terminate only the child explicitly created by this test.
        ::kill(pid_, SIGTERM);
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        ::close(input_);
        ::close(output_);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    pid_t pid() const { return pid_; }
    void deny_read() {
        char command = 'd';
        transfer(output_, &command, 1, true);
        transfer(input_, &command, 1, false);
        check(command == 'd', "child dumpable acknowledgement");
    }
    std::u16string marker;
private:
    pid_t pid_ = -1;
    int input_ = -1;
    int output_ = -1;
};

int run_child(std::string_view mode, int input, int output) {
    const auto marker = memory::make_marker();
    std::u16string undo(document);
    undo.insert(cursor, marker);
    std::string encoded;
    if (mode == "--utf8") encoded = u16_to_utf8(undo);
    else {
        for (const auto unit : undo) {
            encoded.push_back(static_cast<char>(unit & 255));
            encoded.push_back(static_cast<char>(unit >> 8));
        }
    }
    // Remove the visible marker, retaining an undo copy. Clear the temporary
    // UTF-16 marked allocation for the UTF-8 case; only encoded is required.
    undo.assign(undo.size(), u'!');
    std::u16string visible(document);
    transfer(output, const_cast<char16_t*>(marker.data()), marker.size() * sizeof(char16_t), true);
    for (;;) {
        char command;
        transfer(input, &command, 1, false);
        if (command == 'd') {
            check(::prctl(PR_SET_DUMPABLE, 0) == 0, "prctl");
            transfer(output, &command, 1, true);
        }
        // Keep both live allocations observable across the pipe calls.
        check(!encoded.empty() && visible == document, "document changed");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 4) return run_child(argv[1], std::stoi(argv[2]), std::stoi(argv[3]));
        check(::getuid() != 0, "run memory tests as an ordinary user");
        for (const auto mode : {"--utf8", "--utf16"}) {
            Child child(mode);
            auto discovered = memory::discover("llavon_ime_memory_tests", {});
            std::vector<memory::Process> target;
            for (const auto process : discovered.processes) if (process.pid == child.pid()) target.push_back(process);
            check(target.size() == 1, "could not discover readable child");
            const auto capture = memory::capture(target, document, cursor, child.marker, {});
            check(capture.prefix == document.substr(0, cursor), "residue prefix mismatch");
            check(capture.bytes_read > 0 && capture.status == "ready", "no actual process read");
            auto reused = target;
            ++reused[0].start_time;
            check(!memory::capture(reused, document, cursor, child.marker, {}).prefix, "accepted reused PID");
            check(!memory::capture(target, u"不同文件", 2, child.marker, {}).prefix, "accepted unrelated marker adjacency");
            std::stop_source cancelled;
            cancelled.request_stop();
            check(!memory::capture(target, document, cursor, child.marker, cancelled.get_token()).prefix, "ignored cancellation");
            child.deny_read();
            check(!memory::capture(target, document, cursor, child.marker, {}).prefix, "read inaccessible child");
            std::cout << "[ok] " << mode << " residue, PID identity, full-window match, cancellation, permission denial\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
