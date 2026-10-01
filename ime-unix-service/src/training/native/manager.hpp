#pragma once

#include <QMainWindow>
#include <QJsonObject>
#include <QProcess>
#include <QMap>
#include <QSet>
#include <functional>

class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class QProgressBar;
class QPlainTextEdit;
class QTableWidget;
class QStackedWidget;
class QTimer;

namespace llavon::lora {

class HistoryGraph;
class SettingsPage;
class LinuxUpdatesPage;

class Backend final : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(const QJsonValue&)>;
    explicit Backend(QString program, QStringList arguments, QObject* parent = nullptr);
    ~Backend() override;
    void start();
    QString phraseTablePath() const;
    void request(const QString& path, QJsonObject body = {}, Reply reply = {}, bool post = false);
signals:
    void ready();
    void error(const QString& message);
private:
    QProcess process_;
    QString program_;
    QStringList arguments_;
    QByteArray input_;
    QMap<int, Reply> replies_;
    int serial_ = 0;
};

class Manager final : public QMainWindow {
    Q_OBJECT
public:
    explicit Manager(Backend* backend, const QString& hostHelper = {});
    void refresh();
    void showPage(const QString& page);
signals:
    void refreshed();
protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    QWidget* recordsPage();
    QWidget* trainingPage();
    QWidget* historyPage();
    void act(const QString& path, const QJsonObject& body = {}, Backend::Reply reply = {});
    void renderRecords(const QJsonObject& data);
    void renderProtection(const QJsonObject& data);
    void renderState(const QJsonObject& data);
    void renderHistory(const QJsonObject& data);
    void train();
    QJsonObject trainingRequest() const;
    void notice(const QString& message, bool error = false);
    QString selectedRecord() const;
    QString selectedRun() const;
    void updateRecordActions();

    Backend* backend_;
    QTimer* timer_;
    QStackedWidget* pages_;
    SettingsPage *settings_, *phrases_;
    QLabel *heading_, *subtitle_, *message_, *collection_, *counts_, *pageInfo_, *model_, *trainer_, *device_, *job_, *historyDetail_, *emptyRecords_;
    QLineEdit *password_, *confirmation_, *trainPassword_;
    QLabel* trainPasswordLabel_ = nullptr;
    QComboBox *filter_, *strength_, *base_;
    QCheckBox *manualView_, *manualTrain_, *stabilize_;
    QPushButton *unlock_, *setup_, *lock_, *recording_, *forget_, *previous_, *next_, *exclude_, *delete_, *start_, *cancel_, *ancestor_;
    QTableWidget* records_;
    HistoryGraph* history_;
    QMap<QString, QJsonObject> historyRuns_;
    QProgressBar* progress_;
    QPlainTextEdit* log_;
    QMap<QString, QLineEdit*> options_;
    QList<QPushButton*> jobButtons_;
    QJsonObject protection_, state_;
    QSet<QString> skippedIds_;
    int offset_ = 0;
    int refreshPending_ = 0;
    bool running_ = false;
    bool actionPending_ = false;
#ifdef Q_OS_LINUX
    LinuxUpdatesPage* updates_ = nullptr;
#endif
};

} // namespace llavon::lora
