#include "raw_key_harness.hpp"
#include "text/utf.hpp"

using namespace llavon::ime::rawkey;

// Raw document keys and engine keys share this frontend-shaped fixture. Host
// metadata is supplied at the key callback, not by setting composition state.
namespace {
class NativeDocument {
public:
    Harness harness;
    std::string text;
    void document_key(const Key& key) {
        text += llavon::ime::char32_to_utf8(key.input_key().sym);
    }
    void engine_key(std::string_view key, bool excluded = true) {
        const auto units = llavon::ime::utf8_to_u16(text);
        llavon::ime::HostContext context;
        context.valid = true;
        context.text = units;
        context.cursor = context.anchor = units.size();
        context.may_include_preedit = !excluded;
        harness.host().set_surrounding(std::move(context));
        const auto before = harness.commits().size();
        harness.key(key);
        const auto commits = harness.commits();
        for (std::size_t i = before; i < commits.size(); ++i) text += commits.at(i);
    }
};
}

RAWKEY_SUITE("native document excludes composition without losing matching suffix", native_excluded_preedit) {
    NativeDocument native;
    native.document_key(Key(U'你'));
    native.document_key(Key(U'🙂'));
    native.document_key(Key(U'你'));
    native.engine_key("s");
    RAWKEY_ASSERT(native.harness.preedit() == "ㄋ");
    native.engine_key("u");
    RAWKEY_ASSERT(native.harness.preedit() == "ㄋㄧ");
    native.engine_key("3");
    RAWKEY_ASSERT(native.harness.preedit() == "你");
    RAWKEY_ASSERT(native.harness.context_text() == "你🙂你");
    RAWKEY_ASSERT(native.text == "你🙂你");
    native.engine_key("Space");
    RAWKEY_ASSERT(native.harness.has_candidates());
    native.engine_key("1");
    RAWKEY_ASSERT(!native.harness.has_candidates());
    native.engine_key("Return");
    RAWKEY_ASSERT(native.harness.composition_empty());
    RAWKEY_ASSERT(native.harness.last_commit() == "你");
    RAWKEY_ASSERT(native.text == "你🙂你你");
    native.engine_key("s");
    native.engine_key("BackSpace");
    RAWKEY_ASSERT(native.harness.composition_empty());
    RAWKEY_ASSERT(native.harness.commits().size() == 1);
    native.engine_key("s");
    native.engine_key("u");
    native.engine_key("3");
    RAWKEY_ASSERT(native.harness.context_text() == "你🙂你你");
    native.engine_key("Escape");
    RAWKEY_ASSERT(native.text == "你🙂你你");
    RAWKEY_ASSERT(native.harness.commits().size() == 1);
}

RAWKEY_SUITE("legacy preedit inclusion still strips a genuine drawing", native_legacy_inclusion) {
    NativeDocument client;
    // The document model for this host draws the current preedit into text.
    client.document_key(Key(U'文'));
    client.engine_key("s", false);
    client.engine_key("u", false);
    client.document_key(Key(U'你'));
    client.engine_key("3", false);
    RAWKEY_ASSERT(client.harness.context_text() == "文");
    RAWKEY_ASSERT(client.harness.preedit() == "你");
    client.engine_key("Escape", false);
    RAWKEY_ASSERT(client.harness.commits().empty());
}

RAWKEY_SUITE("native focus settlement commits to the original attached object", native_original_focus_commit) {
    Harness harness;
    harness.type("su3");
    RAWKEY_ASSERT(harness.preedit() == "你");
    harness.focus_out();
    const auto original = harness.host().commits();
    RAWKEY_ASSERT(original.size() == 1);
    RAWKEY_ASSERT(original[0].first == 1 && original[0].second == u"你");
    harness.use_context(2);
    RAWKEY_ASSERT(harness.composition_empty());
    RAWKEY_ASSERT(harness.context_text().empty());
    harness.type("cl3");
    RAWKEY_ASSERT(harness.preedit() == "好");
    harness.key("Return");
    const auto committed = harness.host().commits();
    RAWKEY_ASSERT(committed.size() == 2);
    RAWKEY_ASSERT(committed[1].first == 2 && committed[1].second == u"好");
    harness.use_context(1);
    RAWKEY_ASSERT(harness.composition_empty());
    harness.focus_out();
    RAWKEY_ASSERT(harness.host().commits() == committed);
}

RAWKEY_SUITE("native Alt tab shortcut cannot select an IME candidate", native_alt_tab_shortcut) {
    Harness harness;
    harness.type("su3");
    harness.key("Space");
    RAWKEY_ASSERT(harness.has_candidates());
    const auto candidates = harness.candidates();
    const auto cursor = harness.cursor_index();
    RAWKEY_ASSERT(!harness.key_accepted("Alt+1"));
    RAWKEY_ASSERT(harness.preedit() == "你");
    RAWKEY_ASSERT(harness.candidates() == candidates);
    RAWKEY_ASSERT(harness.cursor_index() == cursor);
    RAWKEY_ASSERT(harness.commits().empty());
    harness.focus_out();
    const auto committed = harness.host().commits();
    RAWKEY_ASSERT(committed.size() == 1);
    RAWKEY_ASSERT(committed[0].first == 1 && committed[0].second == u"你");
    harness.use_context(2);
    RAWKEY_ASSERT(!harness.key_accepted("Alt+1"));
    RAWKEY_ASSERT(harness.composition_empty());
    RAWKEY_ASSERT(harness.host().commits() == committed);
}
