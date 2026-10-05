#include "guide.hpp"
#include "settings.hpp"
#include "appearance.hpp"
#include "guide_illustration.hpp"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QEvent>
#include <QPainter>
#include <QFrame>

namespace llavon::lora {
namespace {
QString preferencesPath() {
    return QFileInfo(SettingsStore::configPath()).absolutePath() + "/onboarding.ini";
}
QLabel* paragraph(const QString& text, const char* role = nullptr) {
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    if (role) label->setProperty("role", role);
    return label;
}
struct Topic {
    QScrollArea* area;
    QVBoxLayout* layout;
};
Topic topic(const QString& title, const QString& introduction) {
    auto* content = new QWidget;
    content->setObjectName("settingsForm");
    content->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 8, 12, 16);
    layout->setSpacing(12);
    layout->addWidget(paragraph(title, "guideTopicTitle"));
    if (!introduction.isEmpty()) layout->addWidget(paragraph(introduction, "muted"));
    auto* area = new QScrollArea;
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setWidget(content);
    return {area, layout};
}
QLabel* keycap(const QString& key) {
    auto* label = paragraph(key, "guideKey"); label->setWordWrap(false);
    label->setAlignment(Qt::AlignCenter); label->setMinimumWidth(30);
    return label;
}
void shortcut(QVBoxLayout* layout, const QStringList& keys, const QString& meaning, const QString& detail = {}) {
    auto* row = new QFrame; row->setObjectName("guideShortcutRow");
    auto* horizontal = new QHBoxLayout(row); horizontal->setContentsMargins(0, 9, 0, 9); horizontal->setSpacing(16);
    auto* keyGroup = new QWidget; keyGroup->setFixedWidth(208);
    auto* caps = new QHBoxLayout(keyGroup); caps->setContentsMargins(0, 0, 0, 0); caps->setSpacing(6);
    for (int index = 0; index < keys.size(); ++index) {
        if (index) caps->addWidget(paragraph(QStringLiteral("+"), "muted"));
        caps->addWidget(keycap(keys[index]));
    }
    caps->addStretch(); horizontal->addWidget(keyGroup);
    auto* explanation = new QVBoxLayout; explanation->setSpacing(3);
    explanation->addWidget(paragraph(meaning));
    if (!detail.isEmpty()) explanation->addWidget(paragraph(detail, "muted"));
    horizontal->addLayout(explanation, 1); layout->addWidget(row);
}
void route(QVBoxLayout* layout, const QStringList& stops) {
    auto* line = new QHBoxLayout; line->setSpacing(8);
    for (int index = 0; index < stops.size(); ++index) {
        if (index) line->addWidget(paragraph(QStringLiteral("→"), "guideAnnotation"));
        line->addWidget(paragraph(stops[index]));
    }
    line->addStretch(); layout->addLayout(line);
}
void conversion(QVBoxLayout* layout, const QString& raw, const QString& result) {
    auto* line = new QHBoxLayout; line->setSpacing(24);
    auto* input = new QVBoxLayout; input->setSpacing(6);
    input->addWidget(paragraph(QStringLiteral("標準注音鍵盤按鍵"), "muted"));
    input->addWidget(keycap(raw)); line->addLayout(input, 1);
    line->addWidget(paragraph(QStringLiteral("→"), "guideAnnotation"));
    auto* output = new QVBoxLayout; output->setSpacing(6);
    output->addWidget(paragraph(QStringLiteral("預覽範例"), "muted"));
    auto* preview = paragraph(result, "guideExample"); preview->setObjectName("guideDetailedMixedResult");
    output->addWidget(preview); line->addLayout(output, 1); layout->addLayout(line);
}
void personalizationFlow(QVBoxLayout* layout) {
    auto* line = new QHBoxLayout; line->setSpacing(12);
    const QStringList names{QStringLiteral("保留紀錄"), QStringLiteral("本機訓練"), QStringLiteral("套用模型")};
    for (int index = 0; index < names.size(); ++index) {
        if (index) line->addWidget(paragraph(QStringLiteral("→"), "guideAnnotation"));
        auto* stage = new QWidget; stage->setObjectName("guideFlowStage");
        auto* vertical = new QVBoxLayout(stage); vertical->setContentsMargins(0, 10, 0, 10); vertical->setSpacing(8);
        auto* icon = new QLabel; icon->setPixmap(navigationIcon(index).pixmap(26, 26));
        vertical->addWidget(icon); vertical->addWidget(paragraph(names[index]));
        line->addWidget(stage, 1);
    }
    layout->addLayout(line);
}
QPushButton* troubleshooting(QVBoxLayout* layout, int iconIndex, const QString& symptom,
                            const QString& hint, const QString& actionText, const char* actionName) {
    auto* row = new QFrame; row->setObjectName("guideHelpRow");
    auto* horizontal = new QHBoxLayout(row); horizontal->setContentsMargins(0, 18, 0, 18); horizontal->setSpacing(14);
    auto* icon = new QLabel; icon->setPixmap(navigationIcon(iconIndex).pixmap(26, 26));
    horizontal->addWidget(icon, 0, Qt::AlignTop);
    auto* text = new QVBoxLayout; text->setSpacing(6);
    text->addWidget(paragraph(symptom, "section")); text->addWidget(paragraph(hint, "muted"));
    horizontal->addLayout(text, 1);
    auto* button = new QPushButton(actionText); button->setObjectName(actionName); button->setAutoDefault(false);
    horizontal->addWidget(button); layout->addWidget(row); return button;
}
QPushButton* action(QVBoxLayout* layout, const QString& text, const char* name) {
    auto* button = new QPushButton(text);
    button->setObjectName(name);
    button->setAutoDefault(false);
    layout->addWidget(button, 0, Qt::AlignLeft);
    return button;
}
void illustration(QVBoxLayout* layout, GuideIllustration::Kind kind, int page, const QString& location, const char* name = nullptr) {
    auto* heading = new QHBoxLayout;
    auto* icon = new QLabel; icon->setPixmap(navigationIcon(page).pixmap(18, 18));
    heading->addWidget(icon);
    heading->addWidget(paragraph(location, "muted"), 1);
    auto* sample = paragraph(QStringLiteral("示意"), "muted");
    sample->setToolTip(QStringLiteral("不會修改設定、加入詞彙或啟用收集。"));
    heading->addWidget(sample);
    layout->addLayout(heading);
    auto* preview = new GuideIllustration(kind);
    if (name) preview->setObjectName(name);
    layout->addWidget(preview);
}
QIcon stepIcon(bool backward, bool finished = false) {
    QIcon icon;
    for (const auto ratio : {1, 2}) {
        QPixmap image(18 * ratio, 18 * ratio); image.setDevicePixelRatio(ratio); image.fill(Qt::transparent);
        QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(backward ? appearance().text : QColor(Qt::white), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (finished) painter.drawPolyline(QPolygonF{QPointF(3, 9), QPointF(7, 13), QPointF(15, 5)});
        else {
            painter.drawLine(QPointF(3, 9), QPointF(15, 9));
            painter.drawPolyline(backward ? QPolygonF{QPointF(8, 4), QPointF(3, 9), QPointF(8, 14)} :
                QPolygonF{QPointF(10, 4), QPointF(15, 9), QPointF(10, 14)});
        }
        painter.end(); icon.addPixmap(image);
    }
    return icon;
}
}

bool QuickStartPreferences::dismissed() {
    QSettings preferences(preferencesPath(), QSettings::IniFormat);
    return preferences.value("quickStartDismissed", false).toBool();
}
bool QuickStartPreferences::dismiss() {
    QSettings preferences(preferencesPath(), QSettings::IniFormat);
    preferences.setValue("quickStartDismissed", true);
    preferences.sync();
    return preferences.status() == QSettings::NoError;
}

GuidePage::GuidePage(QWidget* parent) : QDialog(parent) {
    setObjectName("usageGuide");
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(QStringLiteral("拉風 · 使用指南"));
    setWindowModality(Qt::NonModal);
    resize(780, 720); setMinimumSize(660, 480);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 16); layout->setSpacing(16);
    topics_ = new QTabWidget;
    topics_->setObjectName("guideTopics");
    layout->addWidget(topics_);

    auto* quick = new QWidget;
    auto* quickLayout = new QVBoxLayout(quick);
    quickLayout->setContentsMargins(0, 0, 0, 0);
    quickLayout->setSpacing(16);
    progress_ = paragraph({}, "muted");
    progress_->setObjectName("guideProgress");
    quickLayout->addWidget(progress_);
    steps_ = new QStackedWidget;
    steps_->setObjectName("guideSteps");
    quickLayout->addWidget(steps_, 1);
    auto mixedFeature = topic(QStringLiteral("英文和中文，一起打"),
        QStringLiteral("一起判斷英文與注音，不必一直切換輸入方式。"));
    QString mixedGroup;
    for (const auto field : SettingsStore::schema().value("fields").toArray())
        if (field.toObject().value("key").toString() == "smart_english") mixedGroup = field.toObject().value("group").toString();
    illustration(mixedFeature.layout, GuideIllustration::Kind::MixedInput, 3, QStringLiteral("輸入法設定 › %1").arg(mixedGroup));
    auto* example = new QHBoxLayout;
    auto* raw = paragraph(QStringLiteral("hello283"), "guideExample"); raw->setObjectName("guideMixedRaw");
    auto* arrow = paragraph(QStringLiteral("→"), "guideExample");
    auto* result = paragraph(QStringLiteral("hello打"), "guideExample"); result->setObjectName("guideMixedResult");
    example->addWidget(raw); example->addWidget(arrow); example->addWidget(result); example->addStretch();
    mixedFeature.layout->addWidget(paragraph(QStringLiteral("標準注音鍵盤範例；結果仍可改選。"), "muted"));
    auto* mixedAction = new QPushButton(QStringLiteral("查看智慧型中英文…")); mixedAction->setObjectName("guideMixedFeature"); mixedAction->setAutoDefault(false);
    example->addWidget(mixedAction); mixedFeature.layout->addLayout(example);
    connect(mixedAction, &QPushButton::clicked,
        this, [this] { emit settingRequested("smart_english"); });
    mixedFeature.layout->addStretch(); steps_->addWidget(mixedFeature.area);

    auto phraseFeature = topic(QStringLiteral("讓常用名字，一次選對"),
        QStringLiteral("相同注音優先使用指定寫法，不必訓練模型。"));
    illustration(phraseFeature.layout, GuideIllustration::Kind::Phrases, 4, QStringLiteral("替代詞彙 › 詞彙與讀音"));
    phraseFeature.layout->addWidget(paragraph(QStringLiteral("讀音相符才替代，仍可手動改選。"), "muted"));
    connect(action(phraseFeature.layout, QStringLiteral("管理替代詞彙…"), "guidePhrases"), &QPushButton::clicked,
        this, [this] { emit pageRequested("phrases"); });
    phraseFeature.layout->addStretch(); steps_->addWidget(phraseFeature.area);

    auto personalFeature = topic(QStringLiteral("讓模型更懂你的用字習慣"),
        QStringLiteral("用保留的輸入紀錄，訓練自己的 LoRA。"));
    illustration(personalFeature.layout, GuideIllustration::Kind::Personalization, 0, QStringLiteral("訓練資料 › 設定資料密碼"));
    personalFeature.layout->addWidget(paragraph(QStringLiteral("收集預設關閉；自行啟用後才加密記錄，可暫停或刪除。訓練需另備基礎模型與訓練器。"), "muted"));
    connect(action(personalFeature.layout, QStringLiteral("查看本機個人化…"), "guidePersonalization"), &QPushButton::clicked,
        this, [this] { emit pageRequested("records"); });
    personalFeature.layout->addStretch(); steps_->addWidget(personalFeature.area);
    auto* navigation = new QHBoxLayout;
    previous_ = new QPushButton(QStringLiteral("上一步")); previous_->setObjectName("guidePrevious");
    previous_->setAutoDefault(false);
    next_ = new QPushButton; next_->setObjectName("guideNext"); next_->setProperty("primary", true); next_->setFixedWidth(160);
    auto* restart = new QPushButton(QStringLiteral("重新開始")); restart->setObjectName("guideRestart"); restart->setProperty("quiet", true);
    restart->setAutoDefault(false); next_->setAutoDefault(false);
    navigation->addWidget(previous_); navigation->addWidget(restart); navigation->addStretch(); navigation->addWidget(next_);
    quickLayout->addLayout(navigation);
    connect(previous_, &QPushButton::clicked, this, [this] { steps_->setCurrentIndex(steps_->currentIndex() - 1); updateStep(); });
    connect(next_, &QPushButton::clicked, this, [this] {
        if (steps_->currentIndex() == steps_->count() - 1) emit finished();
        else { steps_->setCurrentIndex(steps_->currentIndex() + 1); updateStep(); }
    });
    connect(restart, &QPushButton::clicked, this, &GuidePage::startQuickGuide);
    topics_->addTab(quick, QStringLiteral("功能導覽"));

    // Build detailed examples only when their tab is opened, not on launch or
    // on the first visit to the three-step tour.
    for (const auto& title : {QStringLiteral("快捷鍵"), QStringLiteral("中英混輸"), QStringLiteral("常用詞彙"),
             QStringLiteral("個人化"), QStringLiteral("疑難排解")}) {
        auto placeholder = topic(title, {}); topics_->addTab(placeholder.area, title);
    }
    connect(topics_, &QTabWidget::currentChanged, this, &GuidePage::buildTopic);
    auto* skip = new QPushButton(QStringLiteral("略過全部")); skip->setObjectName("guideSkipAll"); skip->setProperty("quiet", true);
    skip->setAutoDefault(false);
    layout->addWidget(skip, 0, Qt::AlignLeft);
    connect(skip, &QPushButton::clicked, this, &GuidePage::skipped);
    updateStep();
}

void GuidePage::buildTopic(int index) {
    if (index <= 0 || builtTopics_.contains(index)) return;
    auto* area = qobject_cast<QScrollArea*>(topics_->widget(index));
    if (!area) return;
    auto* layout = qobject_cast<QVBoxLayout*>(area->widget()->layout());
    if (!layout) return;
    builtTopics_.insert(index);
    if (index == 1) {
        area->setObjectName("guideShortcutTopic");
        layout->addWidget(paragraph(QStringLiteral("選字與修正"), "section"));
        shortcut(layout, {"Enter"}, QStringLiteral("提交組字"), QStringLiteral("選字窗開啟時，先確認候選"));
        shortcut(layout, {QStringLiteral("← / →")}, QStringLiteral("移動組字游標"));
        shortcut(layout, {QStringLiteral("↓")}, QStringLiteral("開啟候選字"));
        shortcut(layout, {"Tab"}, QStringLiteral("開啟候選；切換選字窗展開"));
        shortcut(layout, {"PgUp / PgDn"}, QStringLiteral("候選翻頁"));
        shortcut(layout, {"Backspace"}, QStringLiteral("刪除游標前內容"));
        shortcut(layout, {"Esc"}, QStringLiteral("關閉選字窗"), QStringLiteral("其餘清除行為依設定"));
        layout->addWidget(paragraph(QStringLiteral("符號與常用詞"), "section"));
        shortcut(layout, {"`"}, QStringLiteral("開啟分類符號"));
        shortcut(layout, {"Shift", QStringLiteral("← / →")}, QStringLiteral("標記詞彙範圍"));
        shortcut(layout, {"Enter"}, QStringLiteral("加入標記的替代詞彙"), QStringLiteral("限 2–8 個有注音的字，不會提交整句"));
    } else if (index == 2) {
        area->setObjectName("guideMixedTopic");
        layout->addWidget(paragraph(QStringLiteral("預設關閉；開啟後一起判斷英文與注音。"), "muted"));
        illustration(layout, GuideIllustration::Kind::MixedInput, 3, QStringLiteral("輸入法設定 › 輸入行為"), "guideDetailedMixedIllustration");
        conversion(layout, "hello283", QStringLiteral("hello打"));
        layout->addWidget(paragraph(QStringLiteral("283 是「ㄉㄚˇ」的標準鍵盤按鍵；其他配置請使用對應按鍵。"), "muted"));
        shortcut(layout, {QStringLiteral("↓")}, QStringLiteral("查看其他解讀與原文"));
        shortcut(layout, {"Enter"}, QStringLiteral("提交目前預覽"), QStringLiteral("混輸的空白保留在組字中，不會提交整句"));
        connect(action(layout, QStringLiteral("查看智慧型中英文…"), "guideSmartEnglish"), &QPushButton::clicked,
            this, [this] { emit settingRequested("smart_english"); });
    } else if (index == 3) {
        area->setObjectName("guidePhraseTopic");
        layout->addWidget(paragraph(QStringLiteral("人名與專有名詞，用你指定的寫法。"), "muted"));
        illustration(layout, GuideIllustration::Kind::Phrases, 4, QStringLiteral("替代詞彙 › 詞彙與讀音"), "guideDetailedPhraseIllustration");
        layout->addWidget(paragraph(QStringLiteral("每筆 2–8 個字 · 同一組讀音限一筆 · 多音字需確認"), "muted"));
        layout->addWidget(paragraph(QStringLiteral("也能直接從組字加入"), "section"));
        shortcut(layout, {"Shift", QStringLiteral("← / →")}, QStringLiteral("標記 2–8 個有注音的字"));
        shortcut(layout, {"Enter"}, QStringLiteral("加入替代詞彙，不會提交整句"));
        layout->addWidget(paragraph(QStringLiteral("相同讀音優先替代，仍可改選；在設定 App 編輯後要儲存並套用。"), "muted"));
        connect(action(layout, QStringLiteral("管理替代詞彙…"), "guidePhraseList"), &QPushButton::clicked,
            this, [this] { emit pageRequested("phrases"); });
    } else if (index == 4) {
        area->setObjectName("guidePrivacyTopic");
        layout->addWidget(paragraph(QStringLiteral("日常打字不必訓練；需要時再開啟本機個人化。"), "muted"));
        illustration(layout, GuideIllustration::Kind::Personalization, 0, QStringLiteral("訓練資料 › 設定資料密碼"), "guideDetailedPrivacyIllustration");
        personalizationFlow(layout);
        layout->addWidget(paragraph(QStringLiteral("收集預設關閉。自行啟用後，符合條件的已提交輸入才會加密記錄；可暫停、排除或刪除。"), "muted"));
        layout->addWidget(paragraph(QStringLiteral("選字推論在本機完成。訓練需另備基礎 checkpoint 與訓練器，可能下載資源；看指南不會收集或開始訓練。"), "muted"));
        connect(action(layout, QStringLiteral("查看訓練資料…"), "guideRecords"), &QPushButton::clicked,
            this, [this] { emit pageRequested("records"); });
    } else if (index == 5) {
        area->setObjectName("guideHelpTopic");
        layout->addWidget(paragraph(QStringLiteral("找不到拉風輸入方式"), "section"));
#ifdef Q_OS_MACOS
        route(layout, {QStringLiteral("系統設定"), QStringLiteral("鍵盤"), QStringLiteral("輸入方式"), QStringLiteral("加入拉風")});
        layout->addWidget(paragraph(QStringLiteral("macOS 26 首次安裝可能需手動加入，或登出再登入。"), "muted"));
#else
        route(layout, {"Fcitx5", QStringLiteral("設定"), QStringLiteral("輸入方式"), "Llavon IME"});
        layout->addWidget(paragraph(QStringLiteral("仍未出現時，先提交正在輸入的內容，再重新啟動 Fcitx5。"), "muted"));
#endif
        connect(troubleshooting(layout, 3, QStringLiteral("按鍵與預期不同"), QStringLiteral("確認鍵盤配置、Caps Lock 與智慧混輸。"),
            QStringLiteral("查看鍵盤設定…"), "guideKeyboard"), &QPushButton::clicked,
            this, [this] { emit settingRequested("keyboard_layout"); });
        connect(troubleshooting(layout, 6, QStringLiteral("預測沒有更新"), QStringLiteral("先確認預測服務與前文來源的狀態。"),
            QStringLiteral("版本與狀態…"), "guideStatus"), &QPushButton::clicked,
            this, [this] { emit pageRequested("about"); });
#ifdef Q_OS_MACOS
        route(layout, {QStringLiteral("輸入法選單"), QStringLiteral("重新啟動")});
        layout->addWidget(paragraph(QStringLiteral("服務沒有回應時可重新啟動，不會刪除詞彙。"), "muted"));
#else
        layout->addWidget(paragraph(QStringLiteral("首次載入模型需要等待；選字結果仍可手動改選。"), "muted"));
#endif
    }
    layout->addStretch();
}

void GuidePage::startQuickGuide() {
    topics_->setCurrentIndex(0);
    steps_->setCurrentIndex(0);
    updateStep();
}
void GuidePage::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (previous_ && next_ && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) updateStep();
}
void GuidePage::updateStep() {
    progress_->setText(QStringLiteral("功能 %1 / %2").arg(steps_->currentIndex() + 1).arg(steps_->count()));
    previous_->setEnabled(steps_->currentIndex() > 0);
    next_->setText(steps_->currentIndex() == steps_->count() - 1 ? QStringLiteral("完成導覽") : QStringLiteral("下一個功能"));
    previous_->setIcon(stepIcon(true));
    next_->setIcon(stepIcon(false, steps_->currentIndex() == steps_->count() - 1));
}
} // namespace llavon::lora
