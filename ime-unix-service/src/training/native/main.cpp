#include "manager.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QTimer>
#include <cstdio>
#include <string_view>
#ifdef Q_OS_MACOS
#include "activation_macos.hpp"
#include "host_macos.hpp"
#endif

int main(int argc, char** argv) {
#ifdef Q_OS_MACOS
    if (argc == 3 && std::string_view(argv[1]) == "--host-request") {
        std::puts(llavon::lora::requestMacHost(argv[2]).c_str()); return 0;
    }
#endif
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--help" || std::string_view(argv[index]) == "-h") {
            std::puts("Llavon native settings and personalization\n"
                      "  --state-dir PATH   Training state directory\n"
                      "  --db PATH          Override training database\n"
                      "  --cli PATH         Override manager CLI\n"
                      "  --tables-dir PATH  Override IME tables\n"
                       "  --page PAGE        settings, phrases, records, training, history, updates, about\n"
                      "  --no-browser       Accepted for compatibility (always native)\n");
            return 0;
        }
    }
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("llavon-ime-lora-gui");
    QCoreApplication::setOrganizationName("llavon-ime");
#ifndef Q_OS_MACOS
    QGuiApplication::setDesktopFileName(QStringLiteral("llavon-ime-lora"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/native/AppIcon.png")));
#endif
    QStringList arguments = application.arguments(); arguments.removeFirst();
    QString page = "settings";
    const auto pageIndex = arguments.indexOf("--page");
    if (pageIndex >= 0) {
        if (pageIndex + 1 >= arguments.size()) { std::fputs("--page requires a value\n", stderr); return 2; }
        page = arguments.takeAt(pageIndex + 1); arguments.removeAt(pageIndex);
    }
    QStringList pages{"settings", "phrases", "records", "training", "history", "updates", "about"};
    if (!pages.contains(page)) { std::fputs("unknown settings page\n", stderr); return 2; }
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (!state.isEmpty()) state += "/llavon-ime/training";
    else {
#ifdef Q_OS_MACOS
        state = QDir::homePath() + "/Library/Application Support/llavon-ime/training";
#else
        state = QDir::homePath() + "/.local/state/llavon-ime/training";
#endif
    }
    const auto stateIndex = arguments.indexOf("--state-dir");
    if (stateIndex >= 0 && stateIndex + 1 < arguments.size()) state = arguments[stateIndex + 1];
    state = QDir(state).absolutePath();
    if (!QDir().mkpath(state)) {
        QMessageBox::critical(nullptr, QStringLiteral("無法開啟管理器"), QStringLiteral("無法建立訓練資料目錄：") + state);
        return 1;
    }
    // A per-user local socket only activates the existing window. Data and
    // passwords travel exclusively through the helper's anonymous pipes.
    const auto identity = state + ":" + QDir::homePath();
    const auto name = "llavon-lora-" + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    QLockFile lock(state + "/native-gui.lock");
    if (!lock.tryLock(100)) {
        QLocalSocket socket; socket.connectToServer(name);
        if (socket.waitForConnected(1500)) { socket.write(page.toUtf8() + '\n'); socket.waitForBytesWritten(1000); return 0; }
        QMessageBox::critical(nullptr, QStringLiteral("管理器已開啟"), QStringLiteral("另一個管理器正在啟動，請稍後再試。"));
        return 1;
    }
    QLocalServer::removeServer(name);
    QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(name)) {
        QMessageBox::critical(nullptr, QStringLiteral("無法開啟管理器"), server.errorString()); return 1;
    }
    QString backendPath = QCoreApplication::applicationDirPath() + "/llavon-ime-lora-backend";
    // A macOS bundle contains only the GUI; its CLI and helper live next to it.
    const QDir binaryDir(QCoreApplication::applicationDirPath());
    if (!QFileInfo::exists(backendPath)) backendPath = binaryDir.absoluteFilePath("../../../llavon-ime-lora-backend");
    if (!QFileInfo::exists(backendPath)) backendPath = LLAVON_BUILD_BACKEND;
    const auto cliPath = QFileInfo(backendPath).absolutePath() + "/llavon-ime-lora";
    if (!arguments.contains("--cli") && !QFileInfo::exists(cliPath) && QFileInfo::exists(LLAVON_BUILD_CLI))
        arguments << "--cli" << LLAVON_BUILD_CLI;
    llavon::lora::Backend backend(backendPath, arguments);
    llavon::lora::Manager window(&backend);
    window.showPage(page);
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
        while (auto* socket = server.nextPendingConnection()) {
            auto activate = [socket, &window] {
                if (socket->bytesAvailable() > 128) { socket->abort(); return; }
                if (!socket->canReadLine()) return;
                window.showPage(QString::fromUtf8(socket->readLine()).trimmed());
                window.showNormal();
#ifdef Q_OS_MACOS
                llavon::lora::activateMacApplication();
#endif
                window.raise(); window.activateWindow();
                socket->disconnectFromServer();
            };
            QObject::connect(socket, &QLocalSocket::readyRead, &window, activate);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(2000, socket, &QObject::deleteLater);
            activate();
        }
    });
    window.show();
#ifdef Q_OS_MACOS
    llavon::lora::activateMacApplication();
#endif
    QTimer::singleShot(0, &backend, &llavon::lora::Backend::start);
    return application.exec();
}
