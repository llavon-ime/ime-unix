#include "settings.hpp"
#include "phrase_list.hpp"
#include "controls.hpp"

#include "config/config.hpp"
#include "config/config_schema.hpp"
#include "phrase_override/phrase_override_store.hpp"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>
#ifdef Q_OS_MACOS
#include <notify.h>
#else
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace llavon::lora {
namespace {
QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
    return file.readAll();
}
void writeFile(const QString& path, const QByteArray& data, const QByteArray& snapshot) {
    if (readFile(path) != snapshot) throw StaleSettings();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("無法建立設定目錄");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(data) != data.size() || !file.commit()) throw std::runtime_error(file.errorString().toStdString());
}
QJsonObject qtJson(const nlohmann::json& value) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(value.dump())).object();
}
QString settingHelp(const QString& key) {
    static const QMap<QString, QString> help{
        {"model_path", QStringLiteral("留空時使用隨輸入法安裝的模型。")},
        {"inference_provider", QStringLiteral("執行預測模型的方式；一般維持自動即可。")},
        {"context_length", QStringLiteral("預測時參考的前文長度；越長越耗資源。")},
        {"thread_count", QStringLiteral("預測使用的 CPU 執行緒數。")},
        {"gpu_layers", QStringLiteral("交給顯示卡運算的模型層數；0 表示只用 CPU。")},
        {"idle_timeout_seconds", QStringLiteral("預測服務閒置超過這個秒數後釋放資源。")},
        {"selection_key_count", QStringLiteral("選字鍵中實際使用的按鍵數。")},
        {"candidate_page_size", QStringLiteral("候選窗每頁顯示的候選字數量。")},
        {"move_cursor_after_selection", QStringLiteral("選字後，游標移到選定的字之後。")},
        {"escape_action", QStringLiteral("逐步取消：先關閉候選窗，再清除未完成的注音或取消選字。送出組字區：候選窗開啟時先關閉候選窗。")},
        {"smart_english", QStringLiteral("不必切換模式即可混打中英文，輸入法會隨按鍵判斷。")},
        {"keyboard_layout", QStringLiteral("不確定就選「標準」：鍵盤上印的注音就是標準配置。")},
    };
    return help.value(key);
}
// Esc has one behavior, stored as two host settings for compatibility.
constexpr auto escapeKey = "escape_action";
constexpr auto presetKey = "@preset";
QString escapeValue(const QJsonObject& values) {
    if (values.value("esc_commits_composition").toBool()) return QStringLiteral("commit");
    return values.value("esc_clears_entire_buffer").toBool() ? QStringLiteral("clear_buffer") : QStringLiteral("clear_reading");
}
bool runtimeSetting(const QString& key) {
    static const QSet<QString> keys{"model_path", "inference_provider", "context_length", "thread_count", "gpu_layers", "idle_timeout_seconds"};
    return keys.contains(key);
}
}

QString SettingsStore::configPath() {
#ifdef Q_OS_MACOS
    return QString::fromStdString(llavon::ime::legacy_config_path().string());
#else
    return QString::fromStdString(llavon::ime::config_path().string());
#endif
}
QString SettingsStore::phrasesPath() { return QString::fromStdString(llavon::ime::phrase_overrides_path().string()); }
QJsonObject SettingsStore::schema() { return qtJson(llavon::ime::config_schema_json()); }

QJsonObject SettingsStore::load() {
    configLoaded_ = false;
    configSnapshot_ = readFile(configPath());
#ifdef Q_OS_MACOS
    const auto json = configSnapshot_.isEmpty() ? nlohmann::json::object() : nlohmann::json::parse(configSnapshot_.toStdString());
    if (!json.is_object()) throw std::runtime_error("設定檔必須是 JSON 物件");
    const auto result = qtJson(llavon::ime::to_json(llavon::ime::config_from_json(json)));
#else
    const auto result = qtJson(llavon::ime::to_json(llavon::ime::load_config()));
#endif
    configLoaded_ = true;
    return result;
}

void SettingsStore::save(const QJsonObject& values) {
    if (!configLoaded_) throw std::runtime_error("請先成功載入設定");
    // Validate all submitted values against the same definitions the hosts use.
    auto config = llavon::ime::default_config();
    for (const auto& field : llavon::ime::config_fields()) {
        const auto entry = values.value(QString::fromStdString(field.key));
        llavon::ime::ConfigValue value;
        bool valid = false;
        switch (field.kind) {
        case llavon::ime::ConfigValueKind::Boolean:
            valid = entry.isBool(); value = entry.toBool(); break;
        case llavon::ime::ConfigValueKind::Integer:
            valid = entry.isDouble() && entry.toDouble() == entry.toInt() && entry.toDouble() >= field.minimum && entry.toDouble() <= field.maximum;
            value = entry.toInt(); break;
        case llavon::ime::ConfigValueKind::Text:
        case llavon::ime::ConfigValueKind::Choice:
            valid = entry.isString() && !entry.toString().contains('\n') && !entry.toString().contains('\r');
            value = entry.toString().toStdString(); break;
        }
        if (!valid || !llavon::ime::set_config_field_value(config, field, value))
            throw std::runtime_error("設定值無效：" + field.label);
    }
    QByteArray output;
#ifdef Q_OS_MACOS
    auto json = configSnapshot_.isEmpty() ? nlohmann::json::object() : nlohmann::json::parse(configSnapshot_.toStdString());
    const auto normalized = llavon::ime::to_json(config);
    for (const auto& [key, value] : normalized.items()) json[key] = value;
    output = QByteArray::fromStdString(json.dump(2) + "\n");
#else
    QMap<QString, QString> replacements;
    for (const auto& field : llavon::ime::config_fields()) {
        const auto value = llavon::ime::config_field_value(config, field);
        QString encoded;
        switch (field.kind) {
        case llavon::ime::ConfigValueKind::Boolean: encoded = std::get<bool>(value) ? "True" : "False"; break;
        case llavon::ime::ConfigValueKind::Integer: encoded = QString::number(std::get<int>(value)); break;
        case llavon::ime::ConfigValueKind::Text:
        case llavon::ime::ConfigValueKind::Choice:
            encoded = QString::fromStdString(nlohmann::json(field.kind == llavon::ime::ConfigValueKind::Choice ?
                llavon::ime::choice_label(field, std::get<std::string>(value)) : std::get<std::string>(value)).dump()); break;
        }
        replacements.insert(QString::fromStdString(field.ini_key), encoded);
    }
    const auto keys = replacements.keys();
    const QSet<QString> known(keys.begin(), keys.end());
    QStringList lines;
    bool section = false;
    auto appendMissing = [&] {
        for (auto it = replacements.cbegin(); it != replacements.cend(); ++it) lines.append(it.key() + "=" + it.value());
        replacements.clear();
    };
    for (const auto& line : QString::fromUtf8(configSnapshot_).split('\n')) {
        if (line.trimmed().startsWith('[') && !section) { appendMissing(); section = true; }
        const auto equals = line.indexOf('=');
        const auto key = line.left(equals).trimmed();
        if (!section && equals >= 0 && replacements.contains(key)) {
            lines.append(key + "=" + replacements.take(key));
        } else if (section || equals < 0 || !known.contains(key)) lines.append(line);
    }
    appendMissing(); output = (lines.join('\n') + '\n').toUtf8();
#endif
    writeFile(configPath(), output, configSnapshot_); configSnapshot_ = output;
}

QString SettingsStore::loadPhrases() {
    phrasesLoaded_ = false; phrasesSnapshot_ = readFile(phrasesPath()); phrasesLoaded_ = true;
    return QString::fromUtf8(phrasesSnapshot_);
}
void SettingsStore::savePhrases(const QString& text) {
    if (!phrasesLoaded_) throw std::runtime_error("請先成功載入替代詞彙");
    const auto errors = PhraseTable(tablePath_).check(text);
    if (!errors.isEmpty()) throw std::runtime_error(errors.front().toStdString());
    const auto output = text.toUtf8();
    writeFile(phrasesPath(), output, phrasesSnapshot_); phrasesSnapshot_ = output;
}

SettingsPage::SettingsPage(bool phrases, QWidget* parent, const QString& tablePath)
    : QWidget(parent), store_(tablePath), phrases_(phrases) {
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(16);
    QHBoxLayout* actions = nullptr;
    if (phrases) {
        auto* toolbar = new QHBoxLayout;
        auto* instructions = new QLabel(QStringLiteral("詞彙與讀音")); instructions->setProperty("role", "section");
        instructions->setWordWrap(true); toolbar->addWidget(instructions, 1);
        auto* add = new QPushButton(QStringLiteral("＋ 新增詞彙")); add->setObjectName("addPhrase"); toolbar->addWidget(add);
        add->setToolTip(QStringLiteral("每筆 2–8 個字，多音字需選擇讀音；一聲不標調。"));
        layout->addLayout(toolbar);
        phraseList_ = new PhraseList(tablePath); phraseList_->setObjectName("phraseList"); layout->addWidget(phraseList_, 1);
        connect(add, &QPushButton::clicked, phraseList_, &PhraseList::addRow);
        connect(phraseList_, &PhraseList::changed, this, [this] { changed(); checkPhrases(); });
        checkStatus_ = new QLabel; checkStatus_->setWordWrap(true); checkStatus_->setTextFormat(Qt::PlainText);
        checkStatus_->setObjectName("phraseCheckStatus"); layout->addWidget(checkStatus_);
        phraseList_->setToolTip(phraseList_->tablePath());
        phraseList_->setAccessibleDescription(add->toolTip());
        actions = new QHBoxLayout;
    } else {
        settle_ = new QTimer(this); settle_->setSingleShot(true); settle_->setInterval(600);
        connect(settle_, &QTimer::timeout, this, &SettingsPage::autoSave);
        auto* content = new QWidget; content->setObjectName("settingsForm"); content->setAttribute(Qt::WA_StyledBackground, true);
        auto* form = new QVBoxLayout(content); form->setContentsMargins(0, 0, 14, 12); form->setSpacing(10);
        // Common settings are always shown; the others are built on first expansion.
        const auto addSection = [this, form](const QString& name, const QString& title, const QString& help) {
            auto* body = new QWidget; auto* bodyLayout = new QVBoxLayout(body);
            bodyLayout->setContentsMargins(0, 0, 0, 8); bodyLayout->setSpacing(10);
            if (name == "common") {
                auto* heading = new QLabel(title); heading->setProperty("role", "section"); form->addWidget(heading);
            } else {
                auto* toggle = new QPushButton; toggle->setObjectName(name == "details" ? "settingsDetailsToggle" : "settingsAdvancedToggle");
                toggle->setCheckable(true); toggle->setProperty("quiet", true); toggle->setProperty("disclosure", true);
                toggle->setAutoDefault(false); toggle->setMinimumHeight(32); toggle->setAccessibleName(title);
                toggle->setAccessibleDescription(help); toggle->setToolTip(help); toggle->setProperty("title", title);
                auto* row = new QHBoxLayout; row->setSpacing(8); row->addWidget(toggle);
                auto* detail = new QLabel(help); detail->setProperty("role", "help"); detail->setWordWrap(true);
                detail->setTextFormat(Qt::PlainText); row->addWidget(detail, 1);
                form->addSpacing(6); form->addLayout(row);
                sectionToggles_.insert(name, toggle);
                connect(toggle, &QPushButton::toggled, this, [this, name](bool expanded) { expandSection(name, expanded); });
                body->hide();
            }
            auto* card = new QFrame; card->setObjectName("settingsCard");
            auto* rows = new QVBoxLayout(card); rows->setContentsMargins(0, 0, 0, 0); rows->setSpacing(0);
            bodyLayout->addWidget(card);
            form->addWidget(body);
            sectionLayouts_.insert(name, rows); sectionBodies_.insert(name, body);
            return bodyLayout;
        };
        addSection("common", QStringLiteral("常用"), {});
        addSection("details", QStringLiteral("自訂細節"), QStringLiteral("按鍵與選字的細節；選擇輸入習慣時會一併調整。"));
        auto* advanced = addSection("advanced", QStringLiteral("進階：模型與執行"), QStringLiteral("一般不需要調整；儲存後會重新啟動預測服務。"));
        form->addStretch();
        for (const auto& name : sectionToggles_.keys()) expandSection(name, false);
        // Model and runtime edits restart the service, so they keep an explicit save.
        actions = new QHBoxLayout; advanced->addLayout(actions);
        QJsonArray common;
        for (const auto& key : {"keyboard_layout", presetKey, "smart_english", "selection_keys", "candidate_layout"})
            common.append(QJsonObject{{"key", key}});
        for (const auto item : SettingsStore::schema().value("fields").toArray()) {
            const auto field = item.toObject(); const auto key = field.value("key").toString();
            if (field.value("group").toString() == QStringLiteral("模型與執行")) {
                advancedKeys_.insert(key); sectionFields_["advanced"].append(field); fieldSections_.insert(key, "advanced");
            } else if (key == "esc_clears_entire_buffer" || key == "esc_commits_composition") {
                if (!fieldSections_.contains(escapeKey)) {
                    sectionFields_["details"].append(QJsonObject{{"key", escapeKey}, {"label", QStringLiteral("Esc 鍵")}, {"kind", "escape"}});
                    fieldSections_.insert(escapeKey, "details");
                }
                fieldSections_.insert(key, "details");
            } else if (std::ranges::any_of(common, [&key](const QJsonValue& entry) { return entry.toObject().value("key").toString() == key; })) {
                for (int index = 0; index < common.size(); ++index)
                    if (common[index].toObject().value("key").toString() == key) common[index] = field;
                fieldSections_.insert(key, "common");
            } else {
                sectionFields_["details"].append(field); fieldSections_.insert(key, "details");
            }
        }
        sectionFields_.insert("common", common); fieldSections_.insert(presetKey, "common");
        auto* area = new QScrollArea; area->setObjectName("settingsScroll"); area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true); area->setWidget(content); layout->addWidget(area, 1);
        buildSection("common");
    }
    status_ = new QLabel; status_->setWordWrap(true); status_->setProperty("role", "muted"); status_->setObjectName(phrases ? "phraseStatus" : "settingsStatus"); layout->addWidget(status_);
    auto* saveButton = new QPushButton(QStringLiteral("儲存並套用")); saveButton->setProperty("primary", true);
    saveButton_ = saveButton; saveButton->setEnabled(false);
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this); saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, [this] { if (dirty_) save(); else flushPending(); });
    saveButton->setObjectName(phrases ? "savePhrases" : "saveSettings"); saveButton->setMinimumHeight(36);
    saveButton->setToolTip(phrases ? SettingsStore::phrasesPath() : SettingsStore::configPath());
    auto* reloadButton = new QPushButton(QStringLiteral("重新載入")); reloadButton->setObjectName(phrases ? "reloadPhrases" : "reloadSettings");
    actions->addStretch(); actions->addWidget(reloadButton); actions->addWidget(saveButton);
    if (phrases) {
        layout->addLayout(actions);
        auto* check = new QPushButton(QStringLiteral("檢查全部")); check->setObjectName("checkPhrases"); actions->insertWidget(1, check);
        connect(check, &QPushButton::clicked, this, [this] { checkPhrases(true); });
    }
    connect(saveButton, &QPushButton::clicked, this, &SettingsPage::save);
    connect(reloadButton, &QPushButton::clicked, this, [this] {
        if (dirty_ && QMessageBox::question(this, QStringLiteral("重新載入"), QStringLiteral("捨棄尚未儲存的修改？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        if (reload() && phrases_) notifyHost(false, false);
    });
    reload();
}

void SettingsPage::expandSection(const QString& section, bool expanded) {
    auto* toggle = sectionToggles_.value(section);
    if (!toggle) return;
    toggle->setText((expanded ? QStringLiteral("▾  ") : QStringLiteral("▸  ")) + toggle->property("title").toString());
    if (expanded) buildSection(section);
    sectionBodies_.value(section)->setVisible(expanded);
}

void SettingsPage::buildSection(const QString& section) {
    if (!sectionFields_.contains(section)) return;
    const bool wasLoading = loading_; loading_ = true;
    const auto items = sectionFields_.take(section);
    auto* form = sectionLayouts_.value(section);
    QStringList keys;
    for (const auto item : items) {
        const auto field = item.toObject();
        const auto key = field.value("key").toString(), kind = field.value("kind").toString();
        QString title = field.value("label").toString(), help = settingHelp(key);
        QWidget* input = nullptr;
        if (key == presetKey) {
            auto* combo = new NativeComboBox; combo->setObjectName("settingsPreset");
            for (const auto entry : SettingsStore::schema().value("presets").toArray()) {
                const auto preset = entry.toObject();
                combo->addItem(preset.value("label").toString(), preset.value("key").toString());
                combo->setItemData(combo->count() - 1, preset.value("description").toString(), Qt::ToolTipRole);
            }
            // Choosing a habit applies it at once; it never changes the keyboard,
            // smart English or the model.
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo] {
                if (loading_) return;
                if (const auto preset = combo->currentData().toString(); preset != "custom") applyPreset(preset);
            });
            title = QStringLiteral("輸入習慣"); preset_ = combo; input = combo;
        } else if (kind == "escape") {
            auto* combo = new NativeComboBox;
            combo->addItem(QStringLiteral("逐步取消"), "clear_reading");
            combo->addItem(QStringLiteral("清除整個組字區"), "clear_buffer");
            combo->addItem(QStringLiteral("送出組字區"), "commit");
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo] {
                if (loading_) return;
                const auto choice = combo->currentData().toString();
                current_["esc_commits_composition"] = choice == "commit";
                current_["esc_clears_entire_buffer"] = choice == "clear_buffer";
                edited("esc_commits_composition", false);
            });
            input = combo;
        } else if (kind == "boolean") {
            auto* toggle = new NativeSwitch;
            connect(toggle, &QCheckBox::toggled, this, [this, key](bool checked) {
                if (loading_) return;
                current_[key] = checked; edited(key, false);
            });
            input = toggle;
        } else if (kind == "choice") {
            auto* combo = new NativeComboBox;
            for (const auto option : field.value("choices").toArray()) {
                const auto choice = option.toObject(); combo->addItem(choice.value("label").toString(), choice.value("value").toString());
            }
            connect(combo, &QComboBox::currentIndexChanged, this, [this, key, combo] {
                if (loading_) return;
                current_[key] = QJsonValue::fromVariant(combo->currentData()); edited(key, false);
            });
            input = combo;
        } else if (kind == "integer") {
            auto* spin = new QSpinBox; spin->setRange(field.value("minimum").toInt(), field.value("maximum").toInt());
            // Typing "12" passes through 1; save once the value settles.
            connect(spin, &QSpinBox::valueChanged, this, [this, key](int value) {
                if (loading_) return;
                current_[key] = value; edited(key, true);
            });
            connect(spin, &QSpinBox::editingFinished, this, &SettingsPage::flushPending);
            input = spin;
        } else {
            auto* line = new QLineEdit; line->setPlaceholderText(QStringLiteral("留空使用預設模型"));
            connect(line, &QLineEdit::textChanged, this, [this, key](const QString& text) {
                if (loading_) return;
                current_[key] = text; edited(key, true);
            });
            input = line;
        }
        if (key != presetKey) input->setObjectName("setting_" + key);
        if (kind != "boolean") input->setMinimumHeight(34);
        input->setAccessibleName(title); fields_.insert(key, input); keys.append(key);
        auto* settingRow = new QFrame; settingRow->setObjectName("settingRow");
        auto* rowLayout = new QVBoxLayout(settingRow); rowLayout->setContentsMargins(16, 9, 16, 9); rowLayout->setSpacing(6);
        auto* titleLabel = new QLabel(title); titleLabel->setWordWrap(true); titleLabel->setBuddy(input);
        auto* heading = new QVBoxLayout; heading->setSpacing(2); heading->addWidget(titleLabel);
        if (key == presetKey) {
            presetHelp_ = new QLabel; presetHelp_->setProperty("role", "help"); presetHelp_->setWordWrap(true);
            presetHelp_->setTextFormat(Qt::PlainText); presetHelp_->setObjectName("settingsPresetDescription");
            heading->addWidget(presetHelp_);
        } else if (!help.isEmpty()) {
            auto* detail = new QLabel(help); detail->setProperty("role", "help"); detail->setWordWrap(true); detail->setTextFormat(Qt::PlainText);
            heading->addWidget(detail); input->setAccessibleDescription(help);
        }
        if (key == "model_path") {
            auto* row = new QWidget; auto* horizontal = new QHBoxLayout(row); horizontal->setContentsMargins(0, 0, 0, 0);
            auto* browse = new QPushButton(QStringLiteral("選擇檔案…")); browse->setMinimumHeight(34);
            auto* useDefault = new QPushButton(QStringLiteral("使用預設")); useDefault->setObjectName("useDefaultModel");
            useDefault->setMinimumHeight(34);
            useDefault->setToolTip(QStringLiteral("清除自訂模型路徑；按「儲存並套用」後改用隨輸入法安裝的模型。"));
            horizontal->addWidget(input, 1); horizontal->addWidget(browse); horizontal->addWidget(useDefault);
            connect(browse, &QPushButton::clicked, this, [this, input] {
                const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("選擇推論模型"), {}, "GGUF (*.gguf);;All files (*)");
                if (!path.isEmpty()) qobject_cast<QLineEdit*>(input)->setText(path);
            });
            const auto updateDefaultButton = [input, useDefault] {
                useDefault->setVisible(!qobject_cast<QLineEdit*>(input)->text().trimmed().isEmpty());
            };
            connect(qobject_cast<QLineEdit*>(input), &QLineEdit::textChanged, useDefault,
                    [updateDefaultButton](const QString&) { updateDefaultButton(); });
            connect(useDefault, &QPushButton::clicked, this, [this, input] {
                qobject_cast<QLineEdit*>(input)->clear();
                showStatus(QStringLiteral("將改用安裝的預設模型；請按「儲存並套用」完成。"));
            });
            rowLayout->addLayout(heading); rowLayout->addWidget(row);
        } else {
            auto* horizontal = new QHBoxLayout; horizontal->setSpacing(16);
            if (kind != "boolean") input->setFixedWidth(200);
            horizontal->addLayout(heading, 1); horizontal->addWidget(input);
            rowLayout->addLayout(horizontal);
        }
        if (form->count() > 0) {
            auto* divider = new QFrame; divider->setObjectName("rowDivider"); divider->setFixedHeight(1);
            form->addWidget(divider);
        }
        form->addWidget(settingRow);
    }
    // New widgets show the current values, including edits and presets made
    // while this section was still collapsed.
    for (const auto& key : keys) fillField(key);
    if (auto* useDefault = findChild<QPushButton*>("useDefaultModel"); useDefault && fields_.contains("model_path"))
        useDefault->setVisible(!qobject_cast<QLineEdit*>(fields_.value("model_path"))->text().trimmed().isEmpty());
    loading_ = wasLoading;
    if (keys.contains(presetKey)) syncPreset();
}

void SettingsPage::fillField(const QString& key) {
    auto* input = fields_.value(key);
    if (!input || key == presetKey) return;
    const bool wasLoading = loading_; loading_ = true;
    const auto value = current_.value(key);
    if (key == escapeKey) {
        auto* combo = qobject_cast<QComboBox*>(input); combo->setCurrentIndex(combo->findData(escapeValue(current_)));
    } else if (auto* spin = qobject_cast<QSpinBox*>(input)) spin->setValue(value.toInt());
    else if (auto* toggle = qobject_cast<QCheckBox*>(input)) toggle->setChecked(value.toBool());
    else if (auto* combo = qobject_cast<QComboBox*>(input)) combo->setCurrentIndex(combo->findData(value.toVariant()));
    else if (auto* line = qobject_cast<QLineEdit*>(input)) line->setText(value.toString());
    loading_ = wasLoading;
}

void SettingsPage::fill() {
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) fillField(it.key());
    syncPreset();
}

void SettingsPage::revealField(const QString& key) {
    const auto target = key == "esc_clears_entire_buffer" || key == "esc_commits_composition" ? QString(escapeKey) : key;
    const auto section = fieldSections_.value(target);
    if (section.isEmpty()) return;
    if (auto* toggle = sectionToggles_.value(section)) toggle->setChecked(true);
    buildSection(section);
    auto* input = fields_.value(target);
    if (!input) return;
    if (auto* area = findChild<QScrollArea*>("settingsScroll")) {
        area->widget()->layout()->activate();
        area->ensureWidgetVisible(input);
    }
    input->setFocus(Qt::OtherFocusReason);
}

QString SettingsPage::matchingPreset() const {
    for (const auto item : SettingsStore::schema().value("presets").toArray()) {
        const auto preset = item.toObject(); const auto values = preset.value("values").toObject();
        bool matches = true;
        for (auto it = values.constBegin(); matches && it != values.constEnd(); ++it) matches = current_.value(it.key()) == it.value();
        if (matches) return preset.value("key").toString();
    }
    return {};
}

void SettingsPage::syncPreset() {
    if (!preset_) return;
    const bool wasLoading = loading_; loading_ = true;
    const auto match = matchingPreset();
    auto custom = preset_->findData("custom");
    if (match.isEmpty()) {
        if (custom < 0) {
            preset_->addItem(QStringLiteral("自訂"), "custom"); custom = preset_->count() - 1;
            preset_->setItemData(custom, QStringLiteral("已調整自訂細節，與任何預設組合都不同。"), Qt::ToolTipRole);
        }
        preset_->setCurrentIndex(custom);
    } else {
        preset_->setCurrentIndex(preset_->findData(match));
        if (custom >= 0) preset_->removeItem(custom);
    }
    presetHelp_->setText(preset_->currentData(Qt::ToolTipRole).toString());
    preset_->setAccessibleDescription(presetHelp_->text());
    loading_ = wasLoading;
}

void SettingsPage::setValue(const QString& key, const QJsonValue& value) {
    if (phrases_ || !current_.contains(key) || current_.value(key) == value) return;
    current_[key] = value; fillField(key); edited(key, false);
}

void SettingsPage::applyPreset(const QString& key) {
    QJsonObject preset;
    for (const auto item : SettingsStore::schema().value("presets").toArray()) {
        if (item.toObject().value("key").toString() == key) { preset = item.toObject(); break; }
    }
    if (preset.isEmpty()) return;
    const auto values = preset.value("values").toObject();
    bool modified = false;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (current_.value(it.key()) == it.value()) continue;
        current_[it.key()] = it.value(); modified = true;
    }
    fill();
    if (!modified) { showStatus(QStringLiteral("目前設定已與「%1」相同。").arg(preset.value("label").toString())); return; }
    emit valuesChanged();
    autoSave();
}

void SettingsPage::edited(const QString& key, bool settle) {
    if (advancedKeys_.contains(key)) { changed(); return; }
    syncPreset();
    emit valuesChanged();
    if (settle) settle_->start(); else autoSave();
}

void SettingsPage::flushPending() {
    if (settle_ && settle_->isActive()) autoSave();
}

void SettingsPage::hideEvent(QHideEvent* event) {
    flushPending();
    QWidget::hideEvent(event);
}

void SettingsPage::autoSave() {
    if (settle_) settle_->stop();
    // Only input settings save here; unsaved model edits stay pending.
    auto target = original_;
    for (auto it = current_.constBegin(); it != current_.constEnd(); ++it)
        if (!advancedKeys_.contains(it.key())) target[it.key()] = it.value();
    if (target == original_) return;
    try {
        try {
            store_.save(target);
        } catch (const StaleSettings&) {
            // Another program changed the file, such as the training page
            // choosing a model: keep its values and write only the edits here.
            auto latest = store_.load();
            for (auto it = target.constBegin(); it != target.constEnd(); ++it)
                if (it.value() != original_.value(it.key())) latest[it.key()] = it.value();
            store_.save(latest); target = latest;
            for (auto it = latest.constBegin(); it != latest.constEnd(); ++it)
                if (!advancedKeys_.contains(it.key()) || current_.value(it.key()) == original_.value(it.key())) current_[it.key()] = it.value();
            autoSaveFailed_ = false;
            original_ = target; fill(); changed();
            notifyHost(false);
            return;
        }
        original_ = target;
        if (autoSaveFailed_) { autoSaveFailed_ = false; changed(); }
        notifyHost(false);
    } catch (const std::exception& error) {
        // Keep the edit: "儲存並套用" retries it, and the page is not reloaded
        // over it while it is unsaved.
        autoSaveFailed_ = true;
        setDirty(true);
        showStatus(QStringLiteral("尚未儲存：%1").arg(QString::fromUtf8(error.what())));
    }
}

bool SettingsPage::checkPhrases(bool explicitCheck) {
    const auto errors = phraseList_->errors();
    checkStatus_->setProperty("validation", errors.isEmpty() ? "valid" : "invalid");
    checkStatus_->style()->unpolish(checkStatus_); checkStatus_->style()->polish(checkStatus_);
    auto summary = errors.mid(0, 3).join('\n');
    if (errors.size() > 3) summary += QStringLiteral("\n另有 %1 個問題，請修正後再檢查。").arg(errors.size() - 3);
    checkStatus_->setText(errors.isEmpty() ? (explicitCheck ? QStringLiteral("檢查通過") : QString()) : summary);
    checkStatus_->setVisible(!checkStatus_->text().isEmpty());
    return errors.isEmpty();
}

void SettingsPage::showStatus(const QString& message) { status_->setText(message); status_->setVisible(!message.isEmpty()); }
void SettingsPage::setDirty(bool dirty) {
    saveButton_->setEnabled(dirty);
    if (dirty != dirty_) { dirty_ = dirty; emit dirtyChanged(dirty); }
}
void SettingsPage::changed() {
    if (loading_) return;
    // Undoing an edit returns to the saved state instead of staying "unsaved".
    bool dirty = false;
    if (phrases_) dirty = phraseList_->text() != loadedPhrases_;
    else {
        dirty = autoSaveFailed_;
        for (const auto& key : advancedKeys_) dirty = dirty || current_.value(key) != original_.value(key);
    }
    setDirty(dirty);
    showStatus(dirty_ ? QStringLiteral("尚未儲存") : QString());
}
bool SettingsPage::reload() {
    flushPending();
    loading_ = true;
    bool loaded = true;
    try {
        if (phrases_) { phraseList_->loadText(store_.loadPhrases()); loadedPhrases_ = phraseList_->text(); }
        else { original_ = store_.load(); current_ = original_; fill(); autoSaveFailed_ = false; }
        setDirty(false); showStatus({});
    } catch (const std::exception& error) { loaded = false; showStatus(QString::fromUtf8(error.what())); }
    loading_ = false;
    if (phrases_) checkPhrases();
    else emit valuesChanged();
    return loaded;
}
void SettingsPage::save() {
    try {
        bool runtime = false;
        if (phrases_) {
            if (!checkPhrases()) { showStatus(QStringLiteral("尚未儲存，請先修正上方查表問題。")); return; }
            store_.savePhrases(phraseList_->text()); loadedPhrases_ = phraseList_->text();
        }
        else {
            if (settle_) settle_->stop();
            for (auto it = current_.constBegin(); it != current_.constEnd(); ++it)
                runtime = runtime || (runtimeSetting(it.key()) && it.value() != original_.value(it.key()));
            store_.save(current_); original_ = current_; autoSaveFailed_ = false;
        }
        setDirty(false); notifyHost(runtime);
    } catch (const std::exception& error) { showStatus(QString::fromUtf8(error.what())); }
}

void SettingsPage::notifyHost(bool restartService, bool saved) {
    const auto action = saved ? QStringLiteral("已儲存") : QStringLiteral("已重新載入");
#ifdef Q_OS_MACOS
    Q_UNUSED(restartService)
    const auto result = notify_post("org.llavon-ime.lora.model-changed");
    showStatus(action + (result == NOTIFY_STATUS_OK ? QStringLiteral("並套用") : QStringLiteral("；切換輸入法後套用。")));
#else
    const auto message = QDBusMessage::createMethodCall("org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1", "ReloadAddonConfig");
    auto call = message; call << "llavon-ime";
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, restartService, action] {
        QDBusPendingReply<> reply = *watcher; watcher->deleteLater();
        showStatus(action + (reply.isError() ? QStringLiteral("；Fcitx5 尚未回應，請重新啟動輸入法以套用。") : QStringLiteral("並套用")));
        if (restartService && !reply.isError()) {
            auto* socket = new QLocalSocket(this);
            connect(socket, &QLocalSocket::connected, socket, [socket] {
                socket->write(QByteArray::fromHex("020000000600")); socket->flush();
            });
            QTimer::singleShot(1500, socket, &QObject::deleteLater);
            auto path = qEnvironmentVariable("LLAVON_IME_UNIX_SOCKET_PATH");
            if (path.isEmpty()) path = QString::fromStdString(llavon::ime::socket_path().string());
            socket->connectToServer(path);
        }
    });
#endif
}

} // namespace llavon::lora
