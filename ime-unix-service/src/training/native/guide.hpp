#pragma once

#include <QDialog>
#include <QSet>

class QTabWidget;

namespace llavon::lora {

// UI-only preferences, separate from engine settings and training consent.
class SetupPreferences final {
public:
    // The first-run setup was finished, skipped or closed.
    static bool seen();
    static bool markSeen();
};

class GuidePage final : public QDialog {
    Q_OBJECT
public:
    explicit GuidePage(QWidget* parent = nullptr);
signals:
    void pageRequested(const QString& page);
    void settingRequested(const QString& key);
    void setupRequested();
private:
    void buildTopic(int index);
    QSet<int> builtTopics_;
    QTabWidget* topics_;
};

} // namespace llavon::lora
