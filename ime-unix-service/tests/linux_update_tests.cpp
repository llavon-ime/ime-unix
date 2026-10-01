#include "linux_updates.hpp"
#include <QDBusObjectPath>
#include <QDBusConnection>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVariant>
#include <memory>

using llavon::lora::LinuxPackageUpdater;

class FakeTransaction final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.PackageKit.Transaction")
public:
    QStringList* calls;
    QStringList* installedIds;
    QStringList packages;
    bool installed = true;
    bool cancelInstall = false;
    bool refreshFails = false;
    qulonglong flags = 0;
    QStringList hints;
    void done(const QString& role, bool failed = false) {
        calls->append(role);
        QTimer::singleShot(10, this, [this, failed] {
            if (failed) emit ErrorCode(1, "test authorization cancelled");
            emit Finished(failed ? 2U : 1U, 0);
        });
    }
public slots:
    void SetHints(const QStringList& values) { hints = values; }
    void Resolve(qulonglong filter, const QStringList& names) {
        flags = filter;
        if (names == QStringList{"llavon-ime-fcitx5"} && installed)
            QTimer::singleShot(0, this, [this] { emit Package(1, "llavon-ime-fcitx5;1.0.0;amd64;installed", "IME"); });
        done("Resolve");
    }
    void RefreshCache(bool) { done("RefreshCache", refreshFails); }
    void GetUpdates(qulonglong) {
        QTimer::singleShot(0, this, [this] {
            for (const auto& id : packages) emit Package(2, id, "test package");
        });
        done("GetUpdates");
    }
    void UpdatePackages(qulonglong transactionFlags, const QStringList& ids) {
        flags = transactionFlags; *installedIds = ids; done("UpdatePackages", cancelInstall);
    }
signals:
    void Package(uint info, const QString& id, const QString& summary);
    void Finished(uint exit, uint runtime);
    void ErrorCode(uint code, const QString& details);
};

class FakePackageKit final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.PackageKit")
public:
    explicit FakePackageKit(QDBusConnection bus) : bus_(std::move(bus)) {}
    QStringList calls, installedIds;
    QStringList packages{"other-app;9.0;amd64;repo", "llavon-ime-fcitx5;1.1.0;amd64;repo"};
    QList<FakeTransaction*> transactions;
    bool installed = true, cancelInstall = false, refreshFails = false;
public slots:
    QDBusObjectPath CreateTransaction() {
        auto* transaction = new FakeTransaction;
        transaction->setParent(this);
        transaction->calls = &calls; transaction->installedIds = &installedIds;
        transaction->packages = packages; transaction->installed = installed;
        transaction->cancelInstall = cancelInstall; transaction->refreshFails = refreshFails;
        transactions.append(transaction);
        const auto path = QStringLiteral("/transaction/%1").arg(transactions.size());
        if (!bus_.registerObject(path, transaction, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) qFatal("cannot export transaction");
        return QDBusObjectPath(path);
    }
private:
    QDBusConnection bus_;
};

class LinuxUpdateTests final : public QObject {
    Q_OBJECT
private slots:
    void packageKitLifecycle() {
        const auto address = qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS");
        auto server = QDBusConnection::connectToBus(address, "update-test-server");
        auto client = QDBusConnection::connectToBus(address, "update-test-client");
        QVERIFY(server.isConnected()); QVERIFY(client.isConnected());
        QVERIFY(server.registerService("org.freedesktop.PackageKit"));
        FakePackageKit kit(server);
        QVERIFY(server.registerObject("/org/freedesktop/PackageKit", &kit, QDBusConnection::ExportAllSlots));
        LinuxPackageUpdater updater(client);
        QSignalSpy status(&updater, &LinuxPackageUpdater::status);
        QSignalSpy installed(&updater, &LinuxPackageUpdater::installed);
        updater.check(); updater.check(); // A duplicate click must not queue another transaction.
        QTRY_VERIFY_WITH_TIMEOUT(!updater.busy(), 5000);
        QCOMPARE(kit.calls, (QStringList{"Resolve", "RefreshCache", "GetUpdates"}));
        QCOMPARE(kit.transactions[0]->flags, qulonglong{1ULL << 2});
        QCOMPARE(kit.transactions[0]->hints, QStringList{"interactive=true"});
        QCOMPARE(updater.candidate(), QString("llavon-ime-fcitx5;1.1.0;amd64;repo"));
        updater.install();
        QTRY_COMPARE(installed.count(), 1);
        QCOMPARE(kit.installedIds, QStringList{"llavon-ime-fcitx5;1.1.0;amd64;repo"});
        QCOMPARE(kit.transactions.last()->flags, qulonglong{1ULL << 1});
        QVERIFY(updater.candidate().isEmpty());

        kit.cancelInstall = true;
        updater.check(); QTRY_VERIFY(!updater.busy());
        updater.install(); QTRY_VERIFY(!updater.busy());
        QCOMPARE(installed.count(), 1); // Failed authorization must never trigger a restart.
        QVERIFY(status.last().first().toString().contains("cancelled"));

        kit.installed = false; kit.calls.clear();
        updater.check(); QTRY_VERIFY(!updater.busy());
        QCOMPARE(kit.calls, QStringList{"Resolve"});
        QVERIFY(updater.candidate().isEmpty());

        kit.installed = true; kit.refreshFails = true; kit.calls.clear();
        updater.check(); QTRY_VERIFY(!updater.busy());
        QCOMPARE(kit.calls, (QStringList{"Resolve", "RefreshCache"}));
        QVERIFY(updater.candidate().isEmpty());

        kit.refreshFails = false;
        kit.packages = {"llavon-ime-fcitx5-git;2.0;amd64;aur", "unrelated;3.0;amd64;repo"};
        updater.check(); QTRY_VERIFY(!updater.busy());
        QVERIFY(updater.candidate().isEmpty());
        const auto count = kit.calls.size(); updater.install(); QCOMPARE(kit.calls.size(), count);

        kit.packages = {"llavon-ime-fcitx5;1.1;amd64;repo", "llavon-ime-fcitx5;1.1;arm64;repo"};
        updater.check(false); QTRY_VERIFY(!updater.busy());
        QCOMPARE(kit.transactions.last()->hints, QStringList{"interactive=false"});
        QVERIFY(updater.candidate().isEmpty());
        server.unregisterService("org.freedesktop.PackageKit");
    }
};
QTEST_GUILESS_MAIN(LinuxUpdateTests)
#include "linux_update_tests.moc"
