#pragma once
#include <QWidget>
#include <QDBusConnection>
#include <QTimer>
#include <functional>

class QLabel;
class QPushButton;
class QCheckBox;

namespace llavon::lora {

// PackageKit owns package trust, dependencies, locks and polkit authorization.
// The UI never downloads executable packages or invokes a root shell.
class LinuxPackageUpdater final : public QObject {
    Q_OBJECT
public:
    explicit LinuxPackageUpdater(QDBusConnection bus = QDBusConnection::systemBus(), QObject* parent = nullptr);
    bool busy() const { return !path_.isEmpty() || creating_; }
    QString candidate() const { return candidate_; }
    void check(bool interactive = true);
    void install();
signals:
    void status(const QString& text);
    void changed();
    void installed();
private slots:
    void package(uint info, const QString& id, const QString& summary);
    void finished(uint exit, uint runtime);
    void errorCode(uint code, const QString& details);
    void properties(const QString& interface, const QVariantMap& changes, const QStringList& invalidated);
private:
    void transaction(const QString& method, const QList<QVariant>& arguments);
    void fail(const QString& text);
    void disconnectTransaction();
    QDBusConnection bus_;
    QString path_, method_, candidate_, error_;
    QStringList results_;
    QTimer watchdog_;
    bool creating_ = false;
    bool interactive_ = true;
};

class LinuxUpdatesPage final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxUpdatesPage(std::function<bool()> trainingBusy, QWidget* parent = nullptr);
    bool busy() const { return updater_.busy() || waitingInstall_ || waitingRestart_; }
private:
    void refresh();
    void poll();
    void restart();
    std::function<bool()> trainingBusy_;
    LinuxPackageUpdater updater_;
    QLabel* status_;
    QPushButton *check_, *install_, *restart_, *cancel_;
    QCheckBox* automatic_;
    QTimer timer_;
    bool waitingInstall_ = false, waitingRestart_ = false, restarting_ = false;
};
} // namespace llavon::lora
