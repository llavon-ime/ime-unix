#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
class QLabel;
class QCheckBox;
class QPushButton;
namespace llavon::lora {
class HostSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit HostSettingsPage(bool updates, QWidget* parent = nullptr, QString helper = {});
    void refresh();
private:
    void request(const QJsonObject& request);
    void render(const QJsonObject& state);
    QLabel *version_, *context_, *status_;
    QCheckBox *checks_ = nullptr, *downloads_ = nullptr;
    QPushButton* check_ = nullptr;
    QProcess process_;
    QTimer timer_;
    QTimer watchdog_;
    QString helper_;
    bool updates_;
};
}
