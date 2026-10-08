#include "manager.hpp"
#include "history_graph.hpp"
#include "settings.hpp"
#include "phrase_table.hpp"
#include "phrase_list.hpp"
#include "host_settings.hpp"
#include "controls.hpp"
#include "appearance.hpp"
#include "guide.hpp"
#include "guide_illustration.hpp"
#include <QWheelEvent>
#include <QAbstractItemView>
#include <QVBoxLayout>
#include <QScrollBar>
#include "config/config.hpp"
#include "phrase_override/phrase_override_store.hpp"
#include "training/commit_store.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QStackedWidget>
#include <QPlainTextEdit>
#include <QCryptographicHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <sqlite3.h>
#include <memory>

using llavon::lora::Backend;
using llavon::lora::Manager;

class NativeTests final : public QObject {
    Q_OBJECT
private slots:
    void encryptedReviewAndActions();
    void jobsAndErrors();
    void jobLogFollowing();
    void realTraining();
    void historyGraphInteractions();
    void unifiedSettings();
    void pageLaunchRouting();
    void deferredPhrasePage();
    void settingsWithoutTrainingBackend();
    void deferredSettingGroups();
    void featureGuide();
    void guideIllustrations();
    void guideDetailTopics();
    void quietInterface();
    void phraseEmptyDiscovery();
    void phraseLookup();
    void appearanceChanges();
    void hostSettingsMigration();
    void controlInteractions();
};

namespace {
class Environment final {
public:
    Environment(const char* name, const QByteArray& value) : name_(name), old_(qgetenv(name)), existed_(qEnvironmentVariableIsSet(name)) { qputenv(name, value); }
    ~Environment() { if (existed_) qputenv(name_.constData(), old_); else qunsetenv(name_.constData()); }
private:
    QByteArray name_, old_;
    bool existed_;
};
template<class Widget>
Widget* widget(Manager& window, const char* name) {
    auto* result = window.findChild<Widget*>(name);
    if (!result) qFatal("Missing widget: %s", name);
    return result;
}
void click(Manager& window, const char* name) { QTest::mouseClick(widget<QPushButton>(window, name), Qt::LeftButton); }
QFrame* phraseRow(QWidget& window, int index) {
    for (auto* row : window.findChildren<QFrame*>("phraseRow"))
        if (row->property("phraseIndex").toInt() == index) return row;
    qFatal("Missing phrase row %d", index);
}
void capture(QWidget& window, const QString& name) {
    const auto directory = qEnvironmentVariable("LLAVON_UI_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    QTest::qWait(150);
    if (!window.grab().save(directory + "/" + name + ".png")) qFatal("Cannot save UI capture");
}
}

void NativeTests::encryptedReviewAndActions() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment assets("LLAVON_IME_LORA_ASSETS_DIR", (directory.path() + "/assets").toUtf8());
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path(), "--cli", LLAVON_TEST_CLI});
    Manager window(&backend); window.show();
    QSignalSpy ready(&window, &Manager::refreshed);
    QSignalSpy errors(&backend, &Backend::error);
    backend.start();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    capture(window, "01-first-run");
    widget<QLineEdit>(window, "reviewPassword")->setText("test-password");
    widget<QLineEdit>(window, "confirmation")->setText("different");
    click(window, "setup");
    QTRY_VERIFY(errors.count() == 1);
    QVERIFY(errors.last().first().toString().contains(QStringLiteral("不同")));
    widget<QLineEdit>(window, "reviewPassword")->setText("test-password");
    widget<QLineEdit>(window, "confirmation")->setText("test-password");
    click(window, "setup");
    QTRY_VERIFY_WITH_TIMEOUT(widget<QPushButton>(window, "unlock")->isVisible(), 10000);
    using namespace ime::unix_service;
    {
        CommitStore store((directory.path() + "/commits.sqlite3").toStdString());
        protocol::RecordCommitRequest request;
        request.source_id[0] = 0x31;
        const QList<std::u16string> answers{u"你好", u"早安", u"工作", u"明天"};
        const QList<std::vector<protocol::CommitEntry>> readings{
            {{u"ㄋㄧˇ", U'你', false}, {u"ㄏㄠˇ", U'好', true}},
            {{u"ㄗㄠˇ", U'早', true}, {u"ㄢ", U'安', false}},
            {{u"ㄍㄨㄥ", U'工', false}, {u"ㄗㄨㄛˋ", U'作', false}},
            {{u"ㄇㄧㄥˊ", U'明', true}, {u"ㄊㄧㄢ", U'天', false}}};
        for (int i = 0; i < 24; ++i) {
            request.event_id[0] = static_cast<std::uint8_t>(i + 1);
            request.source_id[0] = static_cast<std::uint8_t>(i + 1);
            request.context = u"這是原生介面驗證用的測試資料。";
            request.answer = answers[i % 4]; request.entries = readings[i % 4];
            QVERIFY(store.record(request));
        }
        QCOMPARE(store.flush_staged(true), std::size_t(24));
    }
    window.refresh();
    auto* table = widget<QTableWidget>(window, "records");
    QTRY_COMPARE(table->rowCount(), 20);
    QVERIFY(table->item(0, 1)->text().contains(QStringLiteral("加密")));
    capture(window, "02-locked");
    widget<QLineEdit>(window, "reviewPassword")->setText("wrong-password"); click(window, "unlock");
    QTRY_COMPARE(errors.count(), 2);
    QVERIFY(table->item(0, 1)->text().contains(QStringLiteral("加密")));
    widget<QLineEdit>(window, "reviewPassword")->setText("test-password"); click(window, "unlock");
    QTRY_VERIFY_WITH_TIMEOUT(!table->item(0, 1)->text().contains(QStringLiteral("加密")), 10000);
    capture(window, "03-records");
    click(window, "next"); QTRY_COMPARE(table->rowCount(), 4);
    click(window, "previous"); QTRY_COMPARE(table->rowCount(), 20);
    widget<QCheckBox>(window, "manualView")->setChecked(true);
    QTRY_COMPARE(table->rowCount(), 18);
    widget<QCheckBox>(window, "manualView")->setChecked(false);
    QTRY_COMPARE(table->rowCount(), 20);
    table->selectRow(0); QVERIFY(widget<QPushButton>(window, "exclude")->isEnabled());
    const auto removedId = table->item(0, 0)->data(Qt::UserRole).toString();
    click(window, "exclude");
    QTRY_VERIFY(table->item(0, 0)->data(Qt::UserRole).toString() != removedId);
    widget<QComboBox>(window, "recordFilter")->setCurrentIndex(2);
    QTRY_COMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toString(), removedId);
    table->selectRow(0); QVERIFY(!widget<QPushButton>(window, "delete")->isEnabled());
    widget<QComboBox>(window, "recordFilter")->setCurrentIndex(0);
    QTRY_COMPARE(table->rowCount(), 20);
    click(window, "lock");
    QTRY_VERIFY(table->item(0, 1)->text().contains(QStringLiteral("加密")));
    click(window, "recording");
    QTRY_COMPARE(widget<QPushButton>(window, "recording")->text(), QStringLiteral("恢復收集"));
    click(window, "recording");
    QTRY_COMPARE(widget<QPushButton>(window, "recording")->text(), QStringLiteral("暫停收集"));
    table->selectRow(0);
    const auto deletedId = table->item(0, 0)->data(Qt::UserRole).toString();
    QTimer::singleShot(100, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) qFatal("Delete confirmation did not open");
        dialog->button(QMessageBox::Yes)->click();
    });
    click(window, "delete");
    QTRY_VERIFY(table->item(0, 0)->data(Qt::UserRole).toString() != deletedId);
    window.showPage("training");
    capture(window, "04-training");
    QVERIFY(!widget<QPushButton>(window, "startTraining")->isEnabled());
    widget<QComboBox>(window, "strength")->setCurrentIndex(4);
    QVERIFY(widget<QLineEdit>(window, "rank")->isVisible());
    widget<QComboBox>(window, "strength")->setCurrentIndex(0);
    window.showPage("history");
    capture(window, "05-history-empty");
    QCOMPARE(widget<llavon::lora::HistoryGraph>(window, "history")->nodeCount(), 1);
    bool forgot = false;
    backend.request("/api/protection", {{"action", "forget"}}, [&forgot](const QJsonValue& response) {
        forgot = response.toObject().value("ok").toBool();
    }, true);
    QTRY_VERIFY(forgot);
    window.refresh();
    QTRY_COMPARE(table->rowCount(), 0);
    QCOMPARE(errors.count(), 2);
}

void NativeTests::jobsAndErrors() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment assets("LLAVON_IME_LORA_ASSETS_DIR", (directory.path() + "/assets").toUtf8());
    // A controlled external CLI tests process-group cancellation and reporting.
    // No downloads or GPU workloads are needed to exercise the real job runner.
    const auto cli = directory.path() + "/cli";
    QFile file(cli); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\ncase \"$1\" in\nfetch-model) echo downloading; sleep 30;;\ncheck-model) echo update-available=false;;\n*) echo deliberate-test-error >&2; exit 7;;\nesac\n");
    file.close(); QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path(), "--cli", cli});
    Manager window(&backend); window.show();
    QSignalSpy ready(&window, &Manager::refreshed); QSignalSpy errors(&backend, &Backend::error);
    backend.start(); QTRY_VERIFY(ready.count() > 0);
    window.showPage("training");
    click(window, "fetch"); QTRY_VERIFY(widget<QPushButton>(window, "cancelJob")->isEnabled());
    QVERIFY(!widget<QPushButton>(window, "fetch")->isEnabled());
    capture(window, "06-download-running");
    click(window, "cancelJob");
    QTRY_VERIFY_WITH_TIMEOUT(!widget<QPushButton>(window, "cancelJob")->isEnabled(), 10000);
    click(window, "installTrainer");
    QTRY_VERIFY_WITH_TIMEOUT(widget<QLabel>(window, "notice")->text().contains(QStringLiteral("失敗")), 10000);
    capture(window, "07-job-failed");
    backend.request("/api/use-model", {{"id", "invalid"}}, {}, true);
    QTRY_COMPARE(errors.count(), 1);
    QVERIFY(errors.last().first().toString().contains("invalid"));
}

void NativeTests::jobLogFollowing() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment assets("LLAVON_IME_LORA_ASSETS_DIR", (directory.path() + "/assets").toUtf8());
    const auto cli = directory.path() + "/streaming-cli";
    QFile executable(cli); QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write(QStringLiteral("#!/bin/sh\n"
        "[ \"$1\" = fetch-model ] || exit 0\n"
        "i=1; while [ $i -le 120 ]; do echo \"step-$i\"; i=$((i + 1)); done\n"
        "while [ ! -f '%1/continue1' ]; do sleep 0.05; done\n"
        "echo step-121\n"
        "while [ ! -f '%1/continue2' ]; do sleep 0.05; done\n"
        "echo step-122\n").arg(directory.path()).toUtf8());
    executable.close(); QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path(), "--cli", cli});
    Manager window(&backend); window.show();
    QSignalSpy ready(&window, &Manager::refreshed); backend.start(); QTRY_VERIFY(ready.count() > 0);
    window.showPage("training"); click(window, "fetch");
    auto* log = widget<QPlainTextEdit>(window, "jobLog"); auto* scroll = log->verticalScrollBar();
    QTRY_VERIFY_WITH_TIMEOUT(log->toPlainText().contains("step-120"), 10000);
    QVERIFY(scroll->maximum() > 0); QTRY_COMPARE(scroll->value(), scroll->maximum());
    // Reviewing/copying an earlier line must not be pulled to the newest output.
    QTextCursor selection(log->document()); selection.setPosition(0); selection.setPosition(6, QTextCursor::KeepAnchor);
    log->setTextCursor(selection); scroll->setValue(0);
    const auto selected = log->textCursor().selectedText();
    QFile first(directory.path() + "/continue1"); QVERIFY(first.open(QIODevice::WriteOnly)); first.close();
    window.refresh(); QTRY_VERIFY_WITH_TIMEOUT(log->toPlainText().contains("step-121"), 10000);
    QCOMPARE(scroll->value(), 0); QCOMPARE(log->textCursor().selectedText(), selected);
    // Returning to the bottom resumes following, including the completion line.
    scroll->setValue(scroll->maximum());
    QFile second(directory.path() + "/continue2"); QVERIFY(second.open(QIODevice::WriteOnly)); second.close();
    window.refresh(); QTRY_VERIFY_WITH_TIMEOUT(log->toPlainText().contains("step-122"), 10000);
    QTRY_COMPARE(scroll->value(), scroll->maximum());
    QTRY_VERIFY_WITH_TIMEOUT(!widget<QPushButton>(window, "cancelJob")->isEnabled(), 10000);
}

void NativeTests::realTraining() {
    const auto source = qEnvironmentVariable("LLAVON_UI_REAL_ASSETS");
    const auto trainer = qEnvironmentVariable("LLAVON_UI_REAL_TRAINER");
    if (source.isEmpty() || trainer.isEmpty()) QSKIP("Set LLAVON_UI_REAL_ASSETS and LLAVON_UI_REAL_TRAINER to opt in to a real one-step training run");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto assetsPath = directory.path() + "/assets";
    QVERIFY(QDir().mkpath(assetsPath));
    QFile revisionFile(source + "/current.revision"); QVERIFY(revisionFile.open(QIODevice::ReadOnly));
    const auto revision = QString::fromUtf8(revisionFile.readAll()).trimmed(); QCOMPARE(revision.size(), 40);
    QVERIFY(QFile::copy(source + "/current.revision", assetsPath + "/current.revision"));
    QVERIFY(QFile::link(source + "/" + revision, assetsPath + "/" + revision));
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment assets("LLAVON_IME_LORA_ASSETS_DIR", assetsPath.toUtf8());
    Environment trainerOverride("LLAVON_IME_LORA_CLI_PATH", trainer.toUtf8());
    {
        using namespace ime::unix_service;
        CommitStore store((directory.path() + "/commits.sqlite3").toStdString());
        store.configure_password("test-password");
        protocol::RecordCommitRequest request;
        request.event_id[0] = 1; request.source_id[0] = 1;
        request.context = u"早安"; request.answer = u"你好";
        request.entries = {{u"ㄋㄧˇ", U'你', false}, {u"ㄏㄠˇ", U'好', true}};
        QVERIFY(store.record(request)); QCOMPARE(store.flush_staged(true), std::size_t(1));
    }
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path(), "--cli", LLAVON_TEST_CLI, "--tables-dir", LLAVON_TEST_TABLES});
    Manager window(&backend); window.show();
    QSignalSpy ready(&window, &Manager::refreshed); QSignalSpy errors(&backend, &Backend::error);
    backend.start(); QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    window.showPage("training");
    QVERIFY(widget<QPushButton>(window, "startTraining")->isEnabled());
    widget<QComboBox>(window, "strength")->setCurrentIndex(4);
    widget<QLineEdit>(window, "max-steps")->setText("1");
    widget<QLineEdit>(window, "max-seq-length")->setText("32");
    widget<QLineEdit>(window, "device")->setText("cpu");
    widget<QCheckBox>(window, "stabilize")->setChecked(false);
    widget<QLineEdit>(window, "trainPassword")->setText("test-password");
    // The count is computed through the real CLI before this confirmation.
    QTimer confirmation;
    confirmation.setInterval(50);
    connect(&confirmation, &QTimer::timeout, this, [&confirmation] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (dialog) { confirmation.stop(); dialog->button(QMessageBox::Yes)->click(); }
    });
    confirmation.start(); click(window, "startTraining");
    QTRY_VERIFY_WITH_TIMEOUT(widget<QPushButton>(window, "cancelJob")->isEnabled(), 20000);
    capture(window, "08-real-training-running");
    QTRY_VERIFY_WITH_TIMEOUT(!widget<QPushButton>(window, "cancelJob")->isEnabled(), 240000);
    QCOMPARE(errors.count(), 0);
    QVERIFY2(widget<QLabel>(window, "notice")->text().contains(QStringLiteral("已完成")), qPrintable(widget<QLabel>(window, "notice")->text()));
    window.showPage("history");
    auto* history = widget<llavon::lora::HistoryGraph>(window, "history");
    QTRY_COMPARE(history->nodeCount(), 2);
    QString runId;
    for (const auto* item : history->scene()->items())
        if (item->data(0).isValid() && item->data(0).toString() != "base") runId = item->data(0).toString();
    QVERIFY(!runId.isEmpty()); history->selectRun(runId);
    capture(window, "09-real-training-history");
    click(window, "setBase");
    QCOMPARE(widget<QComboBox>(window, "trainingBase")->currentData().toString(), runId);
    window.showPage("history");
    click(window, "applyModel");
    bool applied = false;
    backend.request("/api/state", {}, [&applied](const QJsonValue& response) { applied = !response.toObject().value("active_model_path").toString().isEmpty(); });
    QTRY_VERIFY(applied);
    history->selectRun("base"); click(window, "applyModel");
    bool reverted = false;
    backend.request("/api/state", {}, [&reverted](const QJsonValue& response) { reverted = response.toObject().value("active_model_path").toString().isEmpty(); });
    QTRY_VERIFY(reverted);
    QCOMPARE(errors.count(), 0);
}

void NativeTests::historyGraphInteractions() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment assets("LLAVON_IME_LORA_ASSETS_DIR", (directory.path() + "/assets").toUtf8());
    // A branched history fixture goes through the actual backend/database.
    // No user's records, models or settings are involved.
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open((directory.path() + "/commits.sqlite3").toUtf8().constData(), &db), SQLITE_OK);
    const char* sql = R"SQL(
        CREATE TABLE lora_runs(id INTEGER PRIMARY KEY, completed_at TEXT, record_count INTEGER,
          model_path TEXT, optimizer_steps INTEGER, rank INTEGER, alpha INTEGER, dropout REAL,
          target_modules TEXT, cumulative_record_count INTEGER, parent_id INTEGER, training_request_json TEXT);
        INSERT INTO lora_runs VALUES
          (1,'2026-09-27T09:20:00Z',24,'/test/1.gguf',72,8,16,0,'q_proj,v_proj',24,0,'{"strength":"ultra-low"}'),
          (2,'2026-09-28T08:45:00Z',18,'/test/2.gguf',54,8,16,0,'q_proj,v_proj',42,1,'{"strength":"low"}'),
          (3,'2026-09-28T12:30:00Z',32,'/test/3.gguf',96,8,16,0,'q_proj,v_proj',56,1,'{"strength":"medium"}'),
          (4,'2026-09-29T10:10:00Z',12,'/test/4.gguf',36,8,16,0,'q_proj,v_proj',54,2,'{"strength":"low"}'),
          (5,'2026-09-30T09:00:00Z',20,'/test/5.gguf',60,8,16,0,'q_proj,v_proj',76,3,'{"strength":"high"}');
    )SQL";
    const int status = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db); QCOMPARE(status, SQLITE_OK);
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path(), "--cli", LLAVON_TEST_CLI});
    Manager window(&backend); window.resize(1180, 900); window.show();
    QSignalSpy ready(&window, &Manager::refreshed);
    backend.start(); QTRY_VERIFY(ready.count() > 0);
    window.showPage("history");
    auto* graph = widget<llavon::lora::HistoryGraph>(window, "history");
    QTRY_COMPARE(graph->nodeCount(), 6);
    const auto node = [graph](const QString& id) -> QGraphicsItem* {
        for (auto* item : graph->scene()->items()) if (item->data(0).toString() == id) return item;
        return nullptr;
    };
    QVERIFY(node("1")); QVERIFY(node("2")); QVERIFY(node("3"));
    QVERIFY(node("2")->pos().x() < node("3")->pos().x());
    QVERIFY(node("2")->pos().y() > node("1")->pos().y());
    int edges = 0; for (auto* item : graph->scene()->items()) if (dynamic_cast<QGraphicsPathItem*>(item)) ++edges;
    QCOMPARE(edges, 5);
    const auto point = graph->mapFromScene(node("3")->pos() + QPointF(100, 50));
    QTest::mouseClick(graph->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(graph->selectedId(), QString("3"));
    click(window, "setBase"); QCOMPARE(widget<QComboBox>(window, "trainingBase")->currentData().toString(), QString("3"));
    window.showPage("history");
    click(window, "ancestor"); QCOMPARE(graph->selectedId(), QString("1"));
    const auto drag = [graph](QPoint from, QPoint to) {
        QTest::mousePress(graph->viewport(), Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(graph->viewport()->mapToGlobal(to)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(graph->viewport(), &move);
        QTest::mouseRelease(graph->viewport(), Qt::LeftButton, Qt::NoModifier, to);
    };
    const auto oldPosition = node("3")->pos();
    QList<QPainterPath> oldPaths;
    for (auto* item : graph->scene()->items()) if (auto* edge = dynamic_cast<QGraphicsPathItem*>(item)) oldPaths.append(edge->path());
    drag(point, point + QPoint(54, 24));
    QVERIFY(node("3")->pos() != oldPosition);
    QList<QPainterPath> newPaths;
    for (auto* item : graph->scene()->items()) if (auto* edge = dynamic_cast<QGraphicsPathItem*>(item)) newPaths.append(edge->path());
    QVERIFY(newPaths != oldPaths);
    const auto movedPosition = node("3")->pos();
    const auto center = graph->mapToScene(graph->viewport()->rect().center());
    drag(QPoint(20, 20), QPoint(85, 48));
    QVERIFY(graph->mapToScene(graph->viewport()->rect().center()) != center);
    const auto scale = graph->transform().m11();
    click(window, "zoomIn"); QVERIFY(graph->transform().m11() > scale);
    const auto beforeWheel = graph->transform().m11();
    const QPoint wheelPoint(100, 100);
    QWheelEvent wheel(QPointF(wheelPoint), QPointF(graph->viewport()->mapToGlobal(wheelPoint)),
                      QPoint(), QPoint(0, -120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(graph->viewport(), &wheel);
    QVERIFY(graph->transform().m11() < beforeWheel);
    const auto currentScale = graph->transform().m11();
    const auto currentCenter = graph->mapToScene(graph->viewport()->rect().center());
    // Changing the applied model rebuilds node badges without losing manual
    // positions, zoom, selection or the user's current canvas location.
#ifdef Q_OS_MACOS
    const auto configPath = directory.path() + "/config/llavon-ime/config.json";
    const QByteArray configText = "{\"model_path\":\"/test/2.gguf\"}";
#else
    const auto configPath = directory.path() + "/config/fcitx5/conf/llavon-ime.conf";
    const QByteArray configText = "ModelPath=/test/2.gguf\n";
#endif
    QVERIFY(QDir().mkpath(QFileInfo(configPath).absolutePath()));
    QFile appliedConfig(configPath); QVERIFY(appliedConfig.open(QIODevice::WriteOnly));
    appliedConfig.write(configText); appliedConfig.close();
    ready.clear(); window.refresh(); QTRY_VERIFY(ready.count() > 0);
    QCOMPARE(node("3")->pos(), movedPosition);
    QCOMPARE(graph->transform().m11(), currentScale);
    QCOMPARE(graph->mapToScene(graph->viewport()->rect().center()), currentCenter);
    graph->zoomBy(100); QCOMPARE(graph->transform().m11(), 2.5);
    graph->zoomBy(0.001); QVERIFY(qAbs(graph->transform().m11() - 0.6) < 0.001);
    click(window, "arrangeGraph"); QCOMPARE(node("3")->pos(), oldPosition);
    graph->selectRun("3");
    QTest::keyClick(graph, Qt::Key_Right); QCOMPARE(node("3")->pos(), oldPosition + QPointF(10, 0));
    click(window, "arrangeGraph");
    graph->selectRun("3");
    capture(window, "10-history-node-graph");
    window.resize(800, 620); QTest::qWait(100); click(window, "resetGraph");
    capture(window, "11-history-compact");
}

void NativeTests::unifiedSettings() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    using llavon::lora::SettingsStore;
    SettingsStore store;
    auto values = store.load();
    QCOMPARE(values.size(), SettingsStore::schema().value("fields").toArray().size());
    values["candidate_page_size"] = 7;
    values["keyboard_layout"] = "hsu";
    values["space_selects_candidate"] = false;
    values["model_path"] = QStringLiteral("/tmp/測試 model \\\".gguf");
    store.save(values);
    QCOMPARE(store.load(), values);
    auto invalid = values; invalid["candidate_page_size"] = 0;
    QVERIFY_EXCEPTION_THROWN(store.save(invalid), std::runtime_error);
    QCOMPARE(store.load(), values);
    // Host-owned/unknown fields must survive a round trip. Stale snapshots must
    // not overwrite a newer model chosen from the training page or elsewhere.
    QFile file(SettingsStore::configPath()); QVERIFY(file.open(QIODevice::ReadOnly));
    auto bytes = file.readAll(); file.close();
#ifdef Q_OS_MACOS
    auto object = QJsonDocument::fromJson(bytes).object(); object["future_setting"] = "keep";
    bytes = QJsonDocument(object).toJson();
#else
    bytes += "FutureSetting=keep\n";
#endif
    QVERIFY(file.open(QIODevice::WriteOnly)); file.write(bytes); file.close();
    QVERIFY_EXCEPTION_THROWN(store.save(values), std::runtime_error);
    values = store.load(); store.save(values);
    QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.readAll().contains("keep")); file.close();
    store.loadPhrases(); store.savePhrases(QStringLiteral("# 測試\n拉風 ㄌㄚ-ㄈㄥ\n"));
    llavon::ime::PhraseOverrideStore hostPhrases(SettingsStore::phrasesPath().toStdString());
    QVERIFY(hostPhrases.load()); QCOMPARE(hostPhrases.entries().size(), std::size_t(1));
    QVERIFY_EXCEPTION_THROWN(store.savePhrases(QStringLiteral("不合法")), std::runtime_error);
    QVERIFY(store.loadPhrases().contains(QStringLiteral("拉風")));
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path() + "/training", "--cli", LLAVON_TEST_CLI});
    Manager window(&backend); window.show(); backend.start();
    window.showPage("settings");
    QCOMPARE(widget<QListWidget>(window, "navigation")->currentRow(), 0);
    auto* groups = widget<QTabWidget>(window, "settingsGroups");
    for (int index = 0; index < groups->count(); ++index) groups->setCurrentIndex(index);
    for (const auto item : SettingsStore::schema().value("fields").toArray()) {
        const auto field = item.toObject();
        QVERIFY2(window.findChild<QWidget*>("setting_" + field.value("key").toString()), qPrintable(field.value("key").toString()));
    }
    auto* count = widget<QSpinBox>(window, "setting_candidate_page_size"); QCOMPARE(count->value(), 7);
    count->setValue(9);
    window.showPage("history"); window.showPage("settings"); QCOMPARE(count->value(), 9);
    click(window, "saveSettings"); QCOMPARE(store.load().value("candidate_page_size").toInt(), 9);
    groups->setCurrentIndex(1); capture(window, "12-unified-settings");
    groups->setCurrentIndex(0); capture(window, "13-runtime-settings");
    window.showPage("phrases");
    click(window, "addPhrase");
    auto* row = phraseRow(window, 1); row->findChild<QLineEdit*>("phraseWord")->setText(QStringLiteral("你好"));
    auto* reading = row->findChild<QComboBox*>("phraseReading_1"); reading->setCurrentIndex(reading->findData(QStringLiteral("ㄏㄠˇ")));
    click(window, "savePhrases"); QVERIFY(hostPhrases.load()); QCOMPARE(hostPhrases.entries().size(), std::size_t(2));
    capture(window, "14-phrase-settings");
    QFile external(SettingsStore::phrasesPath()); QVERIFY(external.open(QIODevice::Append));
    external.write(QStringLiteral("# 外部編輯\n").toUtf8()); external.close();
    click(window, "reloadPhrases");
    QVERIFY(widget<llavon::lora::PhraseList>(window, "phraseList")->text().contains(QStringLiteral("外部編輯")));
    QTRY_VERIFY_WITH_TIMEOUT(widget<QLabel>(window, "phraseStatus")->text().startsWith(QStringLiteral("已重新載入")), 5000);
    QVERIFY(QFile::remove(SettingsStore::phrasesPath())); QVERIFY(QDir().mkpath(SettingsStore::phrasesPath()));
    click(window, "reloadPhrases");
    QVERIFY(!widget<QLabel>(window, "phraseStatus")->text().startsWith(QStringLiteral("已")));
}

void NativeTests::phraseLookup() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    const QString tablePath = QStringLiteral(LLAVON_TEST_TABLES "/bopomofo_char.json");
    llavon::lora::PhraseTable table(tablePath);
    QVERIFY2(table.error().isEmpty(), qPrintable(table.error()));
    QVERIFY(table.readings(QStringLiteral("行")).contains(QStringLiteral("ㄏㄤˊ")));
    QVERIFY(table.readings(QStringLiteral("行")).contains(QStringLiteral("ㄒㄧㄥˊ")));
    QVERIFY(table.check(QStringLiteral("拉風 ㄌㄚ-ㄈㄥ\n銀行 ㄧㄣˊ ㄏㄤˊ\n")).isEmpty());
    const auto mismatch = table.check(QStringLiteral("# 註解\n銀行 ㄧㄣˊ-ㄋㄧˇ"));
    QCOMPARE(mismatch.size(), 1); QVERIFY(mismatch.front().contains(QStringLiteral("第 2 行")));
    QVERIFY(mismatch.front().contains(QStringLiteral("行"))); QVERIFY(mismatch.front().contains(QStringLiteral("ㄏㄤˊ")));
    QVERIFY(!table.check(QStringLiteral("拉風 ㄌㄚ-ㄈㄥ\n拉風 ㄌㄚ ㄈㄥ")).isEmpty());
    QVERIFY(!table.check(QStringLiteral("😀風 ㄌㄚ-ㄈㄥ")).isEmpty());
    QVERIFY(!table.check(QStringLiteral("你好 invalid-invalid")).isEmpty());
    // Supplied table paths are authoritative; fail closed rather than using a
    // different installed/source table when the configured one cannot load.
    llavon::lora::PhraseTable missing(directory.path() + "/missing.json");
    QVERIFY(!missing.check(QStringLiteral("拉風 ㄌㄚ-ㄈㄥ")).isEmpty());
    const auto customPath = directory.path() + "/bopomofo_char.json";
    QFile custom(customPath); QVERIFY(custom.open(QIODevice::WriteOnly));
    custom.write(QStringLiteral("{\"ㄌㄚ \":[\"𠮷\"],\"ㄈㄥ \":[\"風\"]}").toUtf8()); custom.close();
    llavon::lora::PhraseTable supplementary(customPath);
    QVERIFY(supplementary.check(QStringLiteral("𠮷風 ㄌㄚ-ㄈㄥ")).isEmpty());
    QVERIFY(!supplementary.check(QStringLiteral("拉風 ㄌㄚ-ㄈㄥ")).isEmpty());
    llavon::lora::SettingsStore store(tablePath); store.loadPhrases();
    store.savePhrases(QStringLiteral("拉風 ㄌㄚ-ㄈㄥ\n"));
    const auto saved = store.loadPhrases();
    QVERIFY_EXCEPTION_THROWN(store.savePhrases(QStringLiteral("你好 ㄌㄚ-ㄈㄥ")), std::runtime_error);
    QCOMPARE(store.loadPhrases(), saved);
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path() + "/training", "--cli", LLAVON_TEST_CLI,
        "--tables-dir", QStringLiteral(LLAVON_TEST_TABLES)});
    Manager window(&backend); window.show();
    QSignalSpy ready(&window, &Manager::refreshed); backend.start();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000); window.showPage("phrases");
    auto* list = widget<llavon::lora::PhraseList>(window, "phraseList");
    QVERIFY(!window.findChild<QPlainTextEdit*>("phraseEditor"));
    QCOMPARE(phraseRow(window, 0)->findChild<QComboBox*>("phraseReading_0")->currentData().toString(), QStringLiteral("ㄌㄚ"));
    click(window, "addPhrase");
    auto* row = phraseRow(window, 1);
    auto* input = row->findChild<QLineEdit*>("phraseWord"); input->setText(QStringLiteral("銀行"));
    auto* polyphonic = row->findChild<QComboBox*>("phraseReading_1");
    QCOMPARE(polyphonic->currentIndex(), -1);
    click(window, "savePhrases"); QCOMPARE(store.loadPhrases(), saved);
    polyphonic->setCurrentIndex(polyphonic->findData(QStringLiteral("ㄏㄤˊ")));
    QVERIFY(list->text().contains(QStringLiteral("銀行 ㄧㄣˊ-ㄏㄤˊ")));
    const auto added = list->text();
    click(window, "savePhrases"); QCOMPARE(store.loadPhrases(), added);
    // Existing rows are editable in place; changing one character retains the
    // explicit choice on the unchanged character, but never guesses a new one.
    input->setText(QStringLiteral("銀河"));
    QCOMPARE(row->findChild<QComboBox*>("phraseReading_0")->currentData().toString(), QStringLiteral("ㄧㄣˊ"));
    input->setText(QStringLiteral("銀行")); polyphonic = row->findChild<QComboBox*>("phraseReading_1");
    QCOMPARE(polyphonic->currentIndex(), -1);
    polyphonic->setCurrentIndex(polyphonic->findData(QStringLiteral("ㄒㄧㄥˊ")));
    click(window, "savePhrases"); QVERIFY(store.loadPhrases().contains(QStringLiteral("銀行 ㄧㄣˊ-ㄒㄧㄥˊ")));
    polyphonic->setCurrentIndex(polyphonic->findData(QStringLiteral("ㄏㄤˊ"))); click(window, "savePhrases");
    window.showPage("settings"); window.showPage("phrases");
    QCOMPARE(phraseRow(window, 1)->findChild<QComboBox*>("phraseReading_1")->currentData().toString(), QStringLiteral("ㄏㄤˊ"));
    capture(window, "21-phrase-list");
    click(window, "addPhrase"); auto* duplicate = phraseRow(window, 2);
    duplicate->findChild<QLineEdit*>("phraseWord")->setText(QStringLiteral("拉風"));
    QVERIFY(widget<QLabel>(window, "phraseCheckStatus")->text().contains(QStringLiteral("重複")));
    click(window, "savePhrases"); QCOMPARE(store.loadPhrases(), added);
    capture(window, "22-phrase-list-validation");
    QTest::mouseClick(duplicate->findChild<QPushButton*>("removePhrase"), Qt::LeftButton);
    click(window, "savePhrases"); QCOMPARE(store.loadPhrases(), added);
    click(window, "addPhrase"); auto* longRow = phraseRow(window, 2);
    auto* longInput = longRow->findChild<QLineEdit*>("phraseWord"); longInput->setText(QStringLiteral("未知😀"));
    QVERIFY(!list->errors().isEmpty());
    longInput->setText(QStringLiteral("拉風拉風拉風拉風"));
    QCOMPARE(longRow->findChildren<QComboBox*>(QRegularExpression("phraseReading_.*")).size(), 8);
    window.resize(800, 620); QTest::qWait(100);
    QCOMPARE(window.size(), QSize(800, 620));
    widget<QScrollArea>(window, "phraseScrollArea")->ensureWidgetVisible(longRow);
    QTRY_COMPARE(widget<QScrollArea>(window, "phraseScrollArea")->horizontalScrollBar()->maximum(), 0);
    for (auto* choice : window.findChildren<QComboBox*>(QRegularExpression("phraseReading_.*")))
        QVERIFY(choice->height() >= 34);
    capture(window, "23-phrase-list-compact");
    click(window, "savePhrases");
    // Malformed legacy lines and comments survive loading. Invalid existing
    // readings stay visible until explicitly corrected through the dropdown.
    llavon::lora::PhraseList legacy(tablePath); legacy.show();
    legacy.loadText(QStringLiteral("# 保留註解\n你好 ㄋㄧˇ-ㄈㄥ\nbroken raw\n"));
    QVERIFY(legacy.text().contains("broken raw")); QVERIFY(!legacy.errors().isEmpty());
    auto* bad = phraseRow(legacy, 0)->findChild<QComboBox*>("phraseReading_1");
    QCOMPARE(bad->currentData().toString(), QStringLiteral("ㄈㄥ"));
    bad->setCurrentIndex(bad->findData(QStringLiteral("ㄏㄠˇ")));
    phraseRow(legacy, 1)->findChild<QLineEdit*>("phraseWord")->setText(QStringLiteral("拉風"));
    QVERIFY(legacy.errors().isEmpty()); QVERIFY(legacy.text().startsWith(QStringLiteral("# 保留註解\n")));
    QTest::mouseClick(phraseRow(legacy, 0)->findChild<QPushButton*>("removePhrase"), Qt::LeftButton);
    QCOMPARE(phraseRow(legacy, 0)->findChild<QLineEdit*>("phraseWord")->text(), QStringLiteral("拉風"));
    QTest::mouseClick(phraseRow(legacy, 0)->findChild<QPushButton*>("removePhrase"), Qt::LeftButton);
    QCOMPARE(legacy.text(), QStringLiteral("# 保留註解\n"));
}

void NativeTests::pageLaunchRouting() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QLockFile lock(directory.path() + "/native-gui.lock"); QVERIFY(lock.tryLock());
    const auto identity = directory.path() + ":" + QDir::homePath();
    const auto name = "llavon-lora-" + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    QLocalServer server; QVERIFY(server.listen(name));
    for (const auto* page : {"settings", "phrases", "records", "training", "history", "updates", "about"}) {
        QProcess app;
        auto environment = QProcessEnvironment::systemEnvironment();
        // Warm routing must work without loading any GUI platform plugin.
        environment.insert("QT_QPA_PLATFORM", "llavon-intentionally-unavailable");
        app.setProcessEnvironment(environment);
        app.start(LLAVON_TEST_GUI, {"--state-dir", directory.path(), "--page", page});
        QVERIFY(server.waitForNewConnection(5000));
        auto* socket = server.nextPendingConnection(); QVERIFY(socket);
        if (!socket->canReadLine()) QVERIFY(socket->waitForReadyRead(3000));
        QCOMPARE(socket->readLine().trimmed(), QByteArray(page));
        QVERIFY(app.waitForFinished(5000)); QCOMPARE(app.exitCode(), 0);
        delete socket;
    }
}

void NativeTests::deferredPhrasePage() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    llavon::lora::SettingsStore store; store.loadPhrases(); store.savePhrases(QStringLiteral("拉風 ㄌㄚ-ㄈㄥ\n"));
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path() + "/training", "--cli", LLAVON_TEST_CLI});
    Manager window(&backend, {}, "settings"); window.show();
    QVERIFY(widget<QLabel>(window, "notice")->text().isEmpty());
    QVERIFY(!widget<QLabel>(window, "notice")->isVisible());
    QVERIFY(!window.findChild<llavon::lora::PhraseList*>("phraseList"));
    // An external edit made after launch is loaded on the first visit.
    store.savePhrases(QStringLiteral("銀行 ㄧㄣˊ-ㄏㄤˊ\n"));
    window.showPage("phrases");
    auto* list = widget<llavon::lora::PhraseList>(window, "phraseList");
    auto* word = phraseRow(window, 0)->findChild<QLineEdit*>("phraseWord");
    QCOMPARE(word->text(), QStringLiteral("銀行"));
    word->setText(QStringLiteral("銀河"));
    window.showPage("settings"); window.showPage("phrases");
    QCOMPARE(window.findChild<llavon::lora::PhraseList*>("phraseList"), list);
    QCOMPARE(word->text(), QStringLiteral("銀河"));
    QTimer::singleShot(100, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) qFatal("Deferred phrase edits lost their close confirmation");
        dialog->button(QMessageBox::Cancel)->click();
    });
    QVERIFY(!window.close()); QVERIFY(window.isVisible());
    QCOMPARE(store.loadPhrases(), QStringLiteral("銀行 ㄧㄣˊ-ㄏㄤˊ\n"));
    QTimer::singleShot(100, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) qFatal("Expected discard confirmation");
        dialog->button(QMessageBox::Yes)->click();
    });
    QVERIFY(window.close());
}

void NativeTests::settingsWithoutTrainingBackend() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    Backend backend(directory.path() + "/missing-training-backend", {});
    QSignalSpy errors(&backend, &Backend::error), ready(&backend, &Backend::ready);
    Manager window(&backend, {}, "settings"); window.show();
    QVERIFY(window.findChild<QSpinBox*>("setting_context_length"));
    QVERIFY(!window.findChild<QTableWidget*>("records"));
    QVERIFY(!window.findChild<QPushButton*>("startTraining"));
    QVERIFY(!window.findChild<llavon::lora::HistoryGraph*>("history"));
    QTest::qWait(100); QCOMPARE(errors.count(), 0); QCOMPARE(ready.count(), 0);
    widget<QTabWidget>(window, "settingsGroups")->setCurrentIndex(1);
    auto* count = widget<QSpinBox>(window, "setting_candidate_page_size");
    count->setValue(7); click(window, "saveSettings");
    llavon::lora::SettingsStore store; QCOMPARE(store.load().value("candidate_page_size").toInt(), 7);
    window.showPage("training");
    QVERIFY(window.findChild<QTableWidget*>("records"));
    QVERIFY(window.findChild<QPushButton*>("startTraining"));
    QVERIFY(window.findChild<llavon::lora::HistoryGraph*>("history"));
    QTRY_COMPARE(errors.count(), 1);
    window.showPage("settings"); QCOMPARE(count->value(), 7);
    QVERIFY(window.close());
}

void NativeTests::deferredSettingGroups() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    llavon::lora::SettingsStore store;
    auto values = store.load(); values["candidate_page_size"] = 9; values["keyboard_layout"] = "ibm"; store.save(values);
    llavon::lora::SettingsPage page(false); page.show();
    auto* tabs = page.findChild<QTabWidget*>("settingsGroups"); QVERIFY(tabs);
    QVERIFY(!page.findChild<QSpinBox*>("setting_candidate_page_size"));
    auto* context = page.findChild<QSpinBox*>("setting_context_length"); QVERIFY(context);
    context->setValue(256);
    QTest::mouseClick(page.findChild<QPushButton*>("saveSettings"), Qt::LeftButton);
    auto saved = store.load(); QCOMPARE(saved.value("context_length").toInt(), 256);
    QCOMPARE(saved.value("candidate_page_size").toInt(), 9); QCOMPARE(saved.value("keyboard_layout").toString(), QStringLiteral("ibm"));
    QVERIFY(!page.isDirty()); tabs->setCurrentIndex(1); QVERIFY(!page.isDirty());
    auto* count = page.findChild<QSpinBox*>("setting_candidate_page_size"); QVERIFY(count); QCOMPARE(count->value(), 9);
    count->setValue(7); QVERIFY(page.isDirty());
    tabs->setCurrentIndex(2); QVERIFY(page.isDirty()); tabs->setCurrentIndex(1);
    QCOMPARE(count->value(), 7); QCOMPARE(context->value(), 256);
    QTest::mouseClick(page.findChild<QPushButton*>("saveSettings"), Qt::LeftButton);
    saved = store.load(); QCOMPARE(saved.value("candidate_page_size").toInt(), 7);
    QCOMPARE(saved.value("keyboard_layout").toString(), QStringLiteral("ibm"));
    QVERIFY(!page.isDirty());
}

void NativeTests::featureGuide() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    llavon::lora::SettingsStore store;
    const auto saved = store.load(); store.save(saved);
    QFile configuration(llavon::lora::SettingsStore::configPath()); QVERIFY(configuration.open(QIODevice::ReadOnly));
    const auto before = configuration.readAll(); configuration.close();
    QVERIFY(!llavon::lora::QuickStartPreferences::dismissed());
    {
        Backend backend(directory.path() + "/missing-backend", {});
        QSignalSpy errors(&backend, &Backend::error), ready(&backend, &Backend::ready);
        Manager window(&backend, {}, "settings"); window.show();
        QVERIFY(widget<QWidget>(window, "quickStart")->isVisible());
        QVERIFY(!window.findChild<llavon::lora::GuidePage*>());
        QCOMPARE(widget<QPushButton>(window, "skipQuickGuide")->text(), QStringLiteral("略過全部"));
        capture(window, "30-feature-invitation");
        click(window, "startQuickGuide");
        auto* guide = widget<llavon::lora::GuidePage>(window, "usageGuide");
        QVERIFY(guide->isVisible());
        QVERIFY(guide->isWindow()); QCOMPARE(guide->windowModality(), Qt::NonModal);
        QCOMPARE(widget<QListWidget>(window, "navigation")->currentRow(), 0);
        auto* steps = widget<QStackedWidget>(window, "guideSteps");
        auto* topics = widget<QTabWidget>(window, "guideTopics");
        const auto advance = [&window] {
            QCoreApplication::processEvents();
            click(window, "guideNext");
            QCoreApplication::processEvents();
        };
        QCOMPARE(steps->count(), 3); QCOMPARE(steps->currentIndex(), 0);
        QVERIFY(!widget<QWidget>(window, "quickStart")->isVisible());
        QVERIFY(!widget<QPushButton>(window, "guidePrevious")->isEnabled());
        QVERIFY(widget<QPushButton>(window, "guideSkipAll")->isVisible());
        capture(*guide, "31-feature-mixed-input");
        click(window, "guideMixedFeature");
        auto* smart = widget<QComboBox>(window, "setting_smart_english");
        QCOMPARE(smart->currentData().toBool(), saved.value("smart_english").toBool());
        QTRY_VERIFY(smart->isVisible());
        click(window, "openGuide"); QCOMPARE(steps->currentIndex(), 0);
        advance(); QCOMPARE(steps->currentIndex(), 1);
        QTest::keyClick(guide, Qt::Key_Escape);
        QTRY_VERIFY(!guide->isVisible()); QVERIFY(window.isVisible());
        QVERIFY(!llavon::lora::QuickStartPreferences::dismissed());
        click(window, "openGuide");
        QCOMPARE(window.findChildren<llavon::lora::GuidePage*>().size(), 1);
        QCOMPARE(widget<llavon::lora::GuidePage>(window, "usageGuide"), guide);
        QCOMPARE(steps->currentIndex(), 1);
        QCOMPARE(widget<QListWidget>(window, "navigation")->currentRow(), 0);
        capture(*guide, "32-feature-phrase-overrides");
        const auto originalPalette = QApplication::palette();
        for (const bool dark : {false, true}) {
            auto palette = originalPalette; palette.setColor(QPalette::Window, dark ? QColor("#202124") : QColor("#f5f5f7"));
            QApplication::setPalette(palette); QCoreApplication::processEvents();
            capture(*guide, dark ? "32-feature-phrase-dark" : "32-feature-phrase-light");
        }
        QApplication::setPalette(originalPalette); QCoreApplication::processEvents();
        click(window, "guidePrevious"); QCOMPARE(steps->currentIndex(), 0);
        advance(); advance(); QCOMPARE(steps->currentIndex(), 2);
        capture(*guide, "33-feature-personalization");
        click(window, "guideRestart"); QCOMPARE(steps->currentIndex(), 0);
        for (int index = 0; index < topics->count(); ++index) {
            topics->setCurrentIndex(index);
            QVERIFY(widget<QPushButton>(window, "guideSkipAll")->isVisible());
            if (auto* area = qobject_cast<QScrollArea*>(topics->currentWidget())) {
                QCoreApplication::processEvents(); QCOMPARE(area->horizontalScrollBar()->maximum(), 0);
            }
        }
        // Viewing every topic never enables recording or starts the helper.
        QTest::qWait(100); QCOMPARE(errors.count(), 0); QCOMPARE(ready.count(), 0);
        QVERIFY(!window.findChild<QTableWidget*>("records"));
        QVERIFY(!QFile::exists(llavon::lora::SettingsStore::phrasesPath()));
        QVERIFY(!llavon::lora::QuickStartPreferences::dismissed());
        guide->resize(660, 480); capture(*guide, "34-guide-compact");
        QVERIFY(widget<QPushButton>(window, "guideSkipAll")->isVisible());
        click(window, "guideSkipAll");
        QVERIFY(llavon::lora::QuickStartPreferences::dismissed());
        QVERIFY(!widget<QWidget>(window, "quickStart")->isVisible());
        QVERIFY(!guide->isVisible());
        QVERIFY(configuration.open(QIODevice::ReadOnly)); QCOMPARE(configuration.readAll(), before); configuration.close();
        QVERIFY(window.close());
    }
    {
        Backend backend(directory.path() + "/missing-backend", {});
        Manager window(&backend, {}, "settings"); window.show();
        QVERIFY(!widget<QWidget>(window, "quickStart")->isVisible());
        click(window, "openGuide");
        QCOMPARE(widget<QStackedWidget>(window, "guideSteps")->currentIndex(), 0);
        // A direct settings link keeps already edited fields intact.
        window.showPage("settings");
        auto* context = widget<QSpinBox>(window, "setting_context_length"); context->setValue(256);
        click(window, "openGuide"); click(window, "guideMixedFeature");
        QCOMPARE(context->value(), 256);
        context->setValue(saved.value("context_length").toInt()); click(window, "saveSettings");
        click(window, "openGuide");
        for (int step = 0; step < 3; ++step) {
            QCoreApplication::processEvents(); click(window, "guideNext"); QCoreApplication::processEvents();
        }
        QTRY_VERIFY(!widget<llavon::lora::GuidePage>(window, "usageGuide")->isVisible());
        QVERIFY(!widget<QWidget>(window, "quickStart")->isVisible());
        click(window, "openGuide"); QCOMPARE(widget<QStackedWidget>(window, "guideSteps")->currentIndex(), 0);
        QVERIFY(window.close());
        QVERIFY(!widget<llavon::lora::GuidePage>(window, "usageGuide")->isVisible());
    }
    // Skipping the initial invitation also persists, without building the guide.
    Environment separate("XDG_CONFIG_HOME", (directory.path() + "/separate").toUtf8());
    Backend backend(directory.path() + "/missing-backend", {});
    Manager window(&backend, {}, "settings"); window.show();
    click(window, "skipQuickGuide"); QVERIFY(llavon::lora::QuickStartPreferences::dismissed());
    QVERIFY(!window.findChild<llavon::lora::GuidePage*>());
    QVERIFY(!QFile::exists(llavon::lora::SettingsStore::configPath()));
    QVERIFY(window.close());
}

void NativeTests::guideIllustrations() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    llavon::lora::SettingsStore store; const auto saved = store.load(); store.save(saved);
    QFile file(llavon::lora::SettingsStore::configPath()); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto before = file.readAll(); file.close();
    Backend backend(directory.path() + "/missing-backend", {});
    QSignalSpy errors(&backend, &Backend::error), ready(&backend, &Backend::ready);
    Manager window(&backend, {}, "settings"); window.show();
    QVERIFY(!window.findChild<llavon::lora::GuideIllustration*>());
    auto* context = widget<QSpinBox>(window, "setting_context_length");
    const auto original = context->value(); context->setValue(original == 256 ? 128 : 256);
    click(window, "openGuide"); auto* guide = widget<llavon::lora::GuidePage>(window, "usageGuide");
    const QStringList names{"guideMixedIllustration", "guidePhraseIllustration", "guidePrivacyIllustration"};
    for (int step = 0; step < names.size(); ++step) {
        auto* scene = guide->findChild<llavon::lora::GuideIllustration*>(names[step]); QVERIFY(scene);
        QVERIFY(scene->isVisible()); QVERIFY(scene->property("illustrationOnly").toBool());
        QVERIFY(scene->findChildren<QLineEdit*>().isEmpty());
        QVERIFY(scene->findChildren<QComboBox*>().isEmpty());
        QVERIFY(scene->findChildren<QPushButton*>().isEmpty());
        for (const int width : {780, 660, 900}) {
            guide->resize(width, 720); QCoreApplication::processEvents();
            const auto rendered = scene->grab(); QVERIFY(!rendered.isNull());
            const auto targets = scene->targetRects(); QCOMPARE(targets.size(), 2);
            for (const auto target : targets) {
                QVERIFY(scene->rect().contains(target)); QVERIFY(target.width() >= 24); QVERIFY(target.height() >= 24);
                // A click on the image must not edit data or give consent.
                // Never synthesize a native Cocoa click outside the scroll
                // viewport: that screen point may belong to a footer button.
                if (scene->visibleRegion().contains(target.center()))
                    QTest::mouseClick(scene, Qt::LeftButton, Qt::NoModifier, target.center());
            }
            auto* area = qobject_cast<QScrollArea*>(widget<QStackedWidget>(window, "guideSteps")->currentWidget()); QVERIFY(area);
            QCOMPARE(area->horizontalScrollBar()->maximum(), 0);
        }
        guide->resize(660, 480); QCoreApplication::processEvents();
        capture(*guide, QStringLiteral("36-illustration-compact-%1").arg(step));
        QVERIFY(widget<QPushButton>(window, "guideSkipAll")->isVisible());
        guide->resize(780, 720); QCoreApplication::processEvents();
        if (step < names.size() - 1) { click(window, "guideNext"); QCoreApplication::processEvents(); }
    }
    QTest::qWait(100); QCOMPARE(errors.count(), 0); QCOMPARE(ready.count(), 0);
    QVERIFY(!window.findChild<QTableWidget*>("records"));
    QVERIFY(!window.findChild<QPushButton*>("setup"));
    QVERIFY(!QFile::exists(llavon::lora::SettingsStore::phrasesPath()));
    QVERIFY(!llavon::lora::QuickStartPreferences::dismissed());
    QCOMPARE(context->value(), original == 256 ? 128 : 256);
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), before); file.close();
    context->setValue(original); click(window, "saveSettings"); QVERIFY(window.close());
}

void NativeTests::guideDetailTopics() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    llavon::lora::SettingsStore store; store.save(store.load());
    QFile configuration(llavon::lora::SettingsStore::configPath()); QVERIFY(configuration.open(QIODevice::ReadOnly));
    const auto before = configuration.readAll(); configuration.close();
    llavon::lora::GuidePage guide; llavon::lora::applyAppearance(&guide); guide.show();
    QSignalSpy pageRequests(&guide, &llavon::lora::GuidePage::pageRequested);
    QSignalSpy settingRequests(&guide, &llavon::lora::GuidePage::settingRequested);
    auto* topics = guide.findChild<QTabWidget*>("guideTopics"); QVERIFY(topics); QCOMPARE(topics->count(), 6);
    QCOMPARE(guide.findChildren<llavon::lora::GuideIllustration*>().size(), 3);
    QVERIFY(!guide.findChild<QPushButton*>("guideRecords"));
    const QStringList names{"guideShortcutTopic", "guideMixedTopic", "guidePhraseTopic", "guidePrivacyTopic", "guideHelpTopic"};
    const auto originalPalette = QApplication::palette();
    const auto restorePalette = qScopeGuard([originalPalette] { QApplication::setPalette(originalPalette); });
    for (int index = 1; index < topics->count(); ++index) {
        topics->setCurrentIndex(index);
        auto* area = qobject_cast<QScrollArea*>(topics->currentWidget()); QVERIFY(area);
        QCOMPARE(area->objectName(), names[index - 1]);
        if (index == 1) QCOMPARE(area->findChildren<QFrame*>("guideShortcutRow").size(), 10);
        if (index >= 2 && index <= 4) QCOMPARE(area->findChildren<llavon::lora::GuideIllustration*>().size(), 1);
        if (index == 4) QCOMPARE(area->findChildren<QWidget*>("guideFlowStage").size(), 3);
        if (index == 5) QCOMPARE(area->findChildren<QFrame*>("guideHelpRow").size(), 2);
        if (index >= 2 && index <= 4) {
            auto* preview = area->findChild<llavon::lora::GuideIllustration*>();
            area->ensureWidgetVisible(preview); QCoreApplication::processEvents();
            for (const auto& target : preview->targetRects()) {
                if (preview->visibleRegion().contains(target.center()))
                    QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier, target.center());
            }
        }
        for (const bool dark : {false, true}) {
            auto palette = originalPalette; palette.setColor(QPalette::Window, dark ? QColor("#202124") : QColor("#f5f5f7"));
            QApplication::setPalette(palette); llavon::lora::applyAppearance(&guide); QCoreApplication::processEvents();
            guide.resize(780, 720);
            area->verticalScrollBar()->setValue(0);
            capture(guide, QStringLiteral("70-topic-%1-%2").arg(index).arg(dark ? "dark" : "light"));
        }
        guide.resize(660, 480); QCoreApplication::processEvents();
        capture(guide, QStringLiteral("71-topic-%1-compact").arg(index));
        QCOMPARE(area->horizontalScrollBar()->maximum(), 0);
        auto* skip = guide.findChild<QPushButton*>("guideSkipAll"); QVERIFY(skip->isVisible());
        QVERIFY(guide.rect().contains(QRect(skip->mapTo(&guide, QPoint()), skip->size())));
        guide.resize(780, 720); QCoreApplication::processEvents();
    }
    QCOMPARE(guide.findChildren<llavon::lora::GuideIllustration*>().size(), 6);
    topics->setCurrentIndex(0); topics->setCurrentIndex(4);
    QCOMPARE(guide.findChildren<llavon::lora::GuideIllustration*>().size(), 6); // Reopening reuses the topic.
    QCOMPARE(pageRequests.count(), 0); QCOMPARE(settingRequests.count(), 0);
    QVERIFY(!QFile::exists(llavon::lora::SettingsStore::phrasesPath()));
    QVERIFY(!llavon::lora::QuickStartPreferences::dismissed());
    QVERIFY(configuration.open(QIODevice::ReadOnly)); QCOMPARE(configuration.readAll(), before); configuration.close();
    for (const auto& entry : {QPair{2, QString("guideSmartEnglish")}, QPair{5, QString("guideKeyboard")}}) {
        topics->setCurrentIndex(entry.first); QCoreApplication::processEvents();
        auto* button = guide.findChild<QPushButton*>(entry.second); QVERIFY(button);
        qobject_cast<QScrollArea*>(topics->currentWidget())->ensureWidgetVisible(button);
        QCoreApplication::processEvents(); QVERIFY(button->visibleRegion().contains(button->rect().center()));
        QTest::mouseClick(button, Qt::LeftButton);
    }
    QCOMPARE(settingRequests.count(), 2);
    QCOMPARE(settingRequests.at(0).at(0).toString(), QStringLiteral("smart_english"));
    QCOMPARE(settingRequests.at(1).at(0).toString(), QStringLiteral("keyboard_layout"));
    for (const auto& entry : {QPair{3, QString("guidePhraseList")}, QPair{4, QString("guideRecords")}, QPair{5, QString("guideStatus")}}) {
        topics->setCurrentIndex(entry.first); QCoreApplication::processEvents();
        auto* button = guide.findChild<QPushButton*>(entry.second); QVERIFY(button);
        qobject_cast<QScrollArea*>(topics->currentWidget())->ensureWidgetVisible(button);
        QCoreApplication::processEvents(); QVERIFY(button->visibleRegion().contains(button->rect().center()));
        QTest::mouseClick(button, Qt::LeftButton);
    }
    QCOMPARE(pageRequests.count(), 3);
    QCOMPARE(pageRequests.at(0).at(0).toString(), QStringLiteral("phrases"));
    QCOMPARE(pageRequests.at(1).at(0).toString(), QStringLiteral("records"));
    QCOMPARE(pageRequests.at(2).at(0).toString(), QStringLiteral("about"));
    QVERIFY(guide.close());
}

void NativeTests::quietInterface() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path() + "/training", "--cli", LLAVON_TEST_CLI});
    Manager window(&backend, {}, "settings"); window.show();
    auto* notice = widget<QLabel>(window, "notice");
    QCOMPARE(window.windowTitle(), QStringLiteral("拉風輸入法"));
    QVERIFY(!notice->isVisible()); QVERIFY(!widget<QLabel>(window, "settingsStatus")->isVisible());
    const QStringList redundant{QStringLiteral("工作空間"), QStringLiteral("設定與個人化"),
        QStringLiteral("個人資料儲存於此裝置"), QStringLiteral("設定儲存於此裝置"), QStringLiteral("已載入目前設定")};
    for (auto* label : window.findChildren<QLabel*>()) if (label->isVisible()) QVERIFY(!redundant.contains(label->text()));
    QCOMPARE(widget<QLabel>(window, "pageHeading")->text(), QStringLiteral("輸入法設定"));
    capture(window, "60-quiet-settings");
    auto* context = widget<QSpinBox>(window, "setting_context_length");
    const auto original = context->value(); context->setValue(original == 256 ? 128 : 256);
    QVERIFY(widget<QLabel>(window, "settingsStatus")->isVisible());
    QCOMPARE(widget<QLabel>(window, "settingsStatus")->text(), QStringLiteral("尚未儲存"));
    click(window, "saveSettings");
    QTRY_VERIFY(widget<QLabel>(window, "settingsStatus")->text().startsWith(QStringLiteral("已儲存")));
    QVERIFY(widget<QLabel>(window, "settingsStatus")->isVisible());
    QVERIFY(!widget<QPushButton>(window, "saveSettings")->toolTip().isEmpty());
    click(window, "reloadSettings"); QVERIFY(!widget<QLabel>(window, "settingsStatus")->isVisible());
    window.showPage("phrases");
    auto* checked = widget<QLabel>(window, "phraseCheckStatus");
    QVERIFY(!checked->isVisible()); QVERIFY(!widget<QLabel>(window, "phraseStatus")->isVisible());
    click(window, "checkPhrases"); QVERIFY(checked->isVisible()); QCOMPARE(checked->text(), QStringLiteral("檢查通過"));
    click(window, "addFirstPhrase"); QVERIFY(checked->isVisible());
    click(window, "savePhrases");
    QVERIFY(widget<QLabel>(window, "phraseStatus")->isVisible());
    QVERIFY(!QFile::exists(llavon::lora::SettingsStore::phrasesPath()));
    auto* row = phraseRow(window, 0); row->findChild<QLineEdit*>("phraseWord")->setText(QStringLiteral("李拉風"));
    const QStringList readings{QStringLiteral("ㄌㄧˇ"), QStringLiteral("ㄌㄚ"), QStringLiteral("ㄈㄥ")};
    for (int index = 0; index < readings.size(); ++index) {
        auto* reading = row->findChild<QComboBox*>(QStringLiteral("phraseReading_%1").arg(index)); QVERIFY(reading);
        reading->setCurrentIndex(reading->findData(readings[index]));
    }
    QVERIFY(!checked->isVisible()); QVERIFY(widget<QLabel>(window, "phraseStatus")->isVisible());
    click(window, "savePhrases");
    QTRY_VERIFY(widget<QLabel>(window, "phraseStatus")->text().startsWith(QStringLiteral("已儲存")));
    click(window, "reloadPhrases");
    QTRY_VERIFY(widget<QLabel>(window, "phraseStatus")->text().startsWith(QStringLiteral("已重新載入")));
    window.showPage("settings"); window.showPage("phrases");
    QVERIFY(!widget<QLabel>(window, "phraseStatus")->isVisible()); QVERIFY(!checked->isVisible());
    capture(window, "61-quiet-phrases");
    QSignalSpy refreshed(&window, &Manager::refreshed);
    window.showPage("training"); QTRY_VERIFY_WITH_TIMEOUT(refreshed.count() > 0, 10000);
    QVERIFY(!notice->isVisible()); QVERIFY(!widget<QPlainTextEdit>(window, "jobLog")->isVisible());
    capture(window, "62-quiet-training");
    window.showPage("records"); capture(window, "63-quiet-records");
    window.showPage("history");
    auto* graph = widget<llavon::lora::HistoryGraph>(window, "history");
    QVERIFY(graph->toolTip().contains(QStringLiteral("拖曳")));
    QVERIFY(graph->accessibleDescription().contains(QStringLiteral("縮放")));
    capture(window, "64-quiet-history");
    // A quiet idle workspace must still surface errors, including on other pages.
    backend.error(QStringLiteral("測試錯誤")); QVERIFY(notice->isVisible());
    QCOMPARE(notice->text(), QStringLiteral("測試錯誤"));
    window.showPage("settings"); QVERIFY(notice->isVisible());
    QVERIFY(window.close());
}

void NativeTests::phraseEmptyDiscovery() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    Backend backend(directory.path() + "/missing-backend", {});
    Manager window(&backend, {}, "settings"); window.show();
    click(window, "openGuide"); click(window, "guideNext"); click(window, "guidePhrases");
    auto* list = widget<llavon::lora::PhraseList>(window, "phraseList");
    auto* empty = widget<QWidget>(window, "phraseEmptyState"); QVERIFY(empty->isVisible());
    QVERIFY(widget<QLabel>(window, "phraseEmptyDescription")->text().contains(QStringLiteral("李拉風")));
    QVERIFY(list->text().isEmpty()); QVERIFY(!QFile::exists(llavon::lora::SettingsStore::phrasesPath()));
    capture(window, "35-phrase-discovery");
    click(window, "addFirstPhrase"); QVERIFY(!empty->isVisible());
    auto* row = phraseRow(window, 0); auto* word = row->findChild<QLineEdit*>("phraseWord");
    QVERIFY(word->text().isEmpty()); word->setText(QStringLiteral("李拉風"));
    const QStringList readings{QStringLiteral("ㄌㄧˇ"), QStringLiteral("ㄌㄚ"), QStringLiteral("ㄈㄥ")};
    for (int index = 0; index < readings.size(); ++index) {
        auto* reading = row->findChild<QComboBox*>(QStringLiteral("phraseReading_%1").arg(index)); QVERIFY(reading);
        const auto selected = reading->findData(readings[index]); QVERIFY(selected >= 0); reading->setCurrentIndex(selected);
    }
    QVERIFY(list->errors().isEmpty()); QVERIFY(!QFile::exists(llavon::lora::SettingsStore::phrasesPath()));
    click(window, "savePhrases");
    llavon::lora::SettingsStore store;
    QCOMPARE(store.loadPhrases(), QStringLiteral("李拉風 ㄌㄧˇ-ㄌㄚ-ㄈㄥ\n"));
    click(window, "openGuide"); QCOMPARE(widget<QStackedWidget>(window, "guideSteps")->currentIndex(), 1);
    click(window, "guidePhrases"); QVERIFY(!empty->isVisible());
    QTest::mouseClick(phraseRow(window, 0)->findChild<QPushButton*>("removePhrase"), Qt::LeftButton);
    QVERIFY(empty->isVisible()); click(window, "savePhrases"); QVERIFY(store.loadPhrases().isEmpty());
    QVERIFY(window.close());
}

void NativeTests::appearanceChanges() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    llavon::lora::SettingsStore store; store.loadPhrases();
    store.savePhrases(QStringLiteral("拉風 ㄌㄚ-ㄈㄥ\n銀行 ㄧㄣˊ-ㄏㄤˊ\n"));
    const auto original = QApplication::palette();
    const auto restore = qScopeGuard([original] { QApplication::setPalette(original); });
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path() + "/training", "--cli", LLAVON_TEST_CLI});
    Manager window(&backend); window.show(); backend.start();
    window.showPage("settings");
    widget<QTabWidget>(window, "settingsGroups")->setCurrentIndex(1);
    auto* count = widget<QSpinBox>(window, "setting_candidate_page_size");
    count->setValue(8); // An appearance change must not reconstruct/lose edits.
    for (const bool dark : {false, true}) {
        auto palette = original; palette.setColor(QPalette::Window, dark ? QColor("#202124") : QColor("#f5f5f7"));
        QApplication::setPalette(palette); QCoreApplication::processEvents();
        QCOMPARE(window.palette().color(QPalette::Window).lightness() < 128, dark);
        QCOMPARE(count->value(), 8);
        QVERIFY(!QPixmap(dark ? ":/native/chevron-dark.xpm" : ":/native/chevron-light.xpm").isNull());
        window.showPage("settings"); widget<QTabWidget>(window, "settingsGroups")->setCurrentIndex(1);
        capture(window, dark ? "31-settings-dark" : "30-settings-light");
        window.showPage("phrases");
        auto* reading = phraseRow(window, 1)->findChild<QComboBox*>("phraseReading_1");
        const auto index = reading->currentIndex(); reading->setFocus(); QTest::keyClick(reading, index == 0 ? Qt::Key_End : Qt::Key_Home);
        QVERIFY(reading->currentIndex() != index); reading->setCurrentIndex(index);
        capture(window, dark ? "33-phrases-dark" : "32-phrases-light");
        click(window, "savePhrases");
    }
    window.showPage("settings"); click(window, "saveSettings");
    QCOMPARE(store.load().value("candidate_page_size").toInt(), 8);
}

void NativeTests::hostSettingsMigration() {
#ifdef Q_OS_MACOS
    Environment port("LLAVON_IME_SETTINGS_PORT", ("org.llavon-ime.settings.qt-test." + QString::number(QCoreApplication::applicationPid())).toUtf8());
    QTemporaryDir directory;
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path() + "/training", "--cli", LLAVON_TEST_CLI});
    Manager window(&backend, LLAVON_TEST_GUI); window.show(); backend.start(); window.showPage("updates");
    auto* page = window.findChild<llavon::lora::HostSettingsPage*>("macUpdates"); QVERIFY(page);
    auto* check = page->findChild<QPushButton*>("checkUpdates");
    auto* automatic = page->findChild<QCheckBox*>("automaticUpdates");
    auto* downloads = page->findChild<QCheckBox*>("automaticDownloads");
    auto* status = page->findChild<QLabel*>("hostStatus");
    auto* instructions = page->findChild<QLabel*>("updateInstructions"); QVERIFY(instructions);
    QVERIFY(instructions->text().contains(QStringLiteral("不會自動安裝")));
    QVERIFY(instructions->toolTip().contains(QStringLiteral("顯示安裝提示")));
    QVERIFY(instructions->toolTip().contains(QStringLiteral("按鈕會改為「安裝並重新啟動…」")));
    QVERIFY(instructions->text().contains(QStringLiteral("管理員授權")));
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("尚未連接")), 5000);
    QVERIFY(!check->isEnabled()); QVERIFY(!automatic->isEnabled());
    const auto fixture = qEnvironmentVariable("LLAVON_SETTINGS_HOST_FIXTURE");
    if (fixture.isEmpty()) return;
    QProcess server;
    auto hostEnvironment = QProcessEnvironment::systemEnvironment();
    const auto readyPath = directory.path() + "/download-ready";
    hostEnvironment.insert("LLAVON_TEST_READY_FILE", readyPath);
    server.setProcessEnvironment(hostEnvironment); server.start(fixture, {"--serve"});
    QVERIFY(server.waitForReadyRead(5000)); QVERIFY(server.readAllStandardOutput().contains("READY"));
    const auto cleanup = qScopeGuard([&server] { server.kill(); server.waitForFinished(3000); });
    page->refresh(); QTRY_VERIFY_WITH_TIMEOUT(automatic->isEnabled(), 5000);
    QVERIFY(automatic->isChecked()); QVERIFY(!downloads->isChecked());
    QCOMPARE(check->text(), QStringLiteral("檢查更新…"));
    QTest::mouseClick(automatic, Qt::LeftButton, Qt::NoModifier, QPoint(8, automatic->height() / 2));
    QTRY_VERIFY_WITH_TIMEOUT(automatic->isEnabled(), 5000); QVERIFY(!automatic->isChecked());
    QTest::mouseClick(downloads, Qt::LeftButton, Qt::NoModifier, QPoint(8, downloads->height() / 2));
    QTRY_VERIFY_WITH_TIMEOUT(downloads->isEnabled(), 5000); QVERIFY(downloads->isChecked());
    page->refresh(); QTRY_VERIFY_WITH_TIMEOUT(downloads->isEnabled(), 5000); QVERIFY(downloads->isChecked());
    capture(window, "34-updates-before-download");
    QFile readyFile(readyPath); QVERIFY(readyFile.open(QIODevice::WriteOnly)); readyFile.close();
    // Background readiness must change the visible page via its periodic
    // refresh, without another check/update button click.
    QTRY_COMPARE_WITH_TIMEOUT(check->text(), QStringLiteral("安裝並重新啟動…"), 6000);
    QVERIFY(status->text().contains(QStringLiteral("已下載，可安裝")));
    QTest::mouseClick(check, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("已下載")), 5000);
    QCOMPARE(check->text(), QStringLiteral("安裝並重新啟動…"));
    QVERIFY(status->text().contains(QStringLiteral("或按「安裝並重新啟動…」")));
    page->refresh(); QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
    QCOMPARE(check->text(), QStringLiteral("安裝並重新啟動…"));
    capture(window, "34-migrated-updates");
    window.showPage("about");
    auto* diagnostics = window.findChild<llavon::lora::HostSettingsPage*>("hostDiagnostics");
    QTRY_VERIFY_WITH_TIMEOUT(diagnostics->findChild<QLabel*>("hostVersion")->text().contains("migration-test"), 5000);
    capture(window, "35-migrated-status"); window.showPage("updates");
    QTRY_VERIFY_WITH_TIMEOUT(automatic->isEnabled(), 5000);
    server.kill(); QVERIFY(server.waitForFinished(3000));
    page->refresh(); QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("尚未連接")), 5000);
    QVERIFY(!downloads->isEnabled()); QVERIFY(!automatic->isEnabled());
    QCOMPARE(check->text(), QStringLiteral("檢查更新…"));
#else
    llavon::lora::HostSettingsPage page(false); page.show();
    QTRY_VERIFY_WITH_TIMEOUT(page.findChild<QLabel*>("hostStatus")->text().contains(QStringLiteral("尚未連接")), 5000);
#endif
}

void NativeTests::controlInteractions() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Environment config("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    Environment phrases("LLAVON_IME_PHRASE_OVERRIDES_PATH", (directory.path() + "/phrases.txt").toUtf8());
    llavon::lora::SettingsStore store; store.loadPhrases(); store.savePhrases(QStringLiteral("銀行 ㄧㄣˊ-ㄏㄤˊ\n"));
    const auto original = QApplication::palette();
    const auto restore = qScopeGuard([original] { QApplication::setPalette(original); });
    Backend backend(LLAVON_NATIVE_BACKEND, {"--state-dir", directory.path() + "/training", "--cli", LLAVON_TEST_CLI});
    Manager window(&backend); window.show(); backend.start(); window.showPage("settings");
    click(window, "skipQuickGuide"); // This suite exercises controls, not the first-use invitation.
    widget<QTabWidget>(window, "settingsGroups")->setCurrentIndex(1);
    auto* keyboard = widget<QComboBox>(window, "setting_keyboard_layout");
    auto* save = widget<QPushButton>(window, "saveSettings");
    auto popupCapture = [](QComboBox* combo, const QString& name) {
        const auto path = qEnvironmentVariable("LLAVON_UI_CAPTURE_DIR");
        if (!path.isEmpty()) {
            QVERIFY(QDir().mkpath(path));
            QVERIFY(combo->view()->window()->grab().save(path + '/' + name + ".png"));
        }
    };
    for (const bool dark : {false, true}) {
        auto palette = original; palette.setColor(QPalette::Window, dark ? QColor("#202124") : QColor("#f5f5f7"));
        QApplication::setPalette(palette); QCoreApplication::processEvents();
        window.showPage("settings"); widget<QTabWidget>(window, "settingsGroups")->setCurrentIndex(1);
        keyboard->setFocus(); keyboard->showPopup();
        QTRY_VERIFY(keyboard->view()->isVisible());
        popupCapture(keyboard, dark ? "41-keyboard-menu-dark" : "40-keyboard-menu-light");
        const int previous = keyboard->currentIndex();
        QTest::keyClick(keyboard->view(), Qt::Key_End); QTest::keyClick(keyboard->view(), Qt::Key_Escape);
        QCOMPARE(keyboard->currentIndex(), previous); // Escape must cancel the pending choice.
        keyboard->showPopup(); QTRY_VERIFY(keyboard->view()->isVisible());
        const int target = previous == 0 ? 1 : 0;
        const auto index = keyboard->model()->index(target, keyboard->modelColumn());
        QTest::mouseClick(keyboard->view()->viewport(), Qt::LeftButton, Qt::NoModifier, keyboard->view()->visualRect(index).center());
        QTRY_COMPARE(keyboard->currentIndex(), target);
        const auto local = QPointF(keyboard->rect().center());
        QWheelEvent wheel(local, keyboard->mapToGlobal(local.toPoint()), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(keyboard, &wheel); QCOMPARE(keyboard->currentIndex(), target);
        const auto geometry = save->geometry(); save->setFocus(); QCoreApplication::processEvents();
        QCOMPARE(save->geometry(), geometry); // The focus ring cannot change the button's footprint.
        capture(window, dark ? "43-controls-dark" : "42-controls-light");
        auto* pageSize = widget<QSpinBox>(window, "setting_candidate_page_size");
        const auto oldSize = pageSize->value();
        QTest::mouseClick(pageSize, Qt::LeftButton, Qt::NoModifier, QPoint(pageSize->width() - 13, 8));
        QCOMPARE(pageSize->value(), oldSize + 1);
        QTest::mouseClick(pageSize, Qt::LeftButton, Qt::NoModifier, QPoint(pageSize->width() - 13, pageSize->height() - 8));
        QCOMPARE(pageSize->value(), oldSize);
        QTest::mouseMove(save, save->rect().center());
        QTest::mousePress(save, Qt::LeftButton);
        capture(window, dark ? "53-button-pressed-dark" : "52-button-pressed-light");
        QTest::mouseRelease(save, Qt::LeftButton);
        QCOMPARE(save->geometry(), geometry);
        window.showPage("phrases");
        auto* reading = phraseRow(window, 0)->findChild<QComboBox*>("phraseReading_1");
        reading->showPopup(); QTRY_VERIFY(reading->view()->isVisible());
        popupCapture(reading, dark ? "45-readings-menu-dark" : "44-readings-menu-light");
        const auto current = reading->currentIndex();
        QTest::keyClick(reading->view(), Qt::Key_Home); QTest::keyClick(reading->view(), Qt::Key_Return);
        QVERIFY(reading->currentIndex() >= 0); reading->setCurrentIndex(current);
        click(window, "savePhrases");
        window.showPage("training");
        QVERIFY(widget<QLabel>(window, "trainPasswordLabel")->isVisible() == widget<QLineEdit>(window, "trainPassword")->isVisible());
        auto* strength = widget<QComboBox>(window, "strength");
        strength->showPopup(); QTRY_VERIFY(strength->view()->isVisible());
        popupCapture(strength, dark ? "47-strength-menu-dark" : "46-strength-menu-light");
        QTest::keyClick(strength->view(), Qt::Key_Escape);
        auto* cancel = widget<QPushButton>(window, "cancelJob");
        QVERIFY(!cancel->isEnabled()); QSignalSpy cancelled(cancel, &QPushButton::clicked);
        QTest::mouseClick(cancel, Qt::LeftButton); QCOMPARE(cancelled.count(), 0);
        capture(window, dark ? "49-training-controls-dark" : "48-training-controls-light");
    }
    window.showPage("settings"); click(window, "saveSettings");
    QCOMPARE(store.load().value("keyboard_layout").toString(), keyboard->currentData().toString());
    window.resize(800, 620); widget<QTabWidget>(window, "settingsGroups")->setCurrentIndex(1);
    capture(window, "50-controls-compact");
    for (auto* combo : window.findChildren<QComboBox*>()) if (combo->isVisible()) {
        QVERIFY(window.rect().contains(QRect(combo->mapTo(&window, QPoint()), combo->size())));
    }
    QWidget standalone; QVBoxLayout layout(&standalone);
    llavon::lora::NativeComboBox longNames(&standalone); layout.addWidget(&longNames);
    llavon::lora::applyAppearance(&standalone); longNames.setFixedWidth(220);
    for (int i = 0; i < 14; ++i) longNames.addItem(QStringLiteral("訓練版本 %1 · ").arg(i) + QString(100, QChar(0x9577)));
    standalone.show(); QVERIFY(QTest::qWaitForWindowExposed(&standalone));
    longNames.setFocus(); longNames.showPopup(); QTRY_VERIFY(longNames.view()->isVisible());
    const auto closePopup = qScopeGuard([&longNames] { longNames.hidePopup(); });
    QVERIFY(longNames.view()->window()->height() < 380);
    QVERIFY(longNames.view()->width() <= 460);
    const auto last = longNames.model()->index(13, 0); longNames.view()->scrollTo(last);
    QCoreApplication::processEvents();
    popupCapture(&longNames, "51-long-names-menu");
    QTest::qWait(150);
    QCOMPARE(longNames.view()->indexAt(longNames.view()->visualRect(last).center()), last);
    QTest::mouseMove(longNames.view()->viewport(), longNames.view()->visualRect(last).center());
    QVERIFY(longNames.view()->viewport()->rect().contains(longNames.view()->visualRect(last).center()));
    QTRY_COMPARE(longNames.view()->currentIndex(), last);
    QTest::mouseClick(longNames.view()->viewport(), Qt::LeftButton, Qt::NoModifier, longNames.view()->visualRect(last).center());
    QTRY_COMPARE(longNames.currentIndex(), 13); QCOMPARE(longNames.toolTip(), longNames.currentText());
}

QTEST_MAIN(NativeTests)
#include "lora_native_tests.moc"
