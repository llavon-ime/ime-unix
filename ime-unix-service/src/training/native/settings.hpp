#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace llavon::lora {

class PhraseList;

// Uses the shared engine schema and each host's existing on-disk format.
// Atomic writes reject stale snapshots rather than overwriting external edits.
class SettingsStore final {
public:
    explicit SettingsStore(const QString& tablePath = {}) : tablePath_(tablePath) {}
    static QString configPath();
    static QString phrasesPath();
    static QJsonObject schema();
    QJsonObject load();
    void save(const QJsonObject& values);
    QString loadPhrases();
    void savePhrases(const QString& text);
private:
    QString tablePath_;
    QByteArray configSnapshot_, phrasesSnapshot_;
    bool configLoaded_ = false, phrasesLoaded_ = false;
};

class SettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(bool phrases, QWidget* parent = nullptr, const QString& tablePath = {});
    bool isDirty() const { return dirty_; }
    bool reload();
    void revealField(const QString& key);
private:
    void fill(const QJsonObject& values);
    void buildGroup(const QString& group);
    QJsonObject values() const;
    void save();
    void changed();
    void notifyHost(bool restartService, bool saved = true);
    bool checkPhrases(bool explicitCheck = false);
    void showStatus(const QString& message);
    SettingsStore store_;
    bool phrases_, dirty_ = false, loading_ = false;
    QMap<QString, QWidget*> fields_;
    QMap<QString, QJsonArray> groupFields_;
    QMap<QString, QVBoxLayout*> groupLayouts_;
    PhraseList* phraseList_ = nullptr;
    QLabel* checkStatus_ = nullptr;
    QLabel* status_;
    QJsonObject original_;
};

} // namespace llavon::lora
