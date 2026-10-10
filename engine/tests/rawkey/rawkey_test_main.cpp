#include "raw_key_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

struct Suite {
    std::string name;
    void (*body)();
};

std::vector<Suite>& registry() {
    static std::vector<Suite> suites;
    return suites;
}

}  // namespace

namespace llavon::ime::rawkey {

SuiteRegistrar::SuiteRegistrar(const char* name, void (*body)()) {
    registry().push_back(Suite{name, body});
}

}  // namespace llavon::ime::rawkey

// The standard raw-key runner: every registered suite drives raw keys through
// the harness and reports one line; a failure never stops the other suites.
int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--memory-client") {
        return llavon::ime::rawkey::memory_client_main(argv[2], std::atoi(argv[3]),
                                                     std::atoi(argv[4]));
    }
    if (argc == 5 && std::string_view(argv[1]) == "--committed-client") {
        return llavon::ime::rawkey::committed_client_main(argv[2], std::atoi(argv[3]),
                                                          std::atoi(argv[4]));
    }
    // Keep the suites away from the developer's real configuration.
    const auto config_home = std::filesystem::temp_directory_path() /
                             ("llavon-ime-rawkey-config-" + std::to_string(::getpid()));
    std::filesystem::remove_all(config_home);
    std::filesystem::create_directories(config_home);
    ::setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);

    int failures = 0;
    std::size_t selected = 0;
    for (const auto& suite : registry()) {
        if (argc > 1 && suite.name.find(argv[1]) == std::string::npos) continue;
        ++selected;
        try {
            suite.body();
            std::printf("[ok] %s\n", suite.name.c_str());
        } catch (const llavon::ime::rawkey::Failure& failure) {
            ++failures;
            std::printf("[FAIL] %s: %s\n", suite.name.c_str(), failure.message.c_str());
        } catch (const std::exception& error) {
            ++failures;
            std::printf("[FAIL] %s: %s\n", suite.name.c_str(), error.what());
        }
    }

    std::error_code error;
    std::filesystem::remove_all(config_home, error);

    if (selected == 0) {
        std::printf("no raw-key suites registered\n");
        return EXIT_FAILURE;
    }
    std::printf("raw-key tests: %zu suite(s), %d failure(s)\n", selected, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
