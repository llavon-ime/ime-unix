#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QWidget>
#include <stdexcept>

class QComboBox;
class QLabel;
class QVBoxLayout;
class QPushButton;
class QTimer;

namespace llavon::lora {

class PhraseList;

// The file changed after it was loaded; the caller may reload and retry.
class StaleSettings final : public std::runtime_error {
public:
    StaleSettings() : std::runtime_error("檔案已由其他程式修改。請先重新載入，再儲存。") {}
};

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

// Input settings save as soon as they change. Model and runtime settings
// restart the prediction service, so they wait for an explicit save.
class SettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(bool phrases, QWidget* parent = nullptr, const QString& tablePath = {});
    // Only unsaved model and runtime edits (or phrase edits) count.
    bool isDirty() const { return dirty_; }
    bool reload();
    void revealField(const QString& key);
    // Writes input settings whose save is still waiting for typing to settle.
    void flushPending();
    QJsonValue value(const QString& key) const { return current_.value(key); }
    void setValue(const QString& key, const QJsonValue& value);
    void applyPreset(const QString& key);
    // The preset the current input settings match, or empty when customized.
    QString matchingPreset() const;
signals:
    void dirtyChanged(bool dirty);
    void valuesChanged();
protected:
    void hideEvent(QHideEvent* event) override;
private:
    void setDirty(bool dirty);
    void fill();
    void fillField(const QString& key);
    void buildSection(const QString& section);
    void expandSection(const QString& section, bool expanded);
    void edited(const QString& key, bool settle);
    void syncPreset();
    void autoSave();
    void save();
    void changed();
    void notifyHost(bool restartService, bool saved = true);
    bool checkPhrases(bool explicitCheck = false);
    void showStatus(const QString& message);
    SettingsStore store_;
    bool phrases_, dirty_ = false, loading_ = false;
    QMap<QString, QWidget*> fields_;
    QMap<QString, QJsonArray> sectionFields_;
    QMap<QString, QVBoxLayout*> sectionLayouts_;
    QMap<QString, QWidget*> sectionBodies_;
    QMap<QString, QPushButton*> sectionToggles_;
    QMap<QString, QString> fieldSections_;
    QSet<QString> advancedKeys_;
    QComboBox* preset_ = nullptr;
    QLabel* presetHelp_ = nullptr;
    QTimer* settle_ = nullptr;
    PhraseList* phraseList_ = nullptr;
    QLabel* checkStatus_ = nullptr;
    QLabel* status_;
    QPushButton* saveButton_ = nullptr;
    QString loadedPhrases_;
    // original_ is what the file holds; current_ includes unsaved edits.
    QJsonObject original_, current_;
};

} // namespace llavon::lora
