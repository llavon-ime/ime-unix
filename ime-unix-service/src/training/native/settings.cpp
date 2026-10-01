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
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
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
    if (readFile(path) != snapshot) throw std::runtime_error("檔案已由其他程式修改。請先重新載入，再儲存。");
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("無法建立設定目錄");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(data) != data.size() || !file.commit()) throw std::runtime_error(file.errorString().toStdString());
}
QJsonObject qtJson(const nlohmann::json& value) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(value.dump())).object();
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
    if (phrases) {
        auto* toolbar = new QHBoxLayout;
        auto* instructions = new QLabel(QStringLiteral("詞彙與讀音")); instructions->setProperty("role", "section");
        instructions->setWordWrap(true); toolbar->addWidget(instructions, 1);
        auto* add = new QPushButton(QStringLiteral("＋ 新增詞彙")); add->setObjectName("addPhrase"); toolbar->addWidget(add);
        layout->addLayout(toolbar);
        phraseList_ = new PhraseList(tablePath); phraseList_->setObjectName("phraseList"); layout->addWidget(phraseList_, 1);
        connect(add, &QPushButton::clicked, phraseList_, &PhraseList::addRow);
        connect(phraseList_, &PhraseList::changed, this, [this] { changed(); checkPhrases(); });
        checkStatus_ = new QLabel; checkStatus_->setWordWrap(true); checkStatus_->setTextFormat(Qt::PlainText);
        checkStatus_->setObjectName("phraseCheckStatus"); layout->addWidget(checkStatus_);
        auto* tableLocation = new QLabel(QStringLiteral("依注音表選音 · 多音字需手動選擇 · 一聲不標調"));
        tableLocation->setToolTip(phraseList_->tablePath()); tableLocation->setProperty("role", "muted"); layout->addWidget(tableLocation);
    } else {
        auto* tabs = new QTabWidget; tabs->setObjectName("settingsGroups"); layout->addWidget(tabs, 1);
        QMap<QString, QVBoxLayout*> groups;
        for (const auto item : SettingsStore::schema().value("fields").toArray()) {
            const auto field = item.toObject(); const auto group = field.value("group").toString();
            if (!groups.contains(group)) {
                auto* content = new QWidget; content->setObjectName("settingsForm"); content->setAttribute(Qt::WA_StyledBackground, true);
                auto* form = new QVBoxLayout(content);
                form->setContentsMargins(0, 0, 8, 12); form->setSpacing(8); form->addStretch();
                auto* area = new QScrollArea; area->setFrameShape(QFrame::NoFrame); area->setWidgetResizable(true); area->setWidget(content);
                tabs->addTab(area, group); groups.insert(group, form);
            }
            const auto key = field.value("key").toString(), kind = field.value("kind").toString();
            QWidget* input = nullptr;
            if (kind == "boolean" || kind == "choice") {
                auto* combo = new NativeComboBox;
                if (kind == "boolean") { combo->addItem(QStringLiteral("啟用"), true); combo->addItem(QStringLiteral("關閉"), false); }
                else for (const auto option : field.value("choices").toArray()) {
                    const auto choice = option.toObject(); combo->addItem(choice.value("label").toString(), choice.value("value").toString());
                }
                connect(combo, &QComboBox::currentIndexChanged, this, &SettingsPage::changed); input = combo;
            } else if (kind == "integer") {
                auto* spin = new QSpinBox; spin->setRange(field.value("minimum").toInt(), field.value("maximum").toInt());
                connect(spin, &QSpinBox::valueChanged, this, &SettingsPage::changed); input = spin;
            } else {
                auto* line = new QLineEdit; line->setPlaceholderText(QStringLiteral("留空使用預設模型"));
                connect(line, &QLineEdit::textChanged, this, &SettingsPage::changed); input = line;
            }
            input->setObjectName("setting_" + key); input->setMinimumHeight(34);
            input->setAccessibleName(field.value("label").toString()); fields_.insert(key, input);
            auto* settingRow = new QFrame; settingRow->setObjectName("settingRow");
            auto* rowLayout = new QVBoxLayout(settingRow); rowLayout->setContentsMargins(16, 10, 16, 10);
            auto* title = new QLabel(field.value("label").toString()); title->setWordWrap(true); title->setBuddy(input);
            if (key == "model_path") {
                auto* row = new QWidget; auto* horizontal = new QHBoxLayout(row); horizontal->setContentsMargins(0, 0, 0, 0);
                auto* browse = new QPushButton(QStringLiteral("選擇檔案…")); browse->setMinimumHeight(34);
                horizontal->addWidget(input, 1); horizontal->addWidget(browse);
                connect(browse, &QPushButton::clicked, this, [this, input] {
                    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("選擇推論模型"), {}, "GGUF (*.gguf);;All files (*)");
                    if (!path.isEmpty()) qobject_cast<QLineEdit*>(input)->setText(path);
                });
                rowLayout->addWidget(title); rowLayout->addWidget(row);
            } else {
                auto* horizontal = new QHBoxLayout; horizontal->setSpacing(16);
                input->setFixedWidth(200);
                horizontal->addWidget(title, 1); horizontal->addWidget(input);
                rowLayout->addLayout(horizontal);
            }
            groups[group]->insertWidget(groups[group]->count() - 1, settingRow);
        }
    }
    auto* location = new QLabel(phrases ? QStringLiteral("phrase_overrides.txt") : QStringLiteral("設定儲存於此裝置"));
    location->setToolTip(phrases ? SettingsStore::phrasesPath() : SettingsStore::configPath());
    location->setTextFormat(Qt::PlainText); location->setWordWrap(true); location->setProperty("role", "muted");
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_ = new QLabel; status_->setWordWrap(true); status_->setProperty("role", "muted"); status_->setObjectName(phrases ? "phraseStatus" : "settingsStatus"); layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    auto* saveButton = new QPushButton(QStringLiteral("儲存並套用")); saveButton->setProperty("primary", true);
    saveButton->setObjectName(phrases ? "savePhrases" : "saveSettings"); saveButton->setMinimumHeight(36);
    auto* reloadButton = new QPushButton(QStringLiteral("重新載入")); reloadButton->setObjectName(phrases ? "reloadPhrases" : "reloadSettings");
    actions->addWidget(location); actions->addStretch(); actions->addWidget(reloadButton); actions->addWidget(saveButton); layout->addLayout(actions);
    if (phrases) {
        auto* check = new QPushButton(QStringLiteral("檢查全部")); check->setObjectName("checkPhrases"); actions->insertWidget(2, check);
        connect(check, &QPushButton::clicked, this, [this] { checkPhrases(); });
    }
    connect(saveButton, &QPushButton::clicked, this, &SettingsPage::save);
    connect(reloadButton, &QPushButton::clicked, this, [this] {
        if (dirty_ && QMessageBox::question(this, QStringLiteral("重新載入"), QStringLiteral("捨棄尚未儲存的修改？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        if (reload() && phrases_) notifyHost(false, false);
    });
    reload();
}

bool SettingsPage::checkPhrases() {
    const auto errors = phraseList_->errors();
    checkStatus_->setProperty("validation", errors.isEmpty() ? "valid" : "invalid");
    checkStatus_->style()->unpolish(checkStatus_); checkStatus_->style()->polish(checkStatus_);
    auto summary = errors.mid(0, 3).join('\n');
    if (errors.size() > 3) summary += QStringLiteral("\n另有 %1 個問題，請修正後再檢查。").arg(errors.size() - 3);
    checkStatus_->setText(errors.isEmpty() ? QStringLiteral("查表檢查通過 · 字音相符，沒有重複的注音組合。") : summary);
    return errors.isEmpty();
}

void SettingsPage::changed() { if (!loading_) { dirty_ = true; status_->setText(QStringLiteral("有尚未儲存的修改")); } }
void SettingsPage::fill(const QJsonObject& values) {
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        const auto value = values.value(it.key());
        if (auto* spin = qobject_cast<QSpinBox*>(it.value())) spin->setValue(value.toInt());
        else if (auto* combo = qobject_cast<QComboBox*>(it.value())) combo->setCurrentIndex(combo->findData(value.toVariant()));
        else qobject_cast<QLineEdit*>(it.value())->setText(value.toString());
    }
}
QJsonObject SettingsPage::values() const {
    QJsonObject result;
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        if (auto* spin = qobject_cast<QSpinBox*>(it.value())) result[it.key()] = spin->value();
        else if (auto* combo = qobject_cast<QComboBox*>(it.value())) result[it.key()] = QJsonValue::fromVariant(combo->currentData());
        else result[it.key()] = qobject_cast<QLineEdit*>(it.value())->text();
    }
    return result;
}
bool SettingsPage::reload() {
    loading_ = true;
    bool loaded = true;
    try {
        if (phrases_) phraseList_->loadText(store_.loadPhrases());
        else { original_ = store_.load(); fill(original_); }
        dirty_ = false; status_->setText(QStringLiteral("已載入目前設定"));
    } catch (const std::exception& error) { loaded = false; status_->setText(QString::fromUtf8(error.what())); }
    loading_ = false;
    if (phrases_) checkPhrases();
    return loaded;
}
void SettingsPage::save() {
    try {
        bool runtime = false;
        if (phrases_) {
            if (!checkPhrases()) { status_->setText(QStringLiteral("尚未儲存，請先修正上方查表問題。")); return; }
            store_.savePhrases(phraseList_->text());
        }
        else {
            const auto updated = values();
            for (const auto* key : {"model_path", "context_length", "thread_count", "gpu_layers", "idle_timeout_seconds"})
                runtime = runtime || updated.value(key) != original_.value(key);
            store_.save(updated); original_ = updated;
        }
        dirty_ = false; notifyHost(runtime);
    } catch (const std::exception& error) { status_->setText(QString::fromUtf8(error.what())); }
}

void SettingsPage::notifyHost(bool restartService, bool saved) {
    const auto action = saved ? QStringLiteral("已儲存") : QStringLiteral("已重新載入");
#ifdef Q_OS_MACOS
    Q_UNUSED(restartService)
    const auto result = notify_post("org.llavon-ime.lora.model-changed");
    status_->setText(action + (result == NOTIFY_STATUS_OK ? QStringLiteral("，已通知輸入法重新載入。") : QStringLiteral("；切換輸入法後套用。")));
#else
    const auto message = QDBusMessage::createMethodCall("org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1", "ReloadAddonConfig");
    auto call = message; call << "llavon-ime";
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, restartService, action] {
        QDBusPendingReply<> reply = *watcher; watcher->deleteLater();
        status_->setText(action + (reply.isError() ? QStringLiteral("；Fcitx5 尚未回應，請重新啟動輸入法以套用。") : QStringLiteral("，Fcitx5 已重新載入設定。")));
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
