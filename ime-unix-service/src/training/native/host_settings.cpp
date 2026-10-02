#include "host_settings.hpp"
#include "controls.hpp"
#include <QCheckBox>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#ifndef Q_OS_MACOS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace llavon::lora {
HostSettingsPage::HostSettingsPage(bool updates, QWidget* parent, QString helper)
    : QWidget(parent), helper_(helper.isEmpty() ? QCoreApplication::applicationFilePath() : std::move(helper)), updates_(updates) {
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(18);
    version_ = new QLabel; version_->setObjectName("hostVersion"); version_->setTextFormat(Qt::PlainText); layout->addWidget(version_);
    context_ = new QLabel; context_->setObjectName("hostContext"); context_->setWordWrap(true); context_->setTextFormat(Qt::PlainText); layout->addWidget(context_);
    status_ = new QLabel; status_->setObjectName("hostStatus"); status_->setWordWrap(true); status_->setTextFormat(Qt::PlainText);
    if (updates_) {
        checks_ = new NativeCheckBox(QStringLiteral("自動檢查更新（每日）")); checks_->setObjectName("automaticUpdates"); layout->addWidget(checks_);
        downloads_ = new NativeCheckBox(QStringLiteral("背景下載更新")); downloads_->setObjectName("automaticDownloads"); layout->addWidget(downloads_);
        auto* note = new QLabel(QStringLiteral("背景下載不會自動安裝。下載就緒後，會在組字與訓練結束時顯示安裝提示。\n設定頁也會顯示「已下載」，按鈕會改為「安裝並重新啟動…」，可再次開啟提示。\n確認並提供管理員授權後才會安裝，並重新啟動輸入法。"));
        note->setObjectName("updateInstructions"); note->setProperty("role", "muted"); note->setWordWrap(true); layout->addWidget(note);
        check_ = new QPushButton(QStringLiteral("檢查更新…")); check_->setObjectName("checkUpdates"); check_->setProperty("primary", true); layout->addWidget(check_);
        check_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        connect(checks_, &QCheckBox::toggled, this, [this](bool value) { request({{"action", "preferences"}, {"checks", value}}); });
        connect(downloads_, &QCheckBox::toggled, this, [this](bool value) { request({{"action", "preferences"}, {"downloads", value}}); });
        connect(check_, &QPushButton::clicked, this, [this] { request({{"action", "check"}}); });
    }
    layout->addWidget(status_); layout->addStretch();
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int exit, QProcess::ExitStatus status) {
        watchdog_.stop();
        const auto state = QJsonDocument::fromJson(process_.readAllStandardOutput()).object();
        if (exit != 0 || status != QProcess::NormalExit || state.isEmpty()) render({{"error", QStringLiteral("輸入法通訊未完成，請稍後重試。")}});
        else render(state);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        watchdog_.stop();
        render({{"error", QStringLiteral("無法連接輸入法：") + process_.errorString()}});
    });
    watchdog_.setSingleShot(true); watchdog_.setInterval(4000);
    connect(&watchdog_, &QTimer::timeout, &process_, &QProcess::kill);
    timer_.setInterval(3000); connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); }); timer_.start();
    render({{"error", QStringLiteral("等待連接輸入法…")}});
    QTimer::singleShot(0, this, [this] { if (isVisible()) refresh(); });
}

void HostSettingsPage::refresh() { request({{"action", "status"}}); }
void HostSettingsPage::request(const QJsonObject& request) {
#ifdef Q_OS_MACOS
    if (process_.state() != QProcess::NotRunning) return;
    if (checks_) { checks_->setEnabled(false); downloads_->setEnabled(false); check_->setEnabled(false); }
    process_.start(helper_, {"--host-request", QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))});
    watchdog_.start();
#else
    Q_UNUSED(request)
    const auto message = QDBusMessage::createMethodCall("org.fcitx.Fcitx5", "/llavon/update", "org.llavon.IME.Update1", "Status");
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QString> reply = *watcher; watcher->deleteLater();
        render(reply.isError() ? QJsonObject{{"error", QStringLiteral("尚未連接拉風輸入法，請先在 Fcitx5 啟用。")}} : QJsonDocument::fromJson(reply.value().toUtf8()).object());
    });
#endif
}
void HostSettingsPage::render(const QJsonObject& state) {
    const bool connected = !state.contains("error");
    version_->setText(connected ? QStringLiteral("目前執行版本：%1").arg(state.value("version").toString()) : QStringLiteral("目前執行版本：未連接"));
    context_->setText(state.value("context").toString());
    status_->setText(connected ? state.value("status").toString() : state.value("error").toString());
    if (checks_) {
        const QSignalBlocker a(checks_), b(downloads_);
        checks_->setChecked(state.value("checks").toBool()); downloads_->setChecked(state.value("downloads").toBool());
        checks_->setEnabled(connected && state.value("enabled").toBool());
        downloads_->setEnabled(connected && state.value("allowsDownloads").toBool());
        check_->setEnabled(connected && state.value("canCheck").toBool());
        check_->setText(state.value("readyToInstall").toBool() ? QStringLiteral("安裝並重新啟動…") : QStringLiteral("檢查更新…"));
    }
}
}
