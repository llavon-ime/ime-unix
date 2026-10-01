#pragma once

#include "phrase_table.hpp"
#include <QWidget>

class QVBoxLayout;
class QScrollArea;
class QLabel;

namespace llavon::lora {

class PhraseRow;

class PhraseList final : public QWidget {
    Q_OBJECT
public:
    explicit PhraseList(const QString& tablePath, QWidget* parent = nullptr);
    void loadText(const QString& text);
    QString text() const;
    QStringList errors();
    void addRow();
    QString tablePath() const { return table_.path(); }
signals:
    void changed();
private:
    struct Entry { QString literal; PhraseRow* row = nullptr; };
    void append(const QString& phrase, const QStringList& readings, const QString& raw = {});
    void renumber();
    PhraseTable table_;
    QList<Entry> entries_;
    QVBoxLayout* rows_;
    QScrollArea* scroll_;
    QLabel* empty_;
};

} // namespace llavon::lora
