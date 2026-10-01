#include "phrase_table.hpp"

#include "phrase_override/phrase_override_store.hpp"
#include "text/utf.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace llavon::lora {

QString PhraseTable::resolvePath() {
    if (const auto path = qEnvironmentVariable("LLAVON_IME_TABLE_PATH"); !path.isEmpty()) return path;
    if (const auto path = qEnvironmentVariable("LLAVON_IME_TABLES_DIR"); !path.isEmpty())
        return QDir(path).filePath("bopomofo_char.json");
    const QDir binary(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        binary.filePath("../share/llavon-ime/tables/bopomofo_char.json"),
        binary.filePath("../../../../share/llavon-ime/tables/bopomofo_char.json"),
        QStringLiteral(LLAVON_SETTINGS_INSTALLED_TABLE), QStringLiteral(LLAVON_SETTINGS_SOURCE_TABLE)};
    for (const auto& path : candidates) if (QFileInfo::exists(path)) return QDir::cleanPath(path);
    return candidates.front();
}

PhraseTable::PhraseTable(const QString& path) : path_(QDir::cleanPath(path.isEmpty() ? resolvePath() : path)) {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        error_ = QStringLiteral("無法讀取注音表：%1").arg(path_); return;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject() || document.object().isEmpty()) {
        error_ = QStringLiteral("注音表格式錯誤：%1").arg(path_); return;
    }
    const auto table = document.object();
    for (auto it = table.begin(); it != table.end(); ++it) {
        const auto reading = it.key().trimmed(); // table encodes first tone with a trailing space
        if (reading.isEmpty() || !it.value().isArray()) { error_ = QStringLiteral("注音表含有無效項目"); break; }
        for (const auto value : it.value().toArray()) {
            const auto character = value.toString();
            if (!value.isString() || character.toUcs4().size() != 1) { error_ = QStringLiteral("注音表含有無效字元"); break; }
            auto& entries = readings_[character];
            if (!entries.contains(reading)) entries.append(reading);
        }
        if (!error_.isEmpty()) break;
    }
    if (readings_.isEmpty() && error_.isEmpty()) error_ = QStringLiteral("注音表沒有可用字元");
    if (!error_.isEmpty()) readings_.clear();
}

QStringList PhraseTable::readings(const QString& character) const { return readings_.value(character); }

QStringList PhraseTable::check(const QString& text) const {
    if (!error_.isEmpty()) return {error_};
    QStringList errors;
    QMap<QString, int> seen;
    int lineNumber = 0;
    for (const auto& line : text.split('\n')) {
        ++lineNumber;
        if (line.trimmed().isEmpty() || line.startsWith('#')) continue;
        const auto record = llavon::ime::PhraseOverrideStore::parse_line(line.toStdString());
        const auto prefix = QStringLiteral("第 %1 行：").arg(lineNumber);
        if (!record || !llavon::ime::PhraseOverrideStore::valid_entry(record->phrase, record->readings.size())) {
            errors.append(prefix + QStringLiteral("需填寫 2–8 個字及對應注音，字數須與注音數相同。")); continue;
        }
        const auto phrase = QString::fromStdString(llavon::ime::u16_to_utf8(record->phrase)).toUcs4();
        QStringList syllables;
        for (qsizetype i = 0; i < phrase.size(); ++i) {
            const auto scalar = static_cast<char32_t>(phrase[i]);
            const auto character = QString::fromUcs4(&scalar, 1);
            const auto allowed = readings(character);
            const auto reading = QString::fromStdString(llavon::ime::u16_to_utf8(record->readings[static_cast<std::size_t>(i)])).trimmed();
            syllables.append(reading);
            if (allowed.isEmpty()) errors.append(prefix + QStringLiteral("第 %1 字「%2」不在目前注音表內。").arg(i + 1).arg(character));
            else if (!allowed.contains(reading)) errors.append(prefix + QStringLiteral("第 %1 字「%2」不對應「%3」；可用：%4。")
                .arg(i + 1).arg(character, reading, allowed.join(QStringLiteral("、"))));
        }
        const auto key = syllables.join('-');
        if (seen.contains(key)) errors.append(prefix + QStringLiteral("注音與第 %1 行重複；同一組注音只能指定一個替代詞。").arg(seen.value(key)));
        else seen.insert(key, lineNumber);
    }
    return errors;
}

} // namespace llavon::lora
