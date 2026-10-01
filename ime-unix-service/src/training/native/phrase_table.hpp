#pragma once

#include <QMap>
#include <QStringList>

namespace llavon::lora {

// Reverse index of the installed, canonical reading → character table.
class PhraseTable final {
public:
    explicit PhraseTable(const QString& path = {});
    static QString resolvePath();
    QStringList readings(const QString& character) const;
    QStringList check(const QString& text) const;
    const QString& path() const { return path_; }
    const QString& error() const { return error_; }
private:
    QString path_, error_;
    QMap<QString, QStringList> readings_;
};

} // namespace llavon::lora
