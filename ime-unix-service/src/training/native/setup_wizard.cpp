#include "setup_wizard.hpp"
#include "appearance.hpp"
#include "controls.hpp"
#include "guide.hpp"
#include "settings.hpp"

#include "bopomofo/keymap.hpp"

#include <QButtonGroup>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>

namespace llavon::lora {
namespace {
const QStringList keyRows{QStringLiteral("1234567890-"), QStringLiteral("qwertyuiop"), QStringLiteral("asdfghjkl;"), QStringLiteral("zxcvbnm,./")};
constexpr double rowOffsets[] = {0.0, 0.5, 0.75, 1.25};

QLabel* paragraph(const QString& text, const char* role = nullptr) {
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    if (role) label->setProperty("role", role);
    return label;
}
QPushButton* actionButton(const QString& text, const char* name) {
    auto* button = new QPushButton(text);
    button->setObjectName(name); button->setAutoDefault(false); button->setMinimumHeight(34);
    return button;
}
struct Step {
    QScrollArea* area;
    QVBoxLayout* layout;
};
Step step(const QString& title, const QString& introduction) {
    auto* content = new QWidget;
    content->setObjectName("settingsForm");
    content->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 4, 12, 12);
    layout->setSpacing(14);
    layout->addWidget(paragraph(title, "guideTopicTitle"));
    layout->addWidget(paragraph(introduction, "muted"));
    auto* area = new QScrollArea;
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setWidget(content);
    return {area, layout};
}
QJsonObject schemaField(const QString& key) {
    for (const auto item : SettingsStore::schema().value("fields").toArray())
        if (item.toObject().value("key").toString() == key) return item.toObject();
    return {};
}
}

KeyboardPreview::KeyboardPreview(QWidget* parent) : QWidget(parent) {
    setObjectName("setupKeyboardPreview");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAccessibleName(QStringLiteral("鍵盤配置預覽"));
    setLayoutName(layout_);
}

QString KeyboardPreview::symbol(QChar key) const {
    using namespace llavon::ime;
    const auto layout = bopomofo_keyboard_layout(layout_.toStdString());
    const char32_t code = key.unicode();
    if (const auto direct = lookup_bopomofo_key(code, layout, false)) return QString::fromUcs4(&*direct, 1);
    if (!is_compact_bopomofo_layout(layout)) return {};
    // A compact layout has no single map; show the symbol a key starts with.
    Syllable syllable;
    if (apply_bopomofo_key(syllable, layout, code, false).status == BopomofoKeyStatus::Rejected) return {};
    return QString::fromStdU16String(syllable.text()).left(1);
}

void KeyboardPreview::setLayoutName(const QString& name) {
    layout_ = name;
    QStringList keys;
    for (const auto& row : keyRows)
        for (const auto key : row)
            if (const auto value = symbol(key); !value.isEmpty()) keys.append(QString(key) + ' ' + value);
    setAccessibleDescription(keys.join(QStringLiteral("、")));
    update();
}

void KeyboardPreview::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto colors = appearance();
    const double units = 11.25, gap = 6;
    const double key = std::min(52.0, (width() - 2.0 - gap * 11) / units);
    const double totalWidth = units * key + gap * 11, totalHeight = 4 * key + 3 * gap;
    const QPointF origin((width() - totalWidth) / 2, (height() - totalHeight) / 2);
    auto symbolFont = font(); symbolFont.setPixelSize(std::max(12, static_cast<int>(key * 0.42))); symbolFont.setWeight(QFont::Medium);
    auto keyFont = font(); keyFont.setPixelSize(std::max(9, static_cast<int>(key * 0.22)));
    for (int row = 0; row < keyRows.size(); ++row) {
        for (int column = 0; column < keyRows[row].size(); ++column) {
            const auto character = keyRows[row][column];
            const QRectF bounds(origin.x() + (rowOffsets[row] + column) * key + column * gap,
                                origin.y() + row * (key + gap), key, key);
            const auto value = symbol(character);
            painter.setPen(QPen(colors.border, 1));
            painter.setBrush(value.isEmpty() ? colors.background : colors.surface);
            painter.drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            painter.setFont(keyFont); painter.setPen(colors.secondary);
            painter.drawText(bounds.adjusted(6, 3, -4, -2), Qt::AlignLeft | Qt::AlignTop, QString(character).toUpper());
            if (value.isEmpty()) continue;
            painter.setFont(symbolFont); painter.setPen(colors.text);
            painter.drawText(bounds.adjusted(0, key * 0.18, 0, 0), Qt::AlignCenter, value);
        }
    }
}

bool SetupWizard::shouldOffer() {
    return !SetupPreferences::seen() && !QFileInfo::exists(SettingsStore::configPath());
}

SetupWizard::SetupWizard(SettingsPage* settings, QWidget* parent) : QDialog(parent), settings_(settings) {
    setObjectName("setupWizard");
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(QStringLiteral("開始使用拉風"));
    setWindowModality(Qt::NonModal);
    resize(720, 640); setMinimumSize(620, 520);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 20, 28, 18); layout->setSpacing(14);
    progress_ = paragraph({}, "muted"); progress_->setObjectName("setupProgress");
    layout->addWidget(progress_);
    steps_ = new QStackedWidget; steps_->setObjectName("setupSteps");
    layout->addWidget(steps_, 1);

    // 1. Keyboard layout.
    auto keyboard = step(QStringLiteral("你用哪一種注音鍵盤？"),
        QStringLiteral("選好就會套用，之後可以在「輸入法設定」更改。"));
    layouts_ = new QButtonGroup(this);
    auto* choices = new QGridLayout; choices->setHorizontalSpacing(18); choices->setVerticalSpacing(8);
    const auto layoutChoices = schemaField("keyboard_layout").value("choices").toArray();
    for (int index = 0; index < layoutChoices.size(); ++index) {
        const auto choice = layoutChoices[index].toObject();
        auto* option = new QRadioButton(choice.value("label").toString());
        option->setObjectName("setupLayout_" + choice.value("value").toString());
        option->setProperty("value", choice.value("value").toString());
        layouts_->addButton(option); choices->addWidget(option, index / 4, index % 4);
    }
    keyboard.layout->addLayout(choices);
    preview_ = new KeyboardPreview; keyboard.layout->addWidget(preview_);
    layoutNote_ = paragraph({}, "muted"); layoutNote_->setObjectName("setupLayoutNote");
    keyboard.layout->addWidget(layoutNote_);
    keyboard.layout->addStretch();
    steps_->addWidget(keyboard.area);
    connect(layouts_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* option) {
        const auto value = option->property("value").toString();
        settings_->setValue("keyboard_layout", value);
        describeLayout(value);
    });

    // 2. Input habit, from the same presets as the settings page.
    auto habit = step(QStringLiteral("你習慣哪一套注音？"),
        QStringLiteral("會調整選字鍵、候選窗與 Esc 等細節；鍵盤配置與智慧型中英文不變。"));
    presets_ = new QButtonGroup(this);
    for (const auto item : SettingsStore::schema().value("presets").toArray()) {
        const auto preset = item.toObject();
        auto* card = new QFrame; card->setObjectName("setupPresetCard");
        auto* cardLayout = new QVBoxLayout(card); cardLayout->setContentsMargins(14, 10, 14, 10); cardLayout->setSpacing(4);
        auto* option = new QRadioButton(preset.value("label").toString());
        option->setObjectName("setupPreset_" + preset.value("key").toString());
        option->setProperty("value", preset.value("key").toString());
        option->setAccessibleDescription(preset.value("description").toString());
        auto* description = paragraph(preset.value("description").toString(), "help");
        description->setContentsMargins(26, 0, 0, 0);
        cardLayout->addWidget(option); cardLayout->addWidget(description);
        // The whole card selects the habit, not only the small radio button.
        card->setCursor(Qt::PointingHandCursor); card->installEventFilter(this);
        presets_->addButton(option); habit.layout->addWidget(card);
    }
    presetNote_ = paragraph(QStringLiteral("目前是自訂設定；選擇其中一套會取代自訂的細節。"), "muted");
    presetNote_->setObjectName("setupPresetNote");
    habit.layout->addWidget(presetNote_);
    habit.layout->addStretch();
    steps_->addWidget(habit.area);
    connect(presets_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* option) {
        settings_->applyPreset(option->property("value").toString());
        sync();
    });

    // 3. Smart Chinese-English.
    auto mixed = step(QStringLiteral("中英文混著打？"),
        QStringLiteral("開啟後不用切換模式，輸入法會判斷你打的是英文還是注音；結果仍可改選。"));
    auto* example = new QHBoxLayout; example->setSpacing(16);
    example->addWidget(paragraph(QStringLiteral("hello283"), "guideExample"));
    example->addWidget(paragraph(QStringLiteral("→"), "guideAnnotation"));
    example->addWidget(paragraph(QStringLiteral("hello打"), "guideExample"));
    example->addStretch();
    mixed.layout->addLayout(example);
    mixed.layout->addWidget(paragraph(QStringLiteral("以標準注音鍵盤為例：283 是「ㄉㄚˇ」。"), "muted"));
    auto* card = new QFrame; card->setObjectName("settingsCard");
    auto* row = new QHBoxLayout(card); row->setContentsMargins(16, 12, 16, 12);
    auto* smartLabel = new QLabel(schemaField("smart_english").value("label").toString());
    smart_ = new NativeSwitch; smart_->setObjectName("setupSmartEnglish");
    smart_->setAccessibleName(smartLabel->text()); smartLabel->setBuddy(smart_);
    row->addWidget(smartLabel, 1); row->addWidget(smart_);
    mixed.layout->addWidget(card);
    mixed.layout->addStretch();
    steps_->addWidget(mixed.area);
    connect(smart_, &QCheckBox::clicked, this, [this](bool checked) { settings_->setValue("smart_english", checked); });

    // 4. Try it, and where to go next.
    auto finish = step(QStringLiteral("完成了，試打看看"),
        QStringLiteral("設定已自動儲存，之後可以在「輸入法設定」調整。"));
    auto* tryInput = new QLineEdit; tryInput->setObjectName("setupTryInput"); tryInput->setMinimumHeight(40);
    tryInput->setPlaceholderText(QStringLiteral("切換到拉風輸入法，在這裡打字"));
    tryInput->setAccessibleName(QStringLiteral("試打"));
    finish.layout->addWidget(tryInput);
#ifdef Q_OS_MACOS
    finish.layout->addWidget(paragraph(QStringLiteral("從選單列的輸入來源選單切換到「拉風輸入法」。找不到時，請看使用指南的「疑難排解」。"), "muted"));
#else
    finish.layout->addWidget(paragraph(QStringLiteral("用 Fcitx5 的切換鍵（預設是 Ctrl+Space）切換到拉風輸入法。找不到時，請看使用指南的「疑難排解」。"), "muted"));
#endif
    finish.layout->addSpacing(8);
    finish.layout->addWidget(paragraph(QStringLiteral("之後可以做"), "section"));
    const auto later = [this, &finish](const QString& title, const QString& detail, const QString& text, const char* name, const QString& page) {
        auto* line = new QHBoxLayout; line->setSpacing(14);
        auto* words = new QVBoxLayout; words->setSpacing(2);
        words->addWidget(paragraph(title)); words->addWidget(paragraph(detail, "muted"));
        line->addLayout(words, 1);
        auto* open = actionButton(text, name); line->addWidget(open, 0, Qt::AlignVCenter);
        finish.layout->addLayout(line);
        connect(open, &QPushButton::clicked, this, [this, page] { emit pageRequested(page); accept(); });
    };
    later(QStringLiteral("替代詞彙"), QStringLiteral("讓人名與專有名詞一次選對，不必訓練模型。"),
          QStringLiteral("管理替代詞彙…"), "setupPhrases", QStringLiteral("phrases"));
    later(QStringLiteral("個人化"), QStringLiteral("用自己的輸入紀錄在本機訓練模型；收集預設關閉。"),
          QStringLiteral("了解個人化…"), "setupPersonalization", QStringLiteral("records"));
    finish.layout->addStretch();
    steps_->addWidget(finish.area);

    auto* navigation = new QHBoxLayout;
    previous_ = actionButton(QStringLiteral("上一步"), "setupPrevious");
    skip_ = actionButton(QStringLiteral("略過"), "setupSkip"); skip_->setProperty("quiet", true);
    skip_->setToolTip(QStringLiteral("保留目前的設定；之後可以從使用指南重新開啟。"));
    next_ = actionButton({}, "setupNext"); next_->setProperty("primary", true); next_->setMinimumWidth(140);
    navigation->addWidget(previous_); navigation->addWidget(skip_); navigation->addStretch(); navigation->addWidget(next_);
    layout->addLayout(navigation);
    connect(previous_, &QPushButton::clicked, this, [this] { steps_->setCurrentIndex(steps_->currentIndex() - 1); updateStep(); });
    connect(next_, &QPushButton::clicked, this, [this] {
        if (steps_->currentIndex() == steps_->count() - 1) accept();
        else { steps_->setCurrentIndex(steps_->currentIndex() + 1); updateStep(); }
    });
    connect(skip_, &QPushButton::clicked, this, &QDialog::reject);
    connect(settings_, &SettingsPage::valuesChanged, this, [this] { if (isVisible()) sync(); });
    updateStep();
}

bool SetupWizard::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease && watched->objectName() == "setupPresetCard"
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        if (auto* option = watched->findChild<QRadioButton*>()) option->click();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void SetupWizard::restart() {
    steps_->setCurrentIndex(0);
    updateStep();
}

void SetupWizard::showEvent(QShowEvent* event) {
    sync();
    QDialog::showEvent(event);
}

void SetupWizard::done(int result) {
    // Closing in any way counts as seen: the setup never asks twice.
    settings_->flushPending();
    (void)SetupPreferences::markSeen();
    QDialog::done(result);
}

void SetupWizard::sync() {
    const auto layout = settings_->value("keyboard_layout").toString();
    for (auto* option : layouts_->buttons())
        if (option->property("value").toString() == layout) option->setChecked(true);
    describeLayout(layout);
    const auto preset = settings_->matchingPreset();
    // No habit is selected while the settings are customized.
    presets_->setExclusive(false);
    for (auto* option : presets_->buttons()) {
        const bool selected = option->property("value").toString() == preset;
        option->setChecked(selected);
        auto* card = option->parentWidget();
        card->setProperty("selected", selected);
        card->style()->unpolish(card); card->style()->polish(card);
    }
    presets_->setExclusive(true);
    presetNote_->setVisible(preset.isEmpty());
    smart_->setChecked(settings_->value("smart_english").toBool());
}

void SetupWizard::describeLayout(const QString& layout) {
    preview_->setLayoutName(layout);
    if (layout == "standard") layoutNote_->setText(QStringLiteral("不確定就選「標準」：鍵盤上印的注音就是標準配置。"));
    else if (llavon::ime::is_compact_bopomofo_layout(llavon::ime::bopomofo_keyboard_layout(layout.toStdString())))
        layoutNote_->setText(QStringLiteral("這種配置一個鍵代表多個注音，輸入法會依前後判斷；圖上只顯示每個鍵的第一個注音。"));
    else layoutNote_->setText(QStringLiteral("圖上是每個鍵輸入的注音。"));
}

void SetupWizard::updateStep() {
    const int index = steps_->currentIndex(), last = steps_->count() - 1;
    progress_->setText(QStringLiteral("開始使用 · %1 / %2").arg(index + 1).arg(steps_->count()));
    previous_->setEnabled(index > 0);
    skip_->setVisible(index < last);
    next_->setText(index == last ? QStringLiteral("完成") : QStringLiteral("下一步"));
}

} // namespace llavon::lora
