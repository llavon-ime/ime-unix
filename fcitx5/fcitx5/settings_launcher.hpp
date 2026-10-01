#pragma once
#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>

namespace llavon::ime {
// Fcitx's existing gear remains an entry point. These are launch actions only;
// engine settings and phrase editing are owned by the unified native app.
FCITX_CONFIGURATION(SettingsLauncher,
    fcitx::ExternalOption settings{this, "SettingsManager", "開啟拉風設定與個人化", LLAVON_IME_INSTALLED_SETTINGS_PATH};
    fcitx::ExternalOption phrases{this, "PhraseOverrides", "管理強制替代詞彙", LLAVON_IME_INSTALLED_PHRASES_PATH};
    fcitx::ExternalOption training{this, "LoraManager", "使用我的輸入改進模型", std::string(LLAVON_IME_INSTALLED_LORA_GUI_PATH) + " --page records"};);
}
