#include "linux_updates.hpp"
#include "controls.hpp"
#include <QCheckBox>
#include <QDateTime>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace llavon::lora {
namespace {
const QString service = QStringLiteral("org.freedesktop.PackageKit");
const QString interface = QStringLiteral("org.freedesktop.PackageKit.Transaction");
constexpr qulonglong installedFilter = 1ULL << 2;
constexpr uint successExit = 1;
bool isProduct(const QString& id) {
    const auto parts = id.split(';');
    return parts.size() == 4 && parts[0] == "llavon-ime-fcitx5" && !parts[1].isEmpty();
}
bool externalTrainingBusy() {
    for (const auto& directory : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false; directory.toUInt(&numeric); if (!numeric) continue;
        QFile comm("/proc/" + directory + "/comm");
        if (!comm.open(QIODevice::ReadOnly)) continue;
        const auto name = comm.readAll().trimmed();
        if (name == "llavon-ime-lora" || name == "llavon-lora") return true;
    }
    return false;
}
}

LinuxPackageUpdater::LinuxPackageUpdater(QDBusConnection bus, QObject* parent)
    : QObject(parent), bus_(std::move(bus)) {
    watchdog_.setSingleShot(true);
    connect(&watchdog_, &QTimer::timeout, this, [this] {
        // Do not clear an install transaction: it may still be running inside
        // PackageKit. Keep the UI locked until Finished actually arrives.
        emit status(QStringLiteral("套件管理器仍在處理；請勿重新啟動。可在系統軟體管理工具查看進度。"));
    });
}

void LinuxPackageUpdater::check(bool interactive) {
    if (busy()) return;
    interactive_ = interactive;
    candidate_.clear(); emit changed();
    transaction("Resolve", {QVariant::fromValue(installedFilter), QStringList{"llavon-ime-fcitx5"}});
}
void LinuxPackageUpdater::install() {
    if (busy() || !isProduct(candidate_)) return;
    interactive_ = true;
    // ONLY_TRUSTED (bit 1). The daemon authorizes via the desktop's polkit agent.
    transaction("UpdatePackages", {QVariant::fromValue(qulonglong{1ULL << 1}), QStringList{candidate_}});
}
void LinuxPackageUpdater::transaction(const QString& method, const QList<QVariant>& arguments) {
    creating_ = true; method_ = method; results_.clear(); error_.clear(); emit changed();
    auto message = QDBusMessage::createMethodCall(service, "/org/freedesktop/PackageKit", service, "CreateTransaction");
    auto* watcher = new QDBusPendingCallWatcher(bus_.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, method, arguments] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher; watcher->deleteLater();
        creating_ = false;
        if (reply.isError()) {
            fail(QStringLiteral("無法使用 PackageKit：%1。請安裝發行版的 PackageKit；AUR／原始碼安裝請使用原本的更新工具。")
                 .arg(reply.error().message())); return;
        }
        path_ = reply.value().path();
        bus_.connect(service, path_, interface, "Package", this, SLOT(package(uint,QString,QString)));
        bus_.connect(service, path_, interface, "Finished", this, SLOT(finished(uint,uint)));
        bus_.connect(service, path_, interface, "ErrorCode", this, SLOT(errorCode(uint,QString)));
        bus_.connect(service, path_, "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                     SLOT(properties(QString,QVariantMap,QStringList)));
        auto hints = QDBusMessage::createMethodCall(service, path_, interface, "SetHints");
        hints << QStringList{interactive_ ? "interactive=true" : "interactive=false"};
        bus_.asyncCall(hints);
        auto call = QDBusMessage::createMethodCall(service, path_, interface, method);
        call.setArguments(arguments);
        auto* started = new QDBusPendingCallWatcher(bus_.asyncCall(call, 15000), this);
        const auto transactionPath = path_;
        connect(started, &QDBusPendingCallWatcher::finished, this, [this, started, transactionPath] {
            QDBusPendingReply<> result = *started; started->deleteLater();
            if (result.isError() && path_ == transactionPath) fail(result.error().message());
        });
        watchdog_.start(120000);
        emit status(method == "UpdatePackages" ? QStringLiteral("正在安裝；需要時系統會要求管理員授權…") : QStringLiteral("正在檢查系統套件更新…"));
        emit changed();
    });
}
void LinuxPackageUpdater::package(uint info, const QString& id, const QString& summary) {
    Q_UNUSED(info) Q_UNUSED(summary)
    if (isProduct(id)) results_.append(id);
}
void LinuxPackageUpdater::errorCode(uint code, const QString& details) {
    Q_UNUSED(code)
    error_ = details;
}
void LinuxPackageUpdater::properties(const QString& name, const QVariantMap& changes, const QStringList& invalidated) {
    Q_UNUSED(invalidated)
    if (name == interface && changes.contains("Percentage")) {
        const auto percentage = changes.value("Percentage").toUInt();
        if (percentage <= 100) emit status(QStringLiteral("套件管理器處理中：%1%").arg(percentage));
    }
}
void LinuxPackageUpdater::disconnectTransaction() {
    watchdog_.stop();
    bus_.disconnect(service, path_, interface, "Package", this, SLOT(package(uint,QString,QString)));
    bus_.disconnect(service, path_, interface, "Finished", this, SLOT(finished(uint,uint)));
    bus_.disconnect(service, path_, interface, "ErrorCode", this, SLOT(errorCode(uint,QString)));
    bus_.disconnect(service, path_, "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                    SLOT(properties(QString,QVariantMap,QStringList)));
    path_.clear();
}
void LinuxPackageUpdater::fail(const QString& text) {
    if (!path_.isEmpty()) disconnectTransaction();
    creating_ = false; candidate_.clear();
    emit status(QStringLiteral("更新未完成：") + text); emit changed();
}
void LinuxPackageUpdater::finished(uint exit, uint runtime) {
    Q_UNUSED(runtime)
    const auto method = method_;
    disconnectTransaction();
    if (exit != successExit || !error_.isEmpty()) { fail(error_.isEmpty() ? QStringLiteral("操作取消或失敗，可再次檢查。") : error_); return; }
    if (method == "Resolve") {
        if (results_.isEmpty()) {
            fail(QStringLiteral("此安裝不是系統套件 llavon-ime-fcitx5；AUR 請用 yay／paru，原始碼請用建置腳本。")); return;
        }
        transaction("RefreshCache", {false}); return;
    }
    if (method == "RefreshCache") { transaction("GetUpdates", {QVariant::fromValue(qulonglong{0})}); return; }
    if (method == "GetUpdates") {
        // Native backend owns version comparison. Never compare GitHub latest
        // against a distro version or replace an AUR -git package.
        if (results_.size() > 1) { fail(QStringLiteral("找到多個架構的更新，請使用系統套件管理器選擇。")); return; }
        candidate_ = results_.value(0);
        emit status(candidate_.isEmpty() ? QStringLiteral("目前套件來源沒有較新的版本。未加入官方 repository 時也會顯示此結果。") :
                    QStringLiteral("可更新至 %1").arg(candidate_.split(';').value(1)));
    } else if (method == "UpdatePackages") {
        candidate_.clear(); emit installed();
    }
    emit changed();
}

LinuxUpdatesPage::LinuxUpdatesPage(std::function<bool()> trainingBusy, QWidget* parent)
    : QWidget(parent), trainingBusy_(std::move(trainingBusy)) {
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(QStringLiteral("透過系統套件管理器更新，需先加入發行版的官方更新來源。"));
    explanation->setWordWrap(true); layout->addWidget(explanation);
    automatic_ = new NativeCheckBox(QStringLiteral("設定視窗開啟時，每日自動檢查更新")); automatic_->setObjectName("automaticUpdates");
    automatic_->setChecked(QSettings().value("updates/automatic", false).toBool()); layout->addWidget(automatic_);
    status_ = new QLabel(QStringLiteral("尚未檢查更新")); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    status_->setObjectName("updateStatus"); layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    check_ = new QPushButton(QStringLiteral("檢查更新")); check_->setObjectName("checkUpdates");
    install_ = new QPushButton(QStringLiteral("立即更新")); install_->setObjectName("installUpdate");
    restart_ = new QPushButton(QStringLiteral("套用新版並重啟輸入法")); restart_->setObjectName("restartUpdatedInputMethod");
    cancel_ = new QPushButton(QStringLiteral("取消等待"));
    for (auto* button : {check_, install_, restart_, cancel_}) actions->addWidget(button);
    layout->addLayout(actions); layout->addStretch();
    connect(&updater_, &LinuxPackageUpdater::status, status_, &QLabel::setText);
    connect(&updater_, &LinuxPackageUpdater::changed, this, &LinuxUpdatesPage::refresh);
    connect(check_, &QPushButton::clicked, this, [this] { updater_.check(); });
    connect(install_, &QPushButton::clicked, this, [this] { waitingInstall_ = true; poll(); refresh(); });
    connect(restart_, &QPushButton::clicked, this, [this] { waitingRestart_ = true; poll(); refresh(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { waitingInstall_ = false; waitingRestart_ = false; status_->setText(QStringLiteral("已取消等待")); refresh(); });
    connect(&updater_, &LinuxPackageUpdater::installed, this, [this] {
        restart_->setVisible(true);
        status_->setText(QStringLiteral("套件已更新；按「套用新版」會等組字與訓練結束後重啟 Fcitx5。設定視窗請重新開啟。"));
    });
    restart_->setVisible(false);
    connect(automatic_, &QCheckBox::toggled, this, [](bool enabled) { QSettings().setValue("updates/automatic", enabled); });
    timer_.setInterval(1000); connect(&timer_, &QTimer::timeout, this, &LinuxUpdatesPage::poll); timer_.start(); refresh();
}
void LinuxUpdatesPage::refresh() {
    check_->setEnabled(!busy()); install_->setEnabled(!busy() && !updater_.candidate().isEmpty());
    restart_->setEnabled(!busy()); cancel_->setVisible(waitingInstall_ || waitingRestart_);
}
void LinuxUpdatesPage::poll() {
    if (waitingInstall_ || waitingRestart_) {
        if (trainingBusy_() || externalTrainingBusy()) { status_->setText(QStringLiteral("等待個人化訓練或管理工作完成…")); return; }
        if (waitingInstall_) { waitingInstall_ = false; updater_.install(); }
        else if (!restarting_) restart();
        refresh(); return;
    }
    if (!automatic_->isChecked() || busy()) return;
    QSettings settings;
    const auto now = QDateTime::currentSecsSinceEpoch();
    if (now - settings.value("updates/lastCheck", 0).toLongLong() < 86400) return;
    settings.setValue("updates/lastCheck", now); updater_.check(false);
}
void LinuxUpdatesPage::restart() {
    restarting_ = true;
    const auto message = QDBusMessage::createMethodCall("org.fcitx.Fcitx5", "/llavon/update", "org.llavon.IME.Update1", "RestartIfIdle");
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<bool> reply = *watcher; watcher->deleteLater(); restarting_ = false;
        if (reply.isError()) {
            waitingRestart_ = false;
            status_->setText(QStringLiteral("套件已安裝，但目前執行的輸入法尚不支援安全重啟。請完成輸入後登出再登入。"));
        } else if (reply.value()) {
            waitingRestart_ = false; restart_->setVisible(false);
            status_->setText(QStringLiteral("已要求重啟 Fcitx5；請重新開啟設定視窗以載入新版。"));
        } else status_->setText(QStringLiteral("等待所有組字完成；若 Fcitx5 無法自行重啟，請完成輸入後登出再登入。"));
        refresh();
    });
}
} // namespace llavon::lora
