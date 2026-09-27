#pragma once

#include "config/config.hpp"
#include "config/config_schema.hpp"

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bopomofo/keymap.hpp"

#ifndef LLAVON_IME_DISPLAY_VERSION
#define LLAVON_IME_DISPLAY_VERSION "unknown"
#endif

namespace llavon::ime {

inline constexpr const char* kFcitxConfigFile = "conf/llavon-ime.conf";

enum class DisplayVersion { Current };
FCITX_CONFIG_ENUM_NAME(DisplayVersion, LLAVON_IME_DISPLAY_VERSION);

// The text and its readings are separate columns so either half can be edited
// without retyping the other; the file keeps the combined "text readings" form.
FCITX_CONFIGURATION(PhraseOverrideEntryConfig,
    fcitx::Option<std::string> phrase{this, "Phrase", "替代文字"};
    fcitx::Option<std::string> readings{this, "Readings", "注音"};);

// The macOS config window reserves a fixed 200pt label column for every list
// except its own punctuation map ("List|Entries$PunctuationMapEntryConfig"),
// which is the only one it lays out across the full width. Naming this alias
// after that type reuses the full-width layout; frontends that do not know the
// type just render a normal sub config list.
class PunctuationMapEntryConfig final : public PhraseOverrideEntryConfig {
public:
    const char* typeName() const override { return "PunctuationMapEntryConfig"; }
};
FCITX_SPECIALIZE_TYPENAME(PunctuationMapEntryConfig, "PunctuationMapEntryConfig")

FCITX_CONFIGURATION(PhraseOverrideEditorConfig,
    fcitx::OptionWithAnnotation<std::vector<PunctuationMapEntryConfig>, fcitx::ListDisplayOptionAnnotation>
        entries{this, "Entries", "強制替代詞彙", {}, {}, {}, fcitx::ListDisplayOptionAnnotation("Phrase")};);

// String annotation that lists runtime choices so fcitx5 UIs render a combo
// box (see fcitx::EnumAnnotation). The stored value is the label, which is what
// the INI keeps; the engine canonicalizes it when reading the config.
class ChoiceAnnotation {
public:
    ChoiceAnnotation() = default;
    explicit ChoiceAnnotation(std::vector<std::string> choices) : choices_(std::move(choices)) {}

    bool skipDescription() { return false; }
    bool skipSave() { return false; }
    void dumpDescription(fcitx::RawConfig& config) const {
        config.setValueByPath("IsEnum", "True");
        for (size_t i = 0; i < choices_.size(); ++i) {
            config.setValueByPath("Enum/" + std::to_string(i), choices_[i]);
        }
    }

private:
    std::vector<std::string> choices_;
};

using ChoiceOption = fcitx::Option<std::string, fcitx::NoConstrain<std::string>,
                                   fcitx::DefaultMarshaller<std::string>, ChoiceAnnotation>;

// One fcitx Option per engine config field, created from config_fields() at
// runtime. Adding an option to the engine schema makes it show up in the
// fcitx5 config UI (and the macOS settings window) without per-platform code.
class SchemaOptions {
public:
    explicit SchemaOptions(fcitx::Configuration* parent);

    ConfigValue read(const ConfigField& field) const;
    bool write(const ConfigField& field, const ConfigValue& value);

private:
    std::unordered_map<std::string, std::function<ConfigValue()>> readers_;
    std::unordered_map<std::string, std::function<bool(const ConfigValue&)>> writers_;
    std::vector<std::unique_ptr<fcitx::OptionBase>> options_;
};

class ImeFcitxConfig final : public fcitx::Configuration {
public:
    ImeFcitxConfig() = default;
    ImeFcitxConfig(const ImeFcitxConfig& other) : ImeFcitxConfig() { copyHelper(other); }
    ImeFcitxConfig& operator=(const ImeFcitxConfig& other) {
        if (this != &other) copyHelper(other);
        return *this;
    }
    bool operator==(const ImeFcitxConfig& other) const { return compareHelper(other); }
    ~ImeFcitxConfig() override = default;

    const char* typeName() const override { return "ImeFcitxConfig"; }

    // Extra, addon-only entries that are not part of the shared engine config.
    fcitx::Option<DisplayVersion> version{this, "Version", "版本", DisplayVersion::Current};
    SchemaOptions fields{this};
#ifdef __linux__
    fcitx::Option<bool> memoryContextEnabled{this, "MemoryContextEnabled",
        "實驗：聚焦時插入／刪除標記並掃描（會影響 Undo）", false};
    fcitx::Option<std::vector<std::string>> memoryContextPrograms{this, "MemoryContextPrograms",
        "已測試會回報插入與刪除的程式名稱（與執行檔名稱相同）", {}};
    fcitx::Option<std::string> memoryContextStatus{this, "MemoryContextStatus", "記憶體上下文狀態", "disabled"};
#endif
    fcitx::SubConfigOption phraseOverrides{this, "PhraseOverrides", "管理強制替代詞彙",
                                            "fcitx://config/addon/llavon-ime/phraseoverrides"};
    // Fcitx5 config tools render ExternalOption as a button and launch the
    // installed manager only when clicked. It does not become a saved setting.
    fcitx::ExternalOption loraManager{this, "LoraManager", "使用我的輸入改進模型",
                                      LLAVON_IME_INSTALLED_LORA_GUI_PATH};
    fcitx::Option<std::string, fcitx::NoConstrain<std::string>, fcitx::DefaultMarshaller<std::string>,
                  fcitx::ToolTipAnnotation>
        accessibilityStatus{this,
                            "AccessibilityStatus",
                            "無障礙狀態",
                            "",
                            fcitx::NoConstrain<std::string>(),
                            fcitx::DefaultMarshaller<std::string>(),
                            fcitx::ToolTipAnnotation("由 IME 更新的唯讀狀態:顯示能否取得聚焦視窗的文字作為預測上下文;可用時會自動使用")};
};

Config to_shared_config(const ImeFcitxConfig& config);
void apply_shared_config(ImeFcitxConfig& target, const Config& source);

}  // namespace llavon::ime
