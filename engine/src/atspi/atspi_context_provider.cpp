#include "atspi/atspi_context_provider.hpp"

#include "text/utf.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include "util/env.hpp"
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <atspi/atspi.h>
#include <dlfcn.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace llavon::ime {

// libatspi is optional at runtime: it is loaded with dlopen so the addon still
// loads on systems without at-spi2-core. When the library is missing the
// provider reports itself unavailable and the engine simply has no
// accessibility context. LLAVON_IME_ATSPI_LIBRARY overrides the library name
// for tests.
struct AtspiLibrary {
    using Init = void (*)();
    using IsInitialized = gboolean (*)();
    using SetTimeout = void (*)(gint, gint);
    using Exit = void (*)();
    using GetDesktopCount = gint (*)();
    using GetDesktop = AtspiAccessible* (*)(gint);
    using GetTextIface = AtspiText* (*)(AtspiAccessible*);
    using GetRole = AtspiRole (*)(AtspiAccessible*, GError**);
    using GetChildCount = gint (*)(AtspiAccessible*, GError**);
    using GetChildAtIndex = AtspiAccessible* (*)(AtspiAccessible*, gint, GError**);
    using GetStateSet = AtspiStateSet* (*)(AtspiAccessible*);
    using StateSetContains = gboolean (*)(AtspiStateSet*, AtspiStateType);
    using TextGetCaretOffset = gint (*)(AtspiText*, GError**);
    using TextGetText = gchar* (*)(AtspiText*, gint, gint, GError**);
    using ListenerNewSimple = AtspiEventListener* (*)(AtspiEventListenerSimpleCB, GDestroyNotify);
    using ListenerRegister = gboolean (*)(AtspiEventListener*, const gchar*, GError**);

    bool open() {
        if (handle != nullptr) return true;
        const char* override = env_with_legacy("LLAVON_IME_ATSPI_LIBRARY", "IME_FCITX5_ATSPI_LIBRARY");
        const char* name = override != nullptr && override[0] != '\0' ? override : "libatspi.so.0";
        handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) return false;

#define LLAVON_IME_LOAD_ATSPI(field, symbol)                                  \
    field = reinterpret_cast<decltype(field)>(::dlsym(handle, #symbol));      \
    if (field == nullptr) {                                                   \
        close();                                                              \
        return false;                                                         \
    }
        LLAVON_IME_LOAD_ATSPI(init, atspi_init)
        LLAVON_IME_LOAD_ATSPI(is_initialized, atspi_is_initialized)
        LLAVON_IME_LOAD_ATSPI(set_timeout, atspi_set_timeout)
        LLAVON_IME_LOAD_ATSPI(exit, atspi_exit)
        LLAVON_IME_LOAD_ATSPI(get_desktop_count, atspi_get_desktop_count)
        LLAVON_IME_LOAD_ATSPI(get_desktop, atspi_get_desktop)
        LLAVON_IME_LOAD_ATSPI(get_text_iface, atspi_accessible_get_text_iface)
        LLAVON_IME_LOAD_ATSPI(get_role, atspi_accessible_get_role)
        LLAVON_IME_LOAD_ATSPI(get_child_count, atspi_accessible_get_child_count)
        LLAVON_IME_LOAD_ATSPI(get_child_at_index, atspi_accessible_get_child_at_index)
        LLAVON_IME_LOAD_ATSPI(get_state_set, atspi_accessible_get_state_set)
        LLAVON_IME_LOAD_ATSPI(state_set_contains, atspi_state_set_contains)
        LLAVON_IME_LOAD_ATSPI(text_get_caret_offset, atspi_text_get_caret_offset)
        LLAVON_IME_LOAD_ATSPI(text_get_text, atspi_text_get_text)
        LLAVON_IME_LOAD_ATSPI(listener_new_simple, atspi_event_listener_new_simple)
        LLAVON_IME_LOAD_ATSPI(listener_register, atspi_event_listener_register)
#undef LLAVON_IME_LOAD_ATSPI
        return true;
    }

    void close() {
        if (handle != nullptr) ::dlclose(handle);
        handle = nullptr;
    }

    void* handle = nullptr;
    Init init = nullptr;
    IsInitialized is_initialized = nullptr;
    SetTimeout set_timeout = nullptr;
    Exit exit = nullptr;
    GetDesktopCount get_desktop_count = nullptr;
    GetDesktop get_desktop = nullptr;
    GetTextIface get_text_iface = nullptr;
    GetRole get_role = nullptr;
    GetChildCount get_child_count = nullptr;
    GetChildAtIndex get_child_at_index = nullptr;
    GetStateSet get_state_set = nullptr;
    StateSetContains state_set_contains = nullptr;
    TextGetCaretOffset text_get_caret_offset = nullptr;
    TextGetText text_get_text = nullptr;
    ListenerNewSimple listener_new_simple = nullptr;
    ListenerRegister listener_register = nullptr;

    ~AtspiLibrary() { close(); }
};

namespace {

bool connect_unix_path(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
    return rc == 0;
}

bool connect_unix_abstract(const std::string& name) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (name.size() + 1 > sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, name.data(), name.size());
    const socklen_t length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), length);
    ::close(fd);
    return rc == 0;
}

bool connect_bus_address(const std::string& address) {
    constexpr std::string_view path_prefix = "unix:path=";
    constexpr std::string_view abstract_prefix = "unix:abstract=";
    const std::string_view base = std::string_view(address).substr(0, address.find(','));
    if (base.rfind(path_prefix, 0) == 0) return connect_unix_path(std::string(base.substr(path_prefix.size())));
    if (base.rfind(abstract_prefix, 0) == 0) {
        return connect_unix_abstract(std::string(base.substr(abstract_prefix.size())));
    }
    return false;
}

bool scan_atspi_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return false;
    for (const auto& entry : it) {
        if (connect_unix_path(entry.path().string())) return true;
    }
    return false;
}

// Asking org.a11y.Bus for its address starts at-spi-bus-launcher through D-Bus
// activation on desktops that do not keep the accessibility bus running.
std::string query_a11y_bus_address() {
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        g_clear_error(&error);
        return {};
    }

    GVariant* result = g_dbus_connection_call_sync(
        connection, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", nullptr,
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    std::string address;
    if (result != nullptr) {
        const char* value = nullptr;
        g_variant_get(result, "(&s)", &value);
        if (value != nullptr) address = value;
        g_variant_unref(result);
    }
    g_clear_error(&error);
    g_object_unref(connection);
    return address;
}

bool bus_reachable() {
    if (const char* address = std::getenv("AT_SPI_BUS_ADDRESS"); address != nullptr && address[0] != '\0') {
        if (connect_bus_address(address)) return true;
    }

    std::vector<std::filesystem::path> dirs;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && runtime[0] != '\0') {
        dirs.emplace_back(std::filesystem::path(runtime) / "at-spi");
    }
    std::filesystem::path fallback(
        "/run/user/" + std::to_string(static_cast<long long>(::getuid())) + "/at-spi");
    if (dirs.empty() || dirs.front() != fallback) dirs.push_back(fallback);
    for (const auto& dir : dirs) {
        if (scan_atspi_dir(dir)) return true;
    }

    const std::string activated = query_a11y_bus_address();
    return !activated.empty() && connect_bus_address(activated);
}

}  // namespace

class AtspiContextProvider::Impl {
public:
    Impl(AtspiContextProvider& owner, size_t max_code_units)
        : owner_(owner), max_code_units_(max_code_units) {}

    bool running() const noexcept { return running_.load(); }

    bool start() {
        if (running_.load()) return true;
        // libatspi cannot be initialised twice in one process: a failed start
        // (missing a11y bus, failed listener, or a browser-style reload) must
        // not be retried, or the next attempt crashes inside libatspi.
        if (attempted_.load()) return false;
        stop();
        return start_backend();
    }

    void stop() {
        running_.store(false);
        if (backend_thread_.joinable()) {
            if (loop_ != nullptr) {
                // Quit on the loop's own context. Calling g_main_loop_quit
                // here can race with the worker entering g_main_loop_run:
                // a quit before run starts is lost, and join then hangs.
                GSource* quit = g_idle_source_new();
                g_source_set_priority(quit, G_PRIORITY_HIGH);
                g_source_set_callback(quit, [](gpointer data) -> gboolean {
                    g_main_loop_quit(static_cast<GMainLoop*>(data));
                    return G_SOURCE_REMOVE;
                }, loop_, nullptr);
                g_source_attach(quit, context_);
                g_source_unref(quit);
            }
            backend_thread_.join();
        }
    }

    void refresh() {
        if (!running_.load() || !owner_.active()) return;
        queue_idle();
    }

private:
    static inline Impl* active_instance_ = nullptr;

    static bool is_password_text(AtspiLibrary& api, AtspiAccessible* object) {
        GError* error = nullptr;
        const AtspiRole role = api.get_role(object, &error);
        g_clear_error(&error);
        return role == ATSPI_ROLE_PASSWORD_TEXT;
    }

    static bool has_text_interface(AtspiLibrary& api, AtspiAccessible* object) {
        AtspiText* text = api.get_text_iface(object);
        if (text == nullptr) return false;
        g_object_unref(text);
        return true;
    }

    static bool is_focused(AtspiLibrary& api, AtspiAccessible* object) {
        if (object == nullptr) return false;
        AtspiStateSet* states = api.get_state_set(object);
        const bool focused = states != nullptr && api.state_set_contains(states, ATSPI_STATE_FOCUSED);
        if (states != nullptr) g_object_unref(states);
        return focused;
    }

    AtspiAccessible* find_focused_text(AtspiAccessible* root, int depth, int* visits) {
        if (root == nullptr || depth > kMaxTreeDepth || *visits >= kMaxTreeVisits) return nullptr;
        ++*visits;

        AtspiStateSet* states = api_.get_state_set(root);
        const bool focused = states != nullptr && api_.state_set_contains(states, ATSPI_STATE_FOCUSED);
        if (states != nullptr) g_object_unref(states);
        if (focused && has_text_interface(api_, root) && !is_password_text(api_, root)) return g_object_ref(root);

        const int children = api_.get_child_count(root, nullptr);
        for (int i = 0; i < children; ++i) {
            GError* error = nullptr;
            AtspiAccessible* child = api_.get_child_at_index(root, i, &error);
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
        const int count = api_.get_desktop_count();
        for (int i = 0; i < count; ++i) {
            AtspiAccessible* desktop = api_.get_desktop(i);
            if (desktop == nullptr) continue;
            if (AtspiAccessible* found = find_focused_text(desktop, 0, &visits)) return found;
        }
        return nullptr;
    }

    void publish_from_object(AtspiAccessible* object) {
        if (object == nullptr || is_password_text(api_, object)) {
            owner_.publish(std::u16string(), false);
            return;
        }
        AtspiText* text = api_.get_text_iface(object);
        if (text == nullptr) {
            owner_.publish(std::u16string(), false);
            return;
        }

        GError* error = nullptr;
        const gint caret = api_.text_get_caret_offset(text, &error);
        if (error != nullptr || caret < 0) {
            g_clear_error(&error);
            g_object_unref(text);
            owner_.publish(std::u16string(), false);
            return;
        }

        const gint window =
            static_cast<gint>(std::min<size_t>(max_code_units_, static_cast<size_t>(kMaxRequestCharacters)));
        const gint start = caret > window ? caret - window : 0;
        gchar* raw = api_.text_get_text(text, start, caret, &error);
        std::u16string sample;
        bool usable = false;
        if (error == nullptr && raw != nullptr) {
            try {
                sample = utf8_prefix_tail(raw, static_cast<size_t>(caret - start), max_code_units_);
                usable = true;
            } catch (const std::exception&) {
                usable = false;
            }
        }
        if (raw != nullptr) g_free(raw);
        g_clear_error(&error);
        g_object_unref(text);
        owner_.publish(std::move(sample), usable);
    }

    void queue_idle() {
        GMainContext* context = context_;
        if (context == nullptr || idle_source_.load() != 0) return;
        idle_generation_.store(owner_.activation_generation());
        GSource* source = g_idle_source_new();
        g_source_set_callback(source, &Impl::on_idle, this, nullptr);
        idle_source_.store(g_source_attach(source, context));
        g_source_unref(source);
    }

    static gboolean on_idle(gpointer data) {
        auto* self = static_cast<Impl*>(data);
        self->idle_source_.store(0);
        if (!self->owner_.active()) {
            if (self->pending_source_ != nullptr) {
                g_object_unref(self->pending_source_);
                self->pending_source_ = nullptr;
            }
            return G_SOURCE_REMOVE;
        }
        if (self->idle_generation_.load() != self->owner_.activation_generation()) {
            if (self->pending_source_ != nullptr) {
                g_object_unref(self->pending_source_);
                self->pending_source_ = nullptr;
            }
            self->queue_idle();
            return G_SOURCE_REMOVE;
        }
        if (self->pending_source_ != nullptr) {
            AtspiAccessible* source = self->pending_source_;
            self->pending_source_ = nullptr;
            if (is_focused(self->api_, source)) {
                self->publish_from_object(source);
            } else {
                AtspiAccessible* focused = self->focused_text_object();
                if (focused != nullptr) {
                    self->publish_from_object(focused);
                    g_object_unref(focused);
                } else {
                    // Some toolkits focus a non-text ancestor while emitting
                    // caret events from its text child. The activation check
                    // above still protects against samples queued before an
                    // input-context switch.
                    self->publish_from_object(source);
                }
            }
            g_object_unref(source);
        } else {
            AtspiAccessible* focused = self->focused_text_object();
            self->publish_from_object(focused);
            if (focused != nullptr) g_object_unref(focused);
        }
        return G_SOURCE_REMOVE;
    }

    static void on_event(const AtspiEvent* event) {
        Impl* self = active_instance_;
        if (self == nullptr || event == nullptr || event->type == nullptr) return;
        if (!self->owner_.active()) return;
        const std::string type(event->type);
        if (type != "object:state-changed:focused" && type != "object:text-caret-moved" &&
            type.find("object:text-changed") != 0) {
            return;
        }
        // A focus loss reports the widget being left behind; sampling it would
        // publish the previous field's text with a fresh sequence right after
        // the engine switched contexts.
        if (type == "object:state-changed:focused" && event->detail1 == 0) {
            if (event->source != nullptr && has_text_interface(self->api_, event->source)) {
                if (self->pending_source_ != nullptr) {
                    g_object_unref(self->pending_source_);
                    self->pending_source_ = nullptr;
                }
                self->owner_.publish(std::u16string(), false);
            }
            return;
        }
        if (event->source != nullptr && has_text_interface(self->api_, event->source)) {
            if (is_password_text(self->api_, event->source)) {
                self->owner_.publish(std::u16string(), false);
                return;
            }
            if (self->pending_source_ != nullptr) g_object_unref(self->pending_source_);
            self->pending_source_ = static_cast<AtspiAccessible*>(g_object_ref(event->source));
        }
        self->queue_idle();
    }

    void run() {
        context_ = g_main_context_new();
        g_main_context_push_thread_default(context_);
        bool ok = false;
        AtspiEventListener* listener = nullptr;
        do {
            if (!bus_reachable()) {
                owner_.set_availability(AccessibilityAvailability::Unavailable, "a11y-bus-unavailable");
                break;
            }
            api_.init();
            if (!api_.is_initialized()) {
                owner_.set_availability(AccessibilityAvailability::Unavailable, "atspi-init-failed");
                break;
            }
            api_.set_timeout(1000, 1000);

            GError* error = nullptr;
            listener = api_.listener_new_simple(&Impl::on_event, nullptr);
            if (!api_.listener_register(listener, "object:state-changed:focused", &error) ||
                !api_.listener_register(listener, "object:text-caret-moved", &error) ||
                !api_.listener_register(listener, "object:text-changed", &error)) {
                g_clear_error(&error);
                owner_.set_availability(AccessibilityAvailability::Unavailable, "atspi-listener-failed");
                break;
            }
            loop_ = g_main_loop_new(context_, FALSE);
            ok = loop_ != nullptr;
            if (!ok) {
                owner_.set_availability(AccessibilityAvailability::Unavailable, "atspi-loop-failed");
            }
        } while (false);

        // Clear the running flag before the waiter can observe a failed
        // start; otherwise a caller that retries `start()` right after it
        // returns false can find the stale flag and report success.
        if (!ok) running_.store(false);
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            ready_ = true;
            ready_ok_ = ok;
        }
        ready_cv_.notify_all();
        if (!ok) {
            if (listener != nullptr) g_object_unref(listener);
            g_main_context_pop_thread_default(context_);
            g_main_context_unref(context_);
            context_ = nullptr;
            if (api_.is_initialized()) api_.exit();
            return;
        }

        owner_.set_availability(AccessibilityAvailability::Available, "atspi");
        active_instance_ = this;
        queue_idle();
        g_main_loop_run(loop_);
        active_instance_ = nullptr;
        if (pending_source_ != nullptr) {
            g_object_unref(pending_source_);
            pending_source_ = nullptr;
        }
        g_object_unref(listener);
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        api_.exit();
        g_main_context_pop_thread_default(context_);
        g_main_context_unref(context_);
        context_ = nullptr;
        running_.store(false);
    }

    bool start_backend() {
        if (!api_.open()) {
            owner_.set_availability(AccessibilityAvailability::Unavailable, "libatspi-missing");
            return false;
        }
        attempted_.store(true);
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            ready_ = false;
            ready_ok_ = false;
        }
        running_.store(true);
        backend_thread_ = std::jthread([this]() { run(); });

        std::unique_lock<std::mutex> lock(ready_mutex_);
        if (!ready_cv_.wait_for(lock, std::chrono::seconds(5), [this]() { return ready_; })) {
            running_.store(false);
        }
        return ready_ok_;
    }

    static constexpr int kMaxTreeDepth = 32;
    static constexpr int kMaxTreeVisits = 20000;
    static constexpr gint kMaxRequestCharacters = 8192;

    AtspiContextProvider& owner_;
    size_t max_code_units_ = 0;
    AtspiLibrary api_;
    std::atomic<bool> running_{false};
    std::atomic<bool> attempted_{false};
    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;
    std::jthread backend_thread_;
    std::atomic<guint> idle_source_{0};
    std::atomic<std::uint64_t> idle_generation_{0};
    AtspiAccessible* pending_source_ = nullptr;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    bool ready_ = false;
    bool ready_ok_ = false;
};

AtspiContextProvider::AtspiContextProvider(size_t max_code_units)
    : AccessibilityContextProvider(max_code_units),
      impl_(std::make_unique<Impl>(*this, max_code_units)) {}

AtspiContextProvider::~AtspiContextProvider() { stop(); }

bool AtspiContextProvider::start() { return impl_->start(); }

void AtspiContextProvider::stop() { impl_->stop(); }

bool AtspiContextProvider::running() const noexcept { return impl_->running(); }

void AtspiContextProvider::refresh() { impl_->refresh(); }

}  // namespace llavon::ime
