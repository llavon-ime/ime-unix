// Optional Linux desktop E2E helper. Build with pkg-config atspi-2; this is not
// linked into the manager. Queries the actual Fcitx/Qt accessibility trees.
#include <atspi/atspi.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::string command, name, argument;
int match = 0, wanted = 0;

bool visit(AtspiAccessible* object, int depth) {
    if (depth > 14) return false;
    auto* states = atspi_accessible_get_state_set(object);
    const bool showing = atspi_state_set_contains(states, ATSPI_STATE_SHOWING);
    g_object_unref(states);
    auto* rawName = atspi_accessible_get_name(object, nullptr);
    const std::string title = rawName ? rawName : ""; g_free(rawName);
    auto* rawRole = atspi_accessible_get_role_name(object, nullptr);
    const std::string role = rawRole ? rawRole : ""; g_free(rawRole);
    if (command == "dump" && showing && !title.empty())
        std::cout << std::string(static_cast<std::size_t>(depth), ' ') << role << " | " << title << '\n';
    if (showing && title == name && (command != "set" || role == "spin button" || role == "text") && match++ == wanted) {
        if (command == "expect") return true;
        if (command == "click") {
            auto* action = atspi_accessible_get_action_iface(object);
            if (action) {
                const bool result = atspi_action_do_action(action, 0, nullptr); g_object_unref(action);
                if (result) return true;
            }
        }
        if (command == "set") {
            auto* value = atspi_accessible_get_value_iface(object);
            if (value) {
                const bool result = atspi_value_set_current_value(value, std::stod(argument), nullptr);
                g_object_unref(value); if (result) return true;
            }
            auto* editable = atspi_accessible_get_editable_text_iface(object);
            if (editable) {
                const bool result = atspi_editable_text_set_text_contents(editable, argument.c_str(), nullptr);
                g_object_unref(editable); if (result) return true;
            }
        }
    }
    const int count = atspi_accessible_get_child_count(object, nullptr);
    for (int i = 0; i < count; ++i) {
        auto* child = atspi_accessible_get_child_at_index(object, i, nullptr);
        if (!child) continue;
        const bool found = visit(child, depth + 1); g_object_unref(child);
        if (found) return true;
    }
    return false;
}
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    command = argv[1]; if (argc > 2) name = argv[2]; if (argc > 3) argument = argv[3];
    if (command == "click" && argc > 3) wanted = std::stoi(argument);
    if (atspi_init() != 0) return 2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        while (g_main_context_iteration(nullptr, false)) {}
        match = 0; auto* desktop = atspi_get_desktop(0);
        const bool found = visit(desktop, 0); g_object_unref(desktop);
        if (found || command == "dump") { atspi_exit(); return 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    std::cerr << "Cannot " << command << " visible accessibility node: " << name << '\n';
    atspi_exit(); return 1;
}
