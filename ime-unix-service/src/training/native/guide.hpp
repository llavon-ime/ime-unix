#pragma once

#include <QDialog>
#include <QSet>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTabWidget;

namespace llavon::lora {

// UI-only preferences, separate from engine settings and training consent.
class QuickStartPreferences final {
public:
    static bool dismissed();
    static bool dismiss();
};

class GuidePage final : public QDialog {
    Q_OBJECT
public:
    explicit GuidePage(QWidget* parent = nullptr);
    void startQuickGuide();
signals:
    void pageRequested(const QString& page);
    void settingRequested(const QString& key);
    void finished();
    void skipped();
protected:
    void changeEvent(QEvent* event) override;
private:
    void updateStep();
    void buildTopic(int index);
    QSet<int> builtTopics_;
    QTabWidget* topics_;
    QStackedWidget* steps_;
    QLabel* progress_;
    QPushButton *previous_ = nullptr, *next_ = nullptr;
};

} // namespace llavon::lora
