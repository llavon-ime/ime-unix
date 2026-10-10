#include "context/caret_prefix_sampler.hpp"
#include "text/utf.hpp"

#include <atspi/atspi.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// Characters requested before the caret. The sampler then bounds the sample
// in UTF-16 code units, so the D-Bus payload stays small even in huge
// documents.
constexpr int kWindowCharacters = 512;
constexpr size_t kWindowCodeUnits = 512;
constexpr int kMaxTreeDepth = 32;
constexpr int kMaxTreeVisits = 20000;

bool connect_unix(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
    return rc == 0;
}

}  // namespace

// dbind's fatal g_error cannot be neutralized, so before calling libatspi we
// test whether the a11y bus socket is actually reachable. A stale socket left
// behind by a dead at-spi-registry would otherwise make libatspi abort the
// process. Returns the socket path when reachable, or an empty string.
std::string reachable_bus_socket() {
    const char* address = getenv("AT_SPI_BUS_ADDRESS");
    if (address == nullptr) {
        // libatspi reads the bus address from the org.a11y.Bus service; the
        // common fallback is the per-user launcher directory. Scan it for a
        // socket we can actually connect to (a stale socket left by a dead
        // registry is refused and must not crash libatspi).
        const std::string dir = "/run/user/" + std::to_string(static_cast<long long>(getuid())) + "/at-spi";
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        if (ec) return {};
        for (const auto& entry : it) {
            const auto candidate = entry.path().string();
            if (connect_unix(candidate)) return candidate;
        }
        return {};
    }
    std::string path(address);
    if (path.rfind("unix:path=", 0) == 0) path = path.substr(10);
    return connect_unix(path) ? path : std::string();
}

guint idle_source = 0;
AtspiAccessible* pending_source = nullptr;

bool is_password_text(AtspiAccessible* obj) {
    GError* error = nullptr;
    const AtspiRole role = atspi_accessible_get_role(obj, &error);
    g_clear_error(&error);
    return role == ATSPI_ROLE_PASSWORD_TEXT;
}

bool has_text_interface(AtspiAccessible* obj) {
    AtspiText* text = atspi_accessible_get_text_iface(obj);
    if (text == nullptr) return false;
    g_object_unref(text);
    return true;
}

// Depth-first search for the focused, editable text widget. AT-SPI marks the
// focused widget (not the application) with ATSPI_STATE_FOCUSED, so app-level
// checks alone return nothing on a real desktop.
AtspiAccessible* find_focused_text(AtspiAccessible* root, int depth, int* visits) {
    if (root == nullptr || depth > kMaxTreeDepth || *visits >= kMaxTreeVisits) return nullptr;
    ++*visits;

    AtspiStateSet* states = atspi_accessible_get_state_set(root);
    const bool focused = states != nullptr && atspi_state_set_contains(states, ATSPI_STATE_FOCUSED);
    if (states != nullptr) g_object_unref(states);

    if (focused && has_text_interface(root) && !is_password_text(root)) return g_object_ref(root);

    const int children = atspi_accessible_get_child_count(root, nullptr);
    for (int i = 0; i < children; ++i) {
        GError* error = nullptr;
        AtspiAccessible* child = atspi_accessible_get_child_at_index(root, i, &error);
        g_clear_error(&error);
        if (child == nullptr) continue;
        AtspiAccessible* found = find_focused_text(child, depth + 1, visits);
        g_object_unref(child);
        if (found != nullptr) return found;
    }
    return nullptr;
}

AtspiAccessible* focused_text_object() {
    int visits = 0;
    const int count = atspi_get_desktop_count();
    for (int i = 0; i < count; ++i) {
        AtspiAccessible* desktop = atspi_get_desktop(i);
        if (desktop == nullptr) continue;
        if (AtspiAccessible* found = find_focused_text(desktop, 0, &visits)) return found;
    }
    return nullptr;
}

void print_snapshot(AtspiAccessible* obj, const char* why) {
    if (obj == nullptr) {
        std::printf("[%s] no focused text object\n", why);
        return;
    }
    GError* error = nullptr;
    gchar* name = atspi_accessible_get_name(obj, &error);
    g_clear_error(&error);
    gchar* role = atspi_accessible_get_role_name(obj, &error);
    g_clear_error(&error);

    AtspiText* text = atspi_accessible_get_text_iface(obj);
    if (text == nullptr) {
        std::printf("[%s] name=\"%s\" role=\"%s\" no text interface\n", why, name ? name : "?",
                    role ? role : "?");
        g_free(name);
        g_free(role);
        return;
    }

    const gint caret = atspi_text_get_caret_offset(text, &error);
    if (error != nullptr) {
        std::printf("[%s] caret query failed: %s\n", why, error->message);
        g_error_free(error);
        g_object_unref(text);
        g_free(name);
        g_free(role);
        return;
    }
    const gint length = atspi_text_get_character_count(text, &error);
    if (error != nullptr) {
        std::printf("[%s] length query failed: %s\n", why, error->message);
        g_error_free(error);
        g_object_unref(text);
        g_free(name);
        g_free(role);
        return;
    }

    // Request only the last window of characters before the caret. AT-SPI
    // offsets are character (scalar) offsets, not UTF-16 code units.
    const gint request_start = caret < 0 ? 0 : (caret > kWindowCharacters ? caret - kWindowCharacters : 0);
    const gint request_end = caret < 0 ? 0 : caret;
    gchar* raw = atspi_text_get_text(text, request_start, request_end, &error);
    if (error != nullptr) {
        std::printf("[%s] text query failed: %s\n", why, error->message);
        g_error_free(error);
        g_object_unref(text);
        g_free(name);
        g_free(role);
        return;
    }
    std::printf("[%s] name=\"%s\" role=\"%s\" caret=%d length=%d window=%d..%d\n", why,
                name ? name : "?", role ? role : "?", caret, length, request_start, request_end);

    std::u16string utf16;
    if (raw != nullptr) {
        try {
            utf16 = llavon::ime::utf8_prefix_tail(
                raw, static_cast<std::size_t>(request_end - request_start), kWindowCodeUnits);
        } catch (const std::runtime_error&) {
            std::printf("[%s] malformed UTF-8 from the accessibility bus\n", why);
        }
        g_free(raw);
    }
    g_object_unref(text);
    g_free(name);
    g_free(role);

    llavon::ime::CaretPrefixSampler sampler(kWindowCodeUnits);
    sampler.set_text(std::move(utf16), kWindowCodeUnits);

    const std::u16string& prefix = sampler.text_before_caret();
    std::string printable;
    try {
        printable = llavon::ime::u16_to_utf8(prefix);
    } catch (const std::runtime_error&) {
    }
    const size_t show = printable.size() < 240 ? printable.size() : 240;
    std::printf("[%s] prefix units=%zu sample=\"%s%s\"\n", why, prefix.size(),
                printable.substr(0, show).c_str(), show < printable.size() ? "..." : "");
}

gboolean handle_snapshot(gpointer user_data) {
    (void)user_data;
    idle_source = 0;
    if (pending_source != nullptr) {
        print_snapshot(pending_source, "event");
        g_object_unref(pending_source);
        pending_source = nullptr;
    } else {
        AtspiAccessible* focused = focused_text_object();
        print_snapshot(focused, "poll");
        if (focused != nullptr) g_object_unref(focused);
    }
    return G_SOURCE_REMOVE;
}

void queue_snapshot() {
    if (idle_source != 0) return;
    idle_source = g_idle_add(handle_snapshot, nullptr);
}

void on_event(const AtspiEvent* event, void* user_data) {
    (void)user_data;
    if (event == nullptr || event->type == nullptr) return;
    const std::string type(event->type);
    if (type != "object:state-changed:focused" && type != "object:text-caret-moved" &&
        type.find("object:text-changed") != 0) {
        return;
    }
    std::printf("[event] %s\n", event->type);
    if (event->source != nullptr && has_text_interface(event->source) && !is_password_text(event->source)) {
        if (pending_source != nullptr) g_object_unref(pending_source);
        pending_source = static_cast<AtspiAccessible*>(g_object_ref(event->source));
    }
    queue_snapshot();
}

int main(int argc, char**) {
    // dbind treats a failed a11y-bus connection as a fatal g_error and aborts
    // the process, so verify the bus socket is reachable before touching
    // libatspi. This also covers stale sockets left by a dead registry.
    if (reachable_bus_socket().empty()) {
        std::printf("no live accessibility bus socket; start at-spi-bus-launcher (is the a11y stack running?)\n");
        return 2;
    }

    (void)atspi_init();

    if (!atspi_is_initialized()) {
        std::printf("no accessibility bus available (is at-spi-bus-launcher running?)\n");
        return 2;
    }
    atspi_set_timeout(1000, 1000);

    if (argc > 1) {
        AtspiAccessible* focused = focused_text_object();
        print_snapshot(focused, "snapshot");
        if (focused != nullptr) g_object_unref(focused);
        atspi_exit();
        return 0;
    }

    AtspiEventListener* listener =
        atspi_event_listener_new_simple([](const AtspiEvent* e) { on_event(e, nullptr); }, nullptr);
    GError* error = nullptr;
    if (!atspi_event_listener_register(listener, "object:state-changed:focused", &error) ||
        !atspi_event_listener_register(listener, "object:text-caret-moved", &error) ||
        !atspi_event_listener_register(listener, "object:text-changed", &error)) {
        std::printf("listener registration failed: %s\n", error ? error->message : "?");
        g_clear_error(&error);
        return 3;
    }
    std::printf("listening for focus / caret / text events; move your caret in an editor\n");

    AtspiAccessible* focused = focused_text_object();
    print_snapshot(focused, "initial");
    if (focused != nullptr) g_object_unref(focused);
    atspi_event_main();
    atspi_exit();
    return 0;
}
