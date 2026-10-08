#include "phrase_list.hpp"
#include "controls.hpp"

#include "phrase_override/phrase_override_store.hpp"
#include "text/utf.hpp"
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QResizeEvent>
#include <algorithm>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

namespace llavon::lora {

class PhraseRow final : public QFrame {
public:
    ~PhraseRow() override;
    PhraseRow(PhraseTable& table, const QString& phrase, const QStringList& readings, const QString& raw)
        : table_(table), raw_(raw) {
        setObjectName("phraseRow");
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(16, 14, 16, 14); layout->setSpacing(12);
        auto* header = new QHBoxLayout;
        number = new QLabel; number->setObjectName("phraseNumber"); number->setFixedWidth(22);
        word = new QLineEdit(phrase); word->setObjectName("phraseWord");
        word->setPlaceholderText(QStringLiteral("輸入詞彙，例如：李拉風")); word->setMinimumHeight(34);
        word->setMaximumWidth(280);
        remove = new QPushButton(QStringLiteral("刪除")); remove->setObjectName("removePhrase");
        remove->setProperty("quiet", true); remove->setProperty("destructive", true);
        header->addWidget(number); header->addWidget(word, 1); header->addStretch(); header->addWidget(remove); layout->addLayout(header);
        grid_ = new QGridLayout; grid_->setSpacing(8); layout->addLayout(grid_);
        message_ = new QLabel; message_->setObjectName("phraseRowStatus"); message_->setWordWrap(true);
        message_->setTextFormat(Qt::PlainText); layout->addWidget(message_);
        rebuild(readings);
        connect(word, &QLineEdit::textChanged, this, [this] {
            QStringList previous;
            const auto characters = word->text().trimmed().toUcs4();
            for (qsizetype i = 0; i < characters.size(); ++i)
                previous.append(i < characters_.size() && characters[i] == characters_[i] && i < choices_.size()
                    ? choices_[i]->currentData().toString() : QString());
            raw_.clear(); rebuild(previous); if (edited) edited();
        });
    }

    QString line() const {
        if (!raw_.isEmpty()) return raw_;
        QStringList readings;
        for (auto* choice : choices_) readings.append(choice->currentData().toString());
        return word->text().trimmed() + ' ' + readings.join('-');
    }

    QString issue() const {
        if (!table_.error().isEmpty()) return table_.error();
        if (!raw_.isEmpty()) return QStringLiteral("原有資料格式無效，請在這一列修正詞彙並選擇讀音，或刪除此列。");
        if (characters_.size() < 2 || characters_.size() > 8) return QStringLiteral("請輸入 2–8 個字。");
        for (const auto value : characters_) {
            const auto scalar = static_cast<char32_t>(value);
            const auto character = QString::fromUcs4(&scalar, 1);
            if (table_.readings(character).isEmpty()) return QStringLiteral("「%1」不在目前注音表內，請確認詞彙。").arg(character);
        }
        for (auto* choice : choices_) if (choice->currentIndex() < 0) return QStringLiteral("請為每個字選擇讀音；多音字不會自動猜測。");
        const auto errors = table_.check(line());
        return errors.isEmpty() ? QString() : errors.front().mid(errors.front().indexOf(QStringLiteral("：")) + 1);
    }

    void showIssue(const QString& issue) {
        message_->setText(issue.isEmpty() ? QStringLiteral("字音相符") : issue);
        message_->setProperty("validation", issue.isEmpty() ? "valid" : "invalid");
        message_->style()->unpolish(message_); message_->style()->polish(message_);
        message_->setVisible(!issue.isEmpty());
    }

    QLabel* number;
    QLineEdit* word;
    QPushButton* remove;
    std::function<void()> edited;
private:
    void resizeEvent(QResizeEvent* event) override {
        QFrame::resizeEvent(event);
        const auto columns = std::clamp((event->size().width() - 32) / 140, 2, 4);
        if (columns == columns_) return;
        columns_ = columns;
        for (int column = 0; column <= 4; ++column) grid_->setColumnStretch(column, column == columns_ ? 1 : 0);
        for (qsizetype i = 0; i < cells_.size(); ++i) {
            grid_->removeWidget(cells_[i]);
            grid_->addWidget(cells_[i], static_cast<int>(i) / columns_, static_cast<int>(i) % columns_);
        }
    }
    void rebuild(const QStringList& readings) {
        while (auto* item = grid_->takeAt(0)) { delete item->widget(); delete item; }
        cells_.clear();
        choices_.clear(); characters_ = word->text().trimmed().toUcs4();
        for (int column = 0; column < 4; ++column) { grid_->setColumnStretch(column, 0); grid_->setColumnMinimumWidth(column, 0); }
        grid_->setColumnStretch(columns_, 1);
        if (characters_.size() >= 2 && characters_.size() <= 8) {
            for (qsizetype i = 0; i < characters_.size(); ++i) {
                const auto scalar = static_cast<char32_t>(characters_[i]);
                const auto character = QString::fromUcs4(&scalar, 1);
                const auto available = table_.readings(character);
                auto* cell = new QWidget; auto* cellLayout = new QHBoxLayout(cell); cellLayout->setContentsMargins(0, 0, 0, 0); cellLayout->setSpacing(4);
                auto* label = new QLabel(character); label->setTextFormat(Qt::PlainText);
                auto* choice = new NativeComboBox; choice->setObjectName(QStringLiteral("phraseReading_%1").arg(i));
                choice->setAccessibleName(QStringLiteral("「%1」的讀音").arg(character)); choice->setMinimumHeight(34);
                choice->setMaximumWidth(130);
                choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); choice->setMinimumContentsLength(3);
                choice->setPlaceholderText(available.isEmpty() ? QStringLiteral("查無此字") : QStringLiteral("選讀音"));
                for (const auto& reading : available) choice->addItem(reading, reading);
                const auto selected = readings.value(i);
                if (!selected.isEmpty() && !available.contains(selected)) choice->addItem(selected + QStringLiteral("（不符）"), selected);
                choice->setCurrentIndex(!selected.isEmpty() ? choice->findData(selected) : available.size() == 1 ? 0 : -1);
                cellLayout->addWidget(label); cellLayout->addWidget(choice, 1); cellLayout->addStretch();
                grid_->addWidget(cell, static_cast<int>(i) / columns_, static_cast<int>(i) % columns_);
                choices_.append(choice); cells_.append(cell);
                connect(choice, &QComboBox::currentIndexChanged, this, [this] { if (edited) edited(); });
            }
        }
        showIssue(issue());
    }
    PhraseTable& table_;
    QString raw_;
    QList<uint> characters_;
    QList<QComboBox*> choices_;
    QList<QWidget*> cells_;
    int columns_ = 2;
    QGridLayout* grid_;
    QLabel* message_;
};

PhraseRow::~PhraseRow() = default;

PhraseList::PhraseList(const QString& tablePath, QWidget* parent) : QWidget(parent), table_(tablePath) {
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea; scroll_->setObjectName("phraseScrollArea"); scroll_->setFrameShape(QFrame::NoFrame); scroll_->setWidgetResizable(true);
    auto* content = new QWidget; content->setObjectName("settingsForm"); content->setAttribute(Qt::WA_StyledBackground, true);
    rows_ = new QVBoxLayout(content); rows_->setContentsMargins(0, 0, 8, 0); rows_->setSpacing(10);
    empty_ = new QWidget; empty_->setObjectName("phraseEmptyState");
    auto* emptyLayout = new QVBoxLayout(empty_); emptyLayout->setContentsMargins(16, 24, 16, 24); emptyLayout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("加入常用詞"));
    title->setProperty("role", "section"); title->setWordWrap(true); emptyLayout->addWidget(title);
    auto* description = new QLabel(QStringLiteral("例如「李拉風」：ㄌㄧˇ-ㄌㄚ-ㄈㄥ。相同注音會優先使用指定寫法。"));
    description->setObjectName("phraseEmptyDescription"); description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true); description->setProperty("role", "muted"); emptyLayout->addWidget(description);
    auto* add = new QPushButton(QStringLiteral("新增我的常用詞")); add->setObjectName("addFirstPhrase"); add->setProperty("primary", true);
    emptyLayout->addWidget(add, 0, Qt::AlignLeft); connect(add, &QPushButton::clicked, this, &PhraseList::addRow);
    rows_->addWidget(empty_); rows_->addStretch();
    scroll_->setWidget(content); layout->addWidget(scroll_);
}

void PhraseList::append(const QString& phrase, const QStringList& readings, const QString& raw) {
    auto* row = new PhraseRow(table_, phrase, readings, raw);
    entries_.append(Entry{{}, row}); rows_->insertWidget(rows_->count() - 1, row);
    row->edited = [this] { errors(); emit changed(); };
    connect(row->remove, &QPushButton::clicked, this, [this, row] {
        for (qsizetype i = 0; i < entries_.size(); ++i) if (entries_[i].row == row) { entries_.removeAt(i); break; }
        rows_->removeWidget(row); row->setProperty("phraseIndex", -1); row->hide(); row->deleteLater(); renumber(); errors(); emit changed();
    });
    renumber();
}

void PhraseList::renumber() {
    int index = 0;
    for (const auto& entry : entries_) if (entry.row) {
        entry.row->number->setText(QStringLiteral("%1").arg(++index, 2, 10, QLatin1Char('0')));
        entry.row->setProperty("phraseIndex", index - 1);
        entry.row->word->setAccessibleName(QStringLiteral("第 %1 列詞彙").arg(index));
    }
    empty_->setVisible(index == 0);
}

void PhraseList::loadText(const QString& text) {
    for (const auto& entry : entries_) if (entry.row) { rows_->removeWidget(entry.row); delete entry.row; }
    entries_.clear(); table_ = PhraseTable(table_.path());
    auto lines = text.split('\n');
    if (lines.last().isEmpty()) lines.removeLast();
    for (const auto& line : lines) {
        if (line.trimmed().isEmpty() || line.startsWith('#')) { entries_.append(Entry{line, nullptr}); continue; }
        const auto record = llavon::ime::PhraseOverrideStore::parse_line(line.toStdString());
        if (!record || !llavon::ime::PhraseOverrideStore::valid_entry(record->phrase, record->readings.size())) {
            append(line, {}, line); continue;
        }
        QStringList readings;
        for (const auto& reading : record->readings) readings.append(QString::fromStdString(llavon::ime::u16_to_utf8(reading)));
        append(QString::fromStdString(llavon::ime::u16_to_utf8(record->phrase)), readings);
    }
    renumber(); errors();
}

void PhraseList::addRow() {
    append({}, {}); auto* row = entries_.last().row; row->word->setFocus(); errors(); emit changed();
    QTimer::singleShot(0, row, [this, row] { scroll_->ensureWidgetVisible(row->word); });
}

QString PhraseList::text() const {
    QStringList lines;
    for (const auto& entry : entries_) lines.append(entry.row ? entry.row->line() : entry.literal);
    return lines.isEmpty() ? QString() : lines.join('\n') + '\n';
}

QStringList PhraseList::errors() {
    table_ = PhraseTable(table_.path());
    QStringList errors;
    QMap<QString, int> seen;
    int index = 0;
    for (const auto& entry : entries_) if (entry.row) {
        ++index;
        auto issue = entry.row->issue();
        if (issue.isEmpty()) {
            const auto record = llavon::ime::PhraseOverrideStore::parse_line(entry.row->line().toStdString());
            const auto key = QString::fromStdString(llavon::ime::PhraseOverrideStore::format_readings(record->readings));
            if (seen.contains(key)) issue = QStringLiteral("注音與第 %1 列重複，請修改或刪除其中一列。").arg(seen.value(key));
            else seen.insert(key, index);
        }
        entry.row->showIssue(issue);
        if (!issue.isEmpty()) errors.append(QStringLiteral("第 %1 列：%2").arg(index).arg(issue));
    }
    if (errors.isEmpty() && !table_.error().isEmpty()) errors.append(table_.error());
    return errors;
}

} // namespace llavon::lora
