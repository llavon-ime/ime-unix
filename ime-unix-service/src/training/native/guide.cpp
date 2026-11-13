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
}

// The key predates the setup; keeping it means people who dismissed the old
// feature tour are not asked again.
bool UiPreferences::seen() {
    QSettings preferences(preferencesPath(), QSettings::IniFormat);
    return preferences.value("quickStartDismissed", false).toBool();
}
bool UiPreferences::markSeen() {
    QSettings preferences(preferencesPath(), QSettings::IniFormat);
    preferences.setValue("quickStartDismissed", true);
    preferences.sync();
    return preferences.status() == QSettings::NoError;
}
QString UiPreferences::look() {
    QSettings preferences(preferencesPath(), QSettings::IniFormat);
    return preferences.value("look").toString() == QStringLiteral("workbook") ? QStringLiteral("workbook") : QStringLiteral("keycap");
}
bool UiPreferences::setLook(const QString& look) {
    QSettings preferences(preferencesPath(), QSettings::IniFormat);
    preferences.setValue("look", look);
    preferences.sync();
    return preferences.status() == QSettings::NoError;
}

GuidePage::GuidePage(QWidget* parent) : QDialog(parent) {
    setObjectName("usageGuide");
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(QStringLiteral("拉風使用指南"));
    setWindowModality(Qt::NonModal);
    resize(780, 720); setMinimumSize(660, 480);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 16); layout->setSpacing(16);
    topics_ = new QTabWidget;
    topics_->setObjectName("guideTopics");
    layout->addWidget(topics_);

    // Topics are built when their tab is opened, not on launch.
    for (const auto& title : {QStringLiteral("快捷鍵"), QStringLiteral("中英混輸"), QStringLiteral("常用詞彙"),
             QStringLiteral("個人化"), QStringLiteral("疑難排解")}) {
        auto placeholder = topic(title, {}); topics_->addTab(placeholder.area, title);
    }
    connect(topics_, &QTabWidget::currentChanged, this, &GuidePage::buildTopic);
    buildTopic(0);
    auto* footer = new QHBoxLayout;
    auto* setup = new QPushButton(QStringLiteral("重新進行開始設定…")); setup->setObjectName("guideSetup");
    setup->setProperty("quiet", true); setup->setAutoDefault(false);
    setup->setToolTip(QStringLiteral("再選一次鍵盤配置、輸入習慣與中英混輸。"));
    footer->addWidget(setup); footer->addStretch();
    layout->addLayout(footer);
    connect(setup, &QPushButton::clicked, this, &GuidePage::setupRequested);
}

void GuidePage::buildTopic(int index) {
    if (index < 0 || builtTopics_.contains(index)) return;
    auto* area = qobject_cast<QScrollArea*>(topics_->widget(index));
    if (!area) return;
    auto* layout = qobject_cast<QVBoxLayout*>(area->widget()->layout());
    if (!layout) return;
    builtTopics_.insert(index);
    if (index == 0) {
        area->setObjectName("guideShortcutTopic");
        layout->addWidget(paragraph(QStringLiteral("選字與修正"), "section"));
        shortcut(layout, {"Enter"}, QStringLiteral("提交組字"), QStringLiteral("選字窗開啟時，先確認候選"));
        shortcut(layout, {QStringLiteral("← / →")}, QStringLiteral("移動組字游標"));
        shortcut(layout, {QStringLiteral("↓")}, QStringLiteral("開啟候選字"));
        shortcut(layout, {"Tab"}, QStringLiteral("開啟候選；依設定切換候選或展開選字窗"));
        shortcut(layout, {"PgUp / PgDn"}, QStringLiteral("候選翻頁"));
        shortcut(layout, {"Backspace"}, QStringLiteral("刪除游標前內容"));
        shortcut(layout, {"Esc"}, QStringLiteral("關閉選字窗"), QStringLiteral("其餘清除行為依設定"));
        layout->addWidget(paragraph(QStringLiteral("符號與常用詞"), "section"));
        shortcut(layout, {"`"}, QStringLiteral("開啟分類符號"));
        shortcut(layout, {"Shift", QStringLiteral("← / →")}, QStringLiteral("標記詞彙範圍"));
        shortcut(layout, {"Enter"}, QStringLiteral("加入標記的替代詞彙"), QStringLiteral("限 2–8 個有注音的字，不會提交整句"));
    } else if (index == 1) {
        area->setObjectName("guideMixedTopic");
        layout->addWidget(paragraph(QStringLiteral("預設關閉；開啟後一起判斷英文與注音。"), "muted"));
        illustration(layout, GuideIllustration::Kind::MixedInput, 3, QStringLiteral("輸入法設定 › 常用"), "guideDetailedMixedIllustration");
        conversion(layout, "hello283", QStringLiteral("hello打"));
        layout->addWidget(paragraph(QStringLiteral("283 是「ㄉㄚˇ」的標準鍵盤按鍵；其他配置請使用對應按鍵。"), "muted"));
        shortcut(layout, {QStringLiteral("↓")}, QStringLiteral("查看其他解讀與原文"));
        shortcut(layout, {"Enter"}, QStringLiteral("提交目前預覽"), QStringLiteral("混輸的空白保留在組字中，不會提交整句"));
        connect(action(layout, QStringLiteral("查看智慧型中英文…"), "guideSmartEnglish"), &QPushButton::clicked,
            this, [this] { emit settingRequested("smart_english"); });
    } else if (index == 2) {
        area->setObjectName("guidePhraseTopic");
        layout->addWidget(paragraph(QStringLiteral("人名與專有名詞，用你指定的寫法。"), "muted"));
        illustration(layout, GuideIllustration::Kind::Phrases, 4, QStringLiteral("替代詞彙 › 詞彙與讀音"), "guideDetailedPhraseIllustration");
        layout->addWidget(paragraph(QStringLiteral("每筆 2–8 個字，同一組讀音只能有一筆，多音字要確認讀音。"), "muted"));
        layout->addWidget(paragraph(QStringLiteral("也能直接從組字加入"), "section"));
        shortcut(layout, {"Shift", QStringLiteral("← / →")}, QStringLiteral("標記 2–8 個有注音的字"));
        shortcut(layout, {"Enter"}, QStringLiteral("加入替代詞彙，不會提交整句"));
        layout->addWidget(paragraph(QStringLiteral("相同讀音優先替代，仍可改選；在設定 App 編輯後要儲存並套用。"), "muted"));
        connect(action(layout, QStringLiteral("管理替代詞彙…"), "guidePhraseList"), &QPushButton::clicked,
            this, [this] { emit pageRequested("phrases"); });
    } else if (index == 3) {
        area->setObjectName("guidePrivacyTopic");
        layout->addWidget(paragraph(QStringLiteral("日常打字不必訓練；需要時再開啟本機個人化。"), "muted"));
        illustration(layout, GuideIllustration::Kind::Personalization, 0, QStringLiteral("訓練資料 › 設定資料密碼"), "guideDetailedPrivacyIllustration");
        personalizationFlow(layout);
        layout->addWidget(paragraph(QStringLiteral("收集預設關閉。自行啟用後，符合條件的已提交輸入才會加密記錄；可暫停、排除或刪除。"), "muted"));
        layout->addWidget(paragraph(QStringLiteral("選字推論在本機完成。訓練需另備基礎 checkpoint 與訓練器，可能下載資源；看指南不會收集或開始訓練。"), "muted"));
        connect(action(layout, QStringLiteral("查看訓練資料…"), "guideRecords"), &QPushButton::clicked,
            this, [this] { emit pageRequested("records"); });
    } else if (index == 4) {
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

} // namespace llavon::lora
