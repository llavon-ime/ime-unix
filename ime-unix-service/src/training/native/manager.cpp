#include "manager.hpp"
#include "guide.hpp"
#include "history_graph.hpp"
#include "settings.hpp"
#include "appearance.hpp"
#include "controls.hpp"
#include "host_settings.hpp"
#ifdef Q_OS_LINUX
#include "linux_updates.hpp"
#endif

#include <QCheckBox>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

namespace llavon::lora {
namespace {
class SentenceDelegate final : public QStyledItemDelegate {
public:
    explicit SentenceDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto background = option;
        initStyleOption(&background, index);
        const auto lines = index.data(Qt::DisplayRole).toString().split('\n');
        background.text.clear();
        background.features &= ~QStyleOptionViewItem::HasDisplay;
        painter->save();
        option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
        painter->restore();
        painter->save();
        painter->setClipRect(option.rect);
        auto font = option.font; font.setPixelSize(16); font.setWeight(QFont::Medium);
        painter->setFont(font); painter->setPen(appearance().text);
        const auto rect = option.rect.adjusted(8, 0, -8, 0);
        painter->drawText(rect.left(), rect.top() + 26,
            QFontMetrics(font).elidedText(lines.value(0), Qt::ElideRight, rect.width()));
        if (lines.size() > 1) {
            font.setPixelSize(11); font.setWeight(QFont::Normal);
            painter->setFont(font); painter->setPen(appearance().secondary);
            painter->drawText(rect.left(), rect.top() + 48,
                QFontMetrics(font).elidedText(lines.mid(1).join(' '), Qt::ElideRight, rect.width()));
        }
        painter->restore();
    }
};
QLabel* label(const QString& text, const char* role = nullptr) {
    auto* widget = new QLabel(text);
    widget->setTextFormat(Qt::PlainText);
    widget->setWordWrap(true);
    if (role) widget->setProperty("role", role);
    return widget;
}
QPushButton* button(const QString& text, const char* name, bool primary = false) {
    auto* widget = new QPushButton(text);
    widget->setObjectName(name);
    widget->setAutoDefault(false);
    if (primary) widget->setProperty("primary", true);
    widget->setMinimumHeight(34);
    widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return widget;
}
QLineEdit* secret(const char* name, const QString& placeholder) {
    auto* widget = new QLineEdit;
    widget->setObjectName(name);
    widget->setEchoMode(QLineEdit::Password);
    widget->setPlaceholderText(placeholder);
    widget->setMinimumHeight(32);
    return widget;
}
QWidget* scrollPage(QWidget* content) {
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}
QString text(const QJsonValue& value) {
    return value.isString() ? value.toString() : QString::number(value.toDouble());
}
QString timeLabel(const QString& value) {
    const auto date = QDateTime::fromString(value, Qt::ISODate);
    return date.isValid() ? date.toLocalTime().toString("MM/dd  HH:mm") : value;
}
}

Manager::Manager(Backend* backend, const QString& hostHelper, const QString& initialPage) : backend_(backend) {
    setWindowTitle(QStringLiteral("拉風輸入法"));
    resize(1080, 760);
    setMinimumSize(800, 620);
    auto* shell = new QWidget;
    auto* horizontal = new QHBoxLayout(shell);
    horizontal->setContentsMargins(0, 0, 0, 0);
    horizontal->setSpacing(0);
    auto* sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(204);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(12, 26, 12, 18);
    side->addWidget(label(QStringLiteral("拉風"), "brand"));
    side->addSpacing(24);
    auto* navigation = new QListWidget;
    navigation->setObjectName("navigation");
    const QStringList sections{QStringLiteral("訓練資料"), QStringLiteral("模型與訓練"), QStringLiteral("訓練歷程"), QStringLiteral("輸入法設定"), QStringLiteral("替代詞彙"), QStringLiteral("軟體更新"), QStringLiteral("版本與狀態")};
    for (const auto index : {3, 4, 0, 1, 2}) {
        auto* item = new QListWidgetItem(sections[index], navigation); item->setData(Qt::UserRole, index);
        item->setIcon(navigationIcon(index));
    }
    auto* updateItem = new QListWidgetItem(sections[5], navigation); updateItem->setData(Qt::UserRole, 5);
    updateItem->setIcon(navigationIcon(5));
    auto* aboutItem = new QListWidgetItem(sections[6], navigation); aboutItem->setData(Qt::UserRole, 6); aboutItem->setIcon(navigationIcon(6));
    navigation->setIconSize(QSize(26, 26));
    navigation->setSpacing(2);
    navigation->setMinimumHeight(300);
    side->addWidget(navigation, 1);
    guideButton_ = button(QStringLiteral("使用指南"), "openGuide");
    guideButton_->setProperty("quiet", true);
    side->addWidget(guideButton_);
    connect(guideButton_, &QPushButton::clicked, this, [this] { showPage("guide"); });
    message_ = label({});
    message_->setObjectName("notice");
    side->addWidget(message_); message_->hide();
    horizontal->addWidget(sidebar);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 26, 28, 20);
    layout->setSpacing(22);
    auto* titleRow = new QHBoxLayout;
    heading_ = label({}, "title");
    heading_->setObjectName("pageHeading");
    titleRow->addWidget(heading_, 1);
    auto* refreshButton = button(QStringLiteral("重新整理"), "refresh");
    refreshButton->setProperty("quiet", true);
    titleRow->addWidget(refreshButton);
    layout->addLayout(titleRow);
    quickStartDismissed_ = QuickStartPreferences::dismissed();
    quickStart_ = new QFrame; quickStart_->setObjectName("quickStart");
    auto* welcome = new QHBoxLayout(quickStart_);
    welcome->setContentsMargins(16, 12, 16, 12); welcome->setSpacing(8);
    welcome->addWidget(label(QStringLiteral("功能導覽"), "section"), 1);
    auto* begin = button(QStringLiteral("認識功能"), "startQuickGuide", true);
    auto* skip = button(QStringLiteral("略過全部"), "skipQuickGuide"); skip->setProperty("quiet", true);
    welcome->addWidget(begin); welcome->addWidget(skip);
    layout->addWidget(quickStart_); quickStart_->hide();
    connect(begin, &QPushButton::clicked, this, [this] { showPage("guide"); guide_->startQuickGuide(); });
    connect(skip, &QPushButton::clicked, this, &Manager::dismissQuickStart);
    pages_ = new QStackedWidget;
    layout->addWidget(pages_, 1);
    for (int index = 0; index < 3; ++index) pages_->addWidget(new QWidget);
    settings_ = new SettingsPage(false);
    pages_->addWidget(settings_); pages_->addWidget(new QWidget);
#ifdef Q_OS_LINUX
    updates_ = new LinuxUpdatesPage([this] { return running_ || actionPending_ || state_.isEmpty(); });
    pages_->addWidget(updates_);
#else
    auto* macUpdates = new HostSettingsPage(true, nullptr, hostHelper); macUpdates->setObjectName("macUpdates");
    pages_->addWidget(macUpdates);
#endif
    auto* hostStatus = new HostSettingsPage(false, nullptr, hostHelper); hostStatus->setObjectName("hostDiagnostics");
    pages_->addWidget(hostStatus);
    horizontal->addWidget(content, 1);
    setCentralWidget(shell);
    connect(navigation, &QListWidget::currentRowChanged, this, [this, navigation](int row) {
        if (row < 0) return;
        const int index = navigation->item(row)->data(Qt::UserRole).toInt();
        quickStart_->setVisible(index == 3 && !quickStartDismissed_);
        if (index < 3) ensurePersonalizationPages();
        const bool firstPhraseVisit = index == 4 && !phrases_;
        if (firstPhraseVisit) {
            // Reading tables and building every phrase row are unrelated to
            // opening Settings. Keep that work on the first visit to Phrases.
            phrases_ = new SettingsPage(true, nullptr, backend_->phraseTablePath());
            auto* placeholder = pages_->widget(4); pages_->removeWidget(placeholder);
            pages_->insertWidget(4, phrases_); delete placeholder;
        }
        pages_->setCurrentIndex(index);
        const QStringList headings{QStringLiteral("訓練資料"), QStringLiteral("模型與訓練"), QStringLiteral("訓練歷程"), QStringLiteral("輸入法設定"), QStringLiteral("替代詞彙"), QStringLiteral("軟體更新"), QStringLiteral("版本與狀態")};
        heading_->setText(headings.value(index));
        if (index == 3 && !settings_->isDirty()) settings_->reload();
        if (index == 4 && !firstPhraseVisit && !phrases_->isDirty()) phrases_->reload();
        if (auto* host = qobject_cast<HostSettingsPage*>(pages_->currentWidget())) host->refresh();
    });
    connect(refreshButton, &QPushButton::clicked, this, &Manager::refresh);
    connect(backend_, &Backend::ready, this, &Manager::refresh);
    connect(backend_, &Backend::error, this, [this](const QString& error) { notice(error, true); });
    auto* shortcut = new QShortcut(QKeySequence::Refresh, this);
    connect(shortcut, &QShortcut::activated, this, &Manager::refresh);
    timer_ = new QTimer(this);
    timer_->setInterval(3000);
    connect(timer_, &QTimer::timeout, this, &Manager::refresh);
    applyAppearance(this);
    qApp->installEventFilter(this);
    showPage(initialPage);
}

void Manager::ensurePersonalizationPages() {
    if (personalizationReady_) return;
    const QList<QWidget*> content{recordsPage(), trainingPage(), historyPage()};
    for (int index = 0; index < content.size(); ++index) {
        auto* placeholder = pages_->widget(index); pages_->removeWidget(placeholder);
        pages_->insertWidget(index, content[index]); delete placeholder;
    }
    personalizationReady_ = true;
    notice(QStringLiteral("正在連接…"));
    for (auto* action : jobButtons_) action->setEnabled(false);
    timer_->start();
    QTimer::singleShot(0, backend_, &Backend::start);
}

bool Manager::eventFilter(QObject* watched, QEvent* event) {
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        applyAppearance(this);
        if (guide_) applyAppearance(guide_);
        if (history_) history_->viewport()->update();
    }
    return QMainWindow::eventFilter(watched, event);
}

void Manager::showPage(const QString& page) {
    if (page == "guide") {
        if (!guide_) {
            guide_ = new GuidePage(this);
            applyAppearance(guide_);
            connect(guide_, &GuidePage::pageRequested, this, [this](const QString& destination) {
                guide_->hide(); showPage(destination); raise(); activateWindow();
            });
            connect(guide_, &GuidePage::settingRequested, this, [this](const QString& key) {
                guide_->hide(); showPage("settings"); raise(); activateWindow(); settings_->revealField(key);
            });
            const auto finish = [this] {
                dismissQuickStart(); guide_->hide(); guide_->startQuickGuide();
                raise(); activateWindow();
            };
            connect(guide_, &GuidePage::finished, this, finish);
            connect(guide_, &GuidePage::skipped, this, finish);
        }
        quickStart_->hide();
        guide_->show(); guide_->raise(); guide_->activateWindow();
        return;
    }
    const QStringList names{"records", "training", "history", "settings", "phrases", "updates", "about"};
    const auto index = names.indexOf(page);
    auto* navigation = findChild<QListWidget*>("navigation");
    for (int row = 0; row < navigation->count(); ++row)
        if (index >= 0 && navigation->item(row)->data(Qt::UserRole).toInt() == index) navigation->setCurrentRow(row);
}

void Manager::dismissQuickStart() {
    // Do not write engine settings, enable collection or start the backend.
    quickStartDismissed_ = true; quickStart_->hide();
    if (!QuickStartPreferences::dismiss())
        notice(QStringLiteral("無法儲存教學偏好；這次已略過，下次開啟可能再次顯示。"), true);
}

QWidget* Manager::recordsPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    collection_ = label({}, "muted");
    layout->addWidget(collection_);
    auto* secrets = recordingCredentials();
    password_ = secrets->findChild<QLineEdit*>("reviewPassword");
    confirmation_ = secrets->findChild<QLineEdit*>("confirmation");
    unlock_ = secrets->findChild<QPushButton*>("unlock");
    setup_ = secrets->findChild<QPushButton*>("setup");
    lock_ = secrets->findChild<QPushButton*>("lock");
    recording_ = secrets->findChild<QPushButton*>("recording");
    layout->addWidget(secrets);
    auto* controls = new QHBoxLayout;
    filter_ = new NativeComboBox;
    filter_->setObjectName("recordFilter");
    filter_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    filter_->setMinimumWidth(132);
    filter_->addItem(QStringLiteral("未訓練"), "pending");
    filter_->addItem(QStringLiteral("已訓練"), "trained");
    filter_->addItem(QStringLiteral("已排除"), "excluded");
    manualView_ = new NativeCheckBox(QStringLiteral("僅顯示手動選字"));
    manualView_->setObjectName("manualView");
    controls->addWidget(filter_);
    controls->addWidget(manualView_);
    controls->addStretch();
    counts_ = label({}, "muted");
    counts_->setWordWrap(false);
    controls->addWidget(counts_);
    layout->addLayout(controls);
    emptyRecords_ = label(QStringLiteral("尚無輸入紀錄"), "muted");
    layout->addWidget(emptyRecords_);
    records_ = new QTableWidget(0, 4);
    records_->setObjectName("records");
    records_->setHorizontalHeaderLabels({QStringLiteral("時間"), QStringLiteral("輸入內容 / 前文"), QStringLiteral("注音"), QStringLiteral("選字")});
    records_->setSelectionBehavior(QAbstractItemView::SelectRows);
    records_->setSelectionMode(QAbstractItemView::SingleSelection);
    records_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    records_->setAlternatingRowColors(true);
    records_->setItemDelegateForColumn(1, new SentenceDelegate(records_));
    records_->setShowGrid(false);
    records_->verticalHeader()->hide();
    records_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    records_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    records_->setColumnWidth(0, 105);
    records_->setColumnWidth(2, 160);
    records_->setColumnWidth(3, 85);
    layout->addWidget(records_, 1);
    auto* footer = new QHBoxLayout;
    exclude_ = button(QStringLiteral("排除"), "exclude");
    delete_ = button(QStringLiteral("刪除…"), "delete");
    delete_->setProperty("destructive", true);
    footer->addWidget(exclude_);
    footer->addWidget(delete_);
    footer->addStretch();
    pageInfo_ = label({}, "muted");
    previous_ = button(QStringLiteral("上一頁"), "previous");
    next_ = button(QStringLiteral("下一頁"), "next");
    footer->addWidget(pageInfo_);
    footer->addWidget(previous_);
    footer->addWidget(next_);
    layout->addLayout(footer);
    forget_ = button(QStringLiteral("忘記密碼，清除所有對話資料…"), "forget");
    forget_->setFlat(true);
    forget_->setProperty("quiet", true); forget_->setProperty("destructive", true);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(forget_);
    bottom->addStretch();
    layout->addLayout(bottom);
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] { offset_ = 0; refresh(); });
    connect(manualView_, &QCheckBox::toggled, this, [this] { offset_ = 0; refresh(); });
    connect(previous_, &QPushButton::clicked, this, [this] { offset_ = qMax(0, offset_ - 20); refresh(); });
    connect(next_, &QPushButton::clicked, this, [this] { offset_ += 20; refresh(); });
    connect(records_, &QTableWidget::itemSelectionChanged, this, &Manager::updateRecordActions);
    connect(unlock_, &QPushButton::clicked, this, [this] {
        const auto password = password_->text(); password_->clear();
        act("/api/unlock", {{"password", password}});
    });
    connect(password_, &QLineEdit::returnPressed, this, [this] { if (unlock_->isVisible()) unlock_->click(); else setup_->click(); });
    connect(setup_, &QPushButton::clicked, this, [this] {
        QJsonObject body{{"action", "set-password"}, {"password", password_->text()}, {"confirmation", confirmation_->text()}};
        password_->clear(); confirmation_->clear();
        act("/api/protection", body);
    });
    connect(lock_, &QPushButton::clicked, this, [this] { act("/api/lock"); });
    connect(recording_, &QPushButton::clicked, this, [this] {
        act("/api/protection", {{"action", protection_.value("enabled").toBool() ? "disable" : "enable"}});
    });
    connect(forget_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(this, QStringLiteral("清除對話資料"),
            QStringLiteral("這會永久刪除所有對話紀錄與資料密碼。模型與訓練歷程會保留。"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            act("/api/protection", {{"action", "forget"}});
    });
    connect(exclude_, &QPushButton::clicked, this, [this] { act("/api/records/" + selectedRecord() + "/exclude"); });
    connect(delete_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("刪除這筆資料"), QStringLiteral("永久刪除選取的句子與注音？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            act("/api/records/" + selectedRecord() + "/delete");
    });
    updateRecordActions();
    return page;
}

QWidget* Manager::trainingPage() {
    auto* page = new QWidget;
    page->setObjectName("trainingContent");
    page->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(label(QStringLiteral("準備環境"), "section"));
    const auto resource = [this, layout](const QString& title, QLabel*& description,
                                       const QString& action, const char* name, const QString& path,
                                       const QString& checkPath) {
        auto* surface = new QFrame; surface->setObjectName("resourceRow");
        auto* row = new QHBoxLayout(surface); row->setContentsMargins(16, 12, 16, 12);
        auto* info = new QVBoxLayout;
        info->addWidget(label(title));
        description = label(QStringLiteral("檢查中…"), "muted");
        info->addWidget(description);
        row->addLayout(info, 1);
        auto* check = button(QStringLiteral("檢查版本"), (QString(name) + "Check").toUtf8().constData());
        auto* download = button(action, name);
        row->addWidget(check); row->addWidget(download);
        jobButtons_ << check << download;
        connect(check, &QPushButton::clicked, this, [this, checkPath] { act(checkPath); });
        connect(download, &QPushButton::clicked, this, [this, path] { act(path); });
        layout->addWidget(surface);
    };
    resource(QStringLiteral("基礎模型"), model_, QStringLiteral("下載 / 更新"), "fetch", "/api/fetch", "/api/check");
    resource(QStringLiteral("LoRA 訓練器"), trainer_, QStringLiteral("安裝 / 更新"), "installTrainer", "/api/install-trainer", "/api/check-trainer");
    device_ = label({}, "muted");
    layout->addWidget(device_);
    layout->addSpacing(4);
    layout->addWidget(label(QStringLiteral("這次如何學習"), "section"));
    auto* form = new QFormLayout;
    form->setVerticalSpacing(12); form->setHorizontalSpacing(20);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    strength_ = new NativeComboBox;
    strength_->setObjectName("strength");
    const QStringList names{"ultra-low", "low", "medium", "high", "advanced"};
    const QStringList titles{QStringLiteral("極低 · 微幅調整"), QStringLiteral("低 · 保守學習"),
        QStringLiteral("中 · 加強個人用字"), QStringLiteral("高 · 明顯調整"), QStringLiteral("進階 · 自訂參數")};
    for (int i = 0; i < names.size(); ++i) strength_->addItem(titles[i], names[i]);
    base_ = new NativeComboBox;
    base_->setObjectName("trainingBase");
    base_->addItem(QStringLiteral("Base model · 從頭開始"), "");
    strength_->setMaximumWidth(400); base_->setMaximumWidth(400);
    form->addRow(QStringLiteral("訓練強度"), strength_);
    form->addRow(QStringLiteral("訓練基底"), base_);
    trainPassword_ = secret("trainPassword", QStringLiteral("開始訓練前驗證資料密碼"));
    trainPassword_->setMaximumWidth(400);
    trainPasswordLabel_ = label(QStringLiteral("資料密碼")); trainPasswordLabel_->setObjectName("trainPasswordLabel");
    trainPasswordLabel_->setBuddy(trainPassword_); form->addRow(trainPasswordLabel_, trainPassword_);
    layout->addLayout(form);
    manualTrain_ = new NativeCheckBox(QStringLiteral("只訓練曾手動選字的句子"));
    manualTrain_->setObjectName("manualTrain"); manualTrain_->setChecked(true);
    stabilize_ = new NativeCheckBox(QStringLiteral("降低模型遺忘（實驗性）"));
    stabilize_->setObjectName("stabilize"); stabilize_->setChecked(true);
    layout->addWidget(manualTrain_);
    layout->addWidget(stabilize_);
    auto* advanced = new QGroupBox(QStringLiteral("進階參數"));
    advanced->setObjectName("advanced");
    auto* advancedForm = new QFormLayout(advanced);
    const QList<QStringList> fields{
        {"rank", "Rank", "8"}, {"alpha", "Alpha", "16"}, {"dropout", "Dropout", "0"},
        {"batch-size", QStringLiteral("批次大小"), "1"}, {"gradient-accumulation", QStringLiteral("梯度累積"), "1"},
        {"epochs", QStringLiteral("訓練輪數"), "1"}, {"max-steps", QStringLiteral("最大步數"), "0"},
        {"learning-rate", QStringLiteral("學習率"), "1e-8"}, {"weight-decay", QStringLiteral("權重衰減"), "0"},
        {"warmup-steps", QStringLiteral("暖身步數"), "0"}, {"max-grad-norm", QStringLiteral("梯度上限"), "1"},
        {"save-every", QStringLiteral("儲存間隔"), "0"}, {"seed", QStringLiteral("隨機種子"), "42"},
        {"max-seq-length", QStringLiteral("序列長度"), "384"}, {"target-modules", QStringLiteral("目標層"), "q_proj,v_proj"},
        {"device", QStringLiteral("裝置 (auto/cpu/cuda/mps)"), "auto"},
        {"dtype", QStringLiteral("精度 (float32/bfloat16)"), "float32"}, {"shuffle", QStringLiteral("打亂資料 (1/0)"), "1"}};
    for (const auto& field : fields) {
        auto* input = new QLineEdit(field[2]); input->setObjectName(field[0]);
        input->setMaxLength(100); options_.insert(field[0], input);
        advancedForm->addRow(field[1], input);
    }
    advanced->hide();
    layout->addWidget(advanced);
    connect(strength_, &QComboBox::currentIndexChanged, this, [this, advanced] {
        advanced->setVisible(strength_->currentData() == "advanced");
    });
    auto* actions = new QHBoxLayout;
    start_ = button(QStringLiteral("開始訓練"), "startTraining", true);
    cancel_ = button(QStringLiteral("取消工作"), "cancelJob"); cancel_->setEnabled(false);
    actions->addWidget(start_); actions->addWidget(cancel_); actions->addStretch();
    layout->addLayout(actions);
    job_ = label(QStringLiteral("尚無執行中的工作"), "muted");
    progress_ = new QProgressBar; progress_->setTextVisible(false);
    log_ = new QPlainTextEdit; log_->setReadOnly(true); log_->setMaximumBlockCount(1000); log_->setMinimumHeight(110);
    log_->setObjectName("jobLog");
    log_->setPlaceholderText(QStringLiteral("下載、安裝與訓練的詳細紀錄會顯示在這裡。"));
    layout->addWidget(job_); layout->addWidget(progress_); layout->addWidget(log_);
    job_->hide(); progress_->hide(); log_->hide();
    layout->addStretch();
    connect(start_, &QPushButton::clicked, this, &Manager::train);
    connect(cancel_, &QPushButton::clicked, this, [this] { act("/api/cancel"); });
    connect(base_, &QComboBox::currentIndexChanged, this, [this] {
        const auto run = base_->currentData(Qt::UserRole + 1).toJsonObject();
        for (const auto& key : {"rank", "alpha", "dropout", "target_modules"}) {
            QString field = key; field.replace('_', '-');
            if (run.contains(key)) options_[field]->setText(text(run.value(key)));
        }
    });
    return scrollPage(page);
}

QWidget* Manager::historyPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0);
    auto* tools = new QHBoxLayout;
    tools->addStretch();
    auto* smaller = button(QStringLiteral("－"), "zoomOut");
    auto* larger = button(QStringLiteral("＋"), "zoomIn");
    smaller->setFixedWidth(34); larger->setFixedWidth(34);
    smaller->setProperty("compact", true); larger->setProperty("compact", true);
    smaller->setAccessibleName(QStringLiteral("縮小歷程")); larger->setAccessibleName(QStringLiteral("放大歷程"));
    auto* zoom = label("100%", "muted"); zoom->setMinimumWidth(42); zoom->setWordWrap(false);
    auto* reset = button(QStringLiteral("重設視圖"), "resetGraph");
    auto* arrange = button(QStringLiteral("整理節點"), "arrangeGraph");
    tools->addWidget(smaller); tools->addWidget(zoom); tools->addWidget(larger);
    tools->addWidget(reset); tools->addWidget(arrange);
    layout->addLayout(tools);
    history_ = new HistoryGraph;
    const auto graphHelp = QStringLiteral("拖曳空白平移 · 拖曳節點調整位置 · Ctrl + 滾輪縮放\n+ / − 縮放、0 重設視圖、[ / ] 切換節點、方向鍵微調位置。");
    history_->setToolTip(graphHelp); history_->setAccessibleDescription(graphHelp);
    reset->setToolTip(graphHelp);
    connect(smaller, &QPushButton::clicked, this, [this] { history_->zoomBy(0.8); });
    connect(larger, &QPushButton::clicked, this, [this] { history_->zoomBy(1.25); });
    connect(reset, &QPushButton::clicked, history_, &HistoryGraph::resetView);
    connect(arrange, &QPushButton::clicked, this, [this] { history_->arrangeNodes(); history_->resetView(); });
    connect(history_, &HistoryGraph::zoomChanged, zoom, [zoom](int percent) { zoom->setText(QString::number(percent) + "%"); });
    layout->addWidget(history_, 1);
    historyDetail_ = label({}, "muted");
    historyDetail_->setMinimumHeight(44);
    historyDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(historyDetail_);
    auto* actions = new QHBoxLayout;
    auto* useBase = button(QStringLiteral("設為訓練基底"), "setBase");
    auto* apply = button(QStringLiteral("套用這個模型"), "applyModel", true);
    ancestor_ = button(QStringLiteral("共同祖先"), "ancestor"); ancestor_->hide();
    actions->addWidget(useBase); actions->addWidget(apply); actions->addWidget(ancestor_); actions->addStretch();
    layout->addLayout(actions);
    connect(useBase, &QPushButton::clicked, this, [this] {
        const auto id = selectedRun(); const auto index = base_->findData(id == "base" ? "" : id);
        if (index >= 0) { base_->setCurrentIndex(index); showPage("training"); }
    });
    connect(apply, &QPushButton::clicked, this, [this] { act("/api/use-model", {{"id", selectedRun()}}); });
    connect(ancestor_, &QPushButton::clicked, this, [this] {
        const auto id = ancestor_->property("runId").toString();
        history_->selectRun(id == "0" ? "base" : id);
    });
    connect(history_, &HistoryGraph::runSelected, this, [this, useBase, apply](const QString& id) {
        useBase->setEnabled(!id.isEmpty() && !actionPending_); apply->setEnabled(!id.isEmpty() && !running_ && !actionPending_);
        if (id.isEmpty()) { historyDetail_->clear(); historyDetail_->setToolTip({}); return; }
        if (id == "base") {
            // Keep the details slot stable while removing text already on the
            // base node; toggling the slot would shift the graph's viewport.
            historyDetail_->clear();
            historyDetail_->setToolTip({}); return;
        }
        const auto run = historyRuns_.value(id);
        const auto parent = text(run.value("parent_id"));
        historyDetail_->setText(QStringLiteral("訓練 #%1  ←  %2\nRank %3 · Alpha %4 · Dropout %5 · %6")
            .arg(id, parent == "0" ? "Base model" : "#" + parent, text(run.value("rank")),
                 text(run.value("alpha")), text(run.value("dropout")), run.value("target_modules").toString()));
        historyDetail_->setToolTip(run.value("model_path").toString() + "\n" +
            QString::fromUtf8(QJsonDocument(run.value("request").toObject()).toJson(QJsonDocument::Indented)));
    });
    return page;
}

void Manager::notice(const QString& message, bool error) {
    message_->setText(message);
    message_->setVisible(!message.isEmpty());
    message_->setToolTip(message);
    message_->setStyleSheet(error ? QString("color:%1;").arg(appearance().error.name()) : QString());
}

void Manager::refresh() {
    if (!personalizationReady_) {
        if (auto* host = qobject_cast<HostSettingsPage*>(pages_->currentWidget())) host->refresh();
        else if (pages_->currentWidget() == settings_ && !settings_->isDirty()) settings_->reload();
        else if (phrases_ && pages_->currentWidget() == phrases_ && !phrases_->isDirty()) phrases_->reload();
        return;
    }
    if (refreshPending_ || actionPending_) return;
    refreshPending_ = 5;
    const auto done = [this] { if (--refreshPending_ == 0) emit refreshed(); };
    backend_->request("/api/state", {}, [this, done](const QJsonValue& value) {
        if (value.isObject()) renderState(value.toObject()); done();
    });
    backend_->request("/api/protection", {}, [this, done](const QJsonValue& value) {
        if (value.isObject()) renderProtection(value.toObject()); done();
    });
    backend_->request("/api/pending-count", {}, [this, done](const QJsonValue& value) {
        if (value.isObject()) {
            const auto totals = value.toObject();
            counts_->setText(QStringLiteral("手動選字 %1").arg(totals.value("manual").toInt()));
            const QStringList names{QStringLiteral("未訓練"), QStringLiteral("已訓練"), QStringLiteral("已排除")};
            const QStringList keys{"count", "trained", "excluded"};
            for (int i = 0; i < 3; ++i) filter_->setItemText(i, names[i] + "  " + QString::number(totals.value(keys[i]).toInt()));
        } done();
    });
    const QString path = QStringLiteral("/api/records?state=%1&offset=%2&manual=%3")
        .arg(filter_->currentData().toString()).arg(offset_).arg(manualView_->isChecked() ? 1 : 0);
    // The existing filter parser accepts manual=1; omit the parameter otherwise.
    backend_->request(manualView_->isChecked() ? path : path.left(path.lastIndexOf('&')), {}, [this, done](const QJsonValue& value) {
        if (value.isObject()) renderRecords(value.toObject()); done();
    });
    backend_->request("/api/history", {}, [this, done](const QJsonValue& value) {
        if (value.isObject()) renderHistory(value.toObject()); done();
    });
}

void Manager::act(const QString& path, const QJsonObject& body, Backend::Reply reply) {
#ifdef Q_OS_LINUX
    if (updates_ && updates_->busy() &&
        (path == "/api/train" || path == "/api/export" || path == "/api/fetch" || path == "/api/install")) {
        notice(QStringLiteral("請等軟體更新完成，或先取消等待更新。")); return;
    }
#endif
    if (actionPending_) return;
    actionPending_ = true; updateRecordActions();
    backend_->request(path, body, [this, onSuccess = std::move(reply)](const QJsonValue& value) {
        actionPending_ = false;
        if (!value.isUndefined() && !value.isNull()) {
            notice(QStringLiteral("已更新"));
            if (onSuccess) onSuccess(value);
        }
        updateRecordActions();
        // Drain any in-flight refresh before requesting the post-action state.
        QTimer::singleShot(100, this, &Manager::refresh);
    }, true);
}

void Manager::renderProtection(const QJsonObject& data) {
    protection_ = data;
    const bool configured = data.value("configured").toBool(), unlocked = data.value("unlocked").toBool();
    collection_->setText(!configured ? QStringLiteral("收集尚未啟用。請妥善保存資料密碼。") :
        QStringLiteral("%1 · %2").arg(data.value("enabled").toBool() ? QStringLiteral("正在加密收集") : QStringLiteral("已暫停收集"),
                                   unlocked ? QStringLiteral("已解鎖檢視") : QStringLiteral("文字已鎖定")));
    password_->setVisible(!unlocked); confirmation_->setVisible(!configured); setup_->setVisible(!configured);
    unlock_->setVisible(configured && !unlocked); lock_->setVisible(unlocked);
    recording_->setVisible(configured); forget_->setVisible(configured);
    recording_->setText(data.value("enabled").toBool() ? QStringLiteral("暫停收集") : QStringLiteral("恢復收集"));
    trainPassword_->setVisible(configured);
    trainPasswordLabel_->setVisible(configured);
}

QString Manager::selectedRecord() const {
    const int row = records_->currentRow();
    return row >= 0 && records_->item(row, 0) ? records_->item(row, 0)->data(Qt::UserRole).toString() : QString();
}
void Manager::updateRecordActions() {
    const bool enabled = !selectedRecord().isEmpty() && filter_->currentData() == "pending" && !actionPending_ && !running_;
    exclude_->setEnabled(enabled); delete_->setEnabled(enabled);
}

void Manager::renderRecords(const QJsonObject& data) {
    const auto selected = selectedRecord();
    const auto rows = data.value("rows").toArray();
    records_->setRowCount(static_cast<int>(rows.size()));
    int index = 0;
    for (const auto value : rows) {
        const auto row = value.toObject();
        const auto id = row.value("id").toString();
        auto* stamp = new QTableWidgetItem(timeLabel(row.value("committed_at").toString()));
        stamp->setData(Qt::UserRole, id); records_->setItem(index, 0, stamp);
        const bool unlocked = row.value("text").toBool();
        const auto answer = unlocked ? row.value("answer").toString() : QStringLiteral("已加密 · 解鎖後檢視");
        const auto context = row.value("context").toString();
        auto* sentence = new QTableWidgetItem(answer + (context.isEmpty() ? "" : "\n" + context));
        sentence->setToolTip(answer + "\n" + context); records_->setItem(index, 1, sentence);
        QStringList readings; for (const auto reading : row.value("readings").toArray()) readings << reading.toString();
        auto* phonetics = new QTableWidgetItem(readings.join(" "));
        phonetics->setToolTip(readings.join(" ")); records_->setItem(index, 2, phonetics);
        bool manual = false; for (const auto flag : row.value("manual").toArray()) manual = manual || flag.toBool();
        records_->setItem(index, 3, new QTableWidgetItem(!unlocked ? QStringLiteral("鎖定") :
            skippedIds_.contains(id) ? QStringLiteral("無法轉換") : manual ? QStringLiteral("手動選字") : QStringLiteral("一般")));
        records_->setRowHeight(index, context.isEmpty() ? 48 : 64);
        if (id == selected) records_->selectRow(index);
        ++index;
    }
    const int total = data.value("total").toInt();
    emptyRecords_->setVisible(rows.isEmpty());
    emptyRecords_->setText(protection_.value("configured").toBool() ? QStringLiteral("尚無符合篩選的紀錄") : QStringLiteral("尚無輸入紀錄"));
    pageInfo_->setText(total ? QStringLiteral("%1–%2 / %3 筆").arg(offset_ + 1).arg(offset_ + rows.size()).arg(total) : QString());
    pageInfo_->setVisible(total > 0);
    previous_->setEnabled(offset_ > 0); next_->setEnabled(data.value("has_more").toBool());
    updateRecordActions();
}

void Manager::renderState(const QJsonObject& data) {
    const auto oldJob = state_.value("job").toObject();
    state_ = data;
    const auto job = data.value("job").toObject();
    running_ = job.value("state") == "running";
    bool updateBusy = false;
#ifdef Q_OS_LINUX
    updateBusy = updates_ && updates_->busy();
#endif
    model_->setText(data.value("model_ready").toBool() ? QStringLiteral("已就緒 · %1").arg(data.value("revision").toString().left(12)) : QStringLiteral("尚未下載 · 訓練需要未量化的 checkpoint"));
    trainer_->setText(data.value("trainer_ready").toBool() ? QStringLiteral("已就緒 · %1").arg(data.value("trainer_version").toString()) : QStringLiteral("尚未安裝，或版本與目前輸入法不符"));
    if (data.value("model_update_available").isBool()) model_->setText(model_->text() + (data.value("model_update_available").toBool() ? QStringLiteral(" · 有更新") : QStringLiteral(" · 已是固定版本")));
    if (data.value("trainer_update_available").isBool()) trainer_->setText(trainer_->text() + (data.value("trainer_update_available").toBool() ? QStringLiteral(" · 有更新") : QStringLiteral(" · 已是固定版本")));
    const QMap<QString, QString> devices{{"apple", "Apple Silicon / Metal"}, {"amd", "AMD / ROCm"}, {"nvidia", "NVIDIA / CUDA"}, {"none", "CPU"}};
    device_->setText(QStringLiteral("訓練裝置 · %1").arg(devices.value(data.value("gpu").toString(), QStringLiteral("自動偵測"))));
    device_->setToolTip(QStringLiteral("後端由訓練器自動選擇"));
    const QMap<QString, QString> kinds{{"fetch", QStringLiteral("下載基礎模型")}, {"install", QStringLiteral("安裝訓練器")},
        {"train", QStringLiteral("個人化訓練")}, {"export", QStringLiteral("匯出模型")}, {"check", QStringLiteral("檢查模型版本")}, {"trainer-check", QStringLiteral("檢查訓練器版本")}};
    const QMap<QString, QString> states{{"running", QStringLiteral("進行中")}, {"completed", QStringLiteral("已完成")}, {"failed", QStringLiteral("失敗")}, {"cancelled", QStringLiteral("已取消")}, {"idle", QStringLiteral("尚無執行中的工作")}};
    job_->setText(kinds.value(job.value("kind").toString()) + " " + states.value(job.value("state").toString()) + " " + job.value("progress").toString());
    const bool indeterminate = running_ && job.value("percent").isNull();
    progress_->setRange(0, indeterminate ? 0 : 100);
    progress_->setValue(job.value("state") == "completed" ? 100 : job.value("percent").toInt());
    const auto log = job.value("log").toString();
    const bool idle = job.value("state") == "idle";
    job_->setVisible(!idle); progress_->setVisible(!idle);
    log_->setVisible(running_ || !log.isEmpty());
    const auto previousLog = log_->toPlainText();
    if (previousLog != log) {
        auto* scroll = log_->verticalScrollBar();
        const bool newJob = oldJob.value("kind") != job.value("kind") ||
            (running_ && oldJob.value("state") != "running");
        const bool follow = newJob || scroll->value() >= scroll->maximum() - 1;
        const int position = scroll->value();
        const int cursorPosition = log_->textCursor().position(), anchor = log_->textCursor().anchor();
        if (!newJob && log.startsWith(previousLog)) {
            QTextCursor end(log_->document()); end.movePosition(QTextCursor::End);
            end.insertText(log.mid(previousLog.size()));
        } else {
            log_->setPlainText(log);
            if (!follow) {
                QTextCursor cursor(log_->document());
                const int last = log_->document()->characterCount() - 1;
                cursor.setPosition(qMin(anchor, last)); cursor.setPosition(qMin(cursorPosition, last), QTextCursor::KeepAnchor);
                log_->setTextCursor(cursor);
            }
        }
        if (follow) {
            log_->moveCursor(QTextCursor::End); log_->ensureCursorVisible();
            scroll->setValue(scroll->maximum());
        } else scroll->setValue(position);
    }
    cancel_->setEnabled(running_ && !actionPending_);
    start_->setEnabled(!running_ && !actionPending_ && !updateBusy && data.value("trainer_ready").toBool() && data.value("model_ready").toBool());
    for (auto* widget : jobButtons_) widget->setEnabled(!running_ && !actionPending_ && !updateBusy);
    findChild<QPushButton*>("applyModel")->setEnabled(!running_ && !actionPending_ && !history_->selectedId().isEmpty());
    updateRecordActions();
    if (oldJob.value("state") != job.value("state") && !running_ && job.value("state") != "idle") notice(job_->text(), job.value("state") == "failed");
    else if (message_->text() == QStringLiteral("正在連接…")) notice({});
}

QString Manager::selectedRun() const {
    return history_->selectedId();
}
void Manager::renderHistory(const QJsonObject& data) {
    const auto oldBase = base_->currentData().toString();
    const bool first = base_->count() == 1 && base_->currentData(Qt::UserRole + 1).isNull();
    const QSignalBlocker blocker(base_);
    base_->clear(); base_->addItem(QStringLiteral("Base model · 從頭開始"), "");
    historyRuns_.clear();
    const auto runs = data.value("runs").toArray();
    for (const auto value : runs) {
        const auto run = value.toObject(); const auto id = text(run.value("id"));
        const auto title = QStringLiteral("#%1 · %2").arg(id, timeLabel(run.value("completed_at").toString()));
        base_->addItem(title, id); base_->setItemData(base_->count() - 1, run, Qt::UserRole + 1);
        historyRuns_.insert(id, run);
    }
    history_->setRuns(runs, state_.value("active_model_path").toString());
    int index = base_->findData(oldBase);
    if (first && runs.size()) index = 1;
    base_->setCurrentIndex(qMax(0, index));
    if (first && index > 0) {
        const auto run = base_->currentData(Qt::UserRole + 1).toJsonObject();
        for (const auto& key : {"rank", "alpha", "dropout", "target_modules"}) {
            QString field = key; field.replace('_', '-');
            if (run.contains(key)) options_[field]->setText(text(run.value(key)));
        }
    }
    base_->setItemData(0, false, Qt::UserRole + 1);
    ancestor_->setVisible(!data.value("common_ancestor").isNull());
    ancestor_->setProperty("runId", text(data.value("common_ancestor")));
}

QJsonObject Manager::trainingRequest() const {
    QJsonObject options;
    for (auto it = options_.cbegin(); it != options_.cend(); ++it) options.insert(it.key(), it.value()->text());
    return {{"options", options}, {"strength", strength_->currentData().toString()}, {"base_run_id", base_->currentData().toString()},
        {"only_manually_selected", manualTrain_->isChecked()}, {"stabilize_intruders", stabilize_->isChecked()}, {"password", trainPassword_->text()}};
}
void Manager::train() {
#ifdef Q_OS_LINUX
    if (updates_ && updates_->busy()) return;
#endif
    if (actionPending_ || running_) return;
    const auto body = trainingRequest();
    act("/api/count-trainable", {{"password", trainPassword_->text()}, {"max_seq_length", options_["max-seq-length"]->text()},
        {"only_manually_selected", manualTrain_->isChecked()}}, [this, body](const QJsonValue& value) {
        const auto report = value.toObject();
        skippedIds_.clear(); for (const auto id : report.value("skipped_ids").toArray()) skippedIds_.insert(id.toString());
        const int count = report.value("records").toInt();
        if (!count) { notice(QStringLiteral("沒有可訓練的資料。請累積手動選字紀錄，或調整資料範圍。"), true); return; }
        if (QMessageBox::question(this, QStringLiteral("開始個人化訓練"),
            QStringLiteral("本次使用 %1 筆資料、%2 個樣本；略過 %3 筆。開始訓練？").arg(count).arg(report.value("samples").toInt()).arg(report.value("skipped").toInt()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) == QMessageBox::Yes) {
            trainPassword_->clear(); act("/api/train", body);
        }
    });
}

void Manager::closeEvent(QCloseEvent* event) {
#ifdef Q_OS_LINUX
    if (updates_ && updates_->busy()) {
        notice(QStringLiteral("更新正在處理，請等待完成；若尚在等待組字或訓練，可到軟體更新頁取消等待。"));
        event->ignore(); return;
    }
#endif
    if ((settings_->isDirty() || (phrases_ && phrases_->isDirty())) && QMessageBox::question(this, QStringLiteral("尚未儲存"),
        QStringLiteral("設定或替代詞彙有未儲存的修改。捨棄修改並關閉？"), QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel) != QMessageBox::Yes) { event->ignore(); return; }
    if (running_ && QMessageBox::question(this, QStringLiteral("工作仍在執行"), QStringLiteral("關閉會取消目前工作。取消工作並關閉？"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) { event->ignore(); return; }
    if (guide_) guide_->close();
    timer_->stop(); event->accept();
}

} // namespace llavon::lora
