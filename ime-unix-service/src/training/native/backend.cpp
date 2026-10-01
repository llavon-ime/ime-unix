#include "manager.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QDir>
#include <utility>

namespace llavon::lora {

QString Backend::phraseTablePath() const {
    const auto index = arguments_.indexOf("--tables-dir");
    return index >= 0 && index + 1 < arguments_.size()
        ? QDir(arguments_[index + 1]).filePath("bopomofo_char.json") : QString();
}

Backend::Backend(QString program, QStringList arguments, QObject* parent)
    : QObject(parent), program_(std::move(program)), arguments_(std::move(arguments)) {
    connect(&process_, &QProcess::started, this, &Backend::ready);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        input_ += process_.readAllStandardOutput();
        while (true) {
            const auto end = input_.indexOf('\n');
            if (end < 0) break;
            const auto line = input_.left(end);
            input_.remove(0, end + 1);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(line, &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                emit error(QStringLiteral("管理器回覆格式錯誤"));
                continue;
            }
            const auto response = document.object();
            auto reply = replies_.take(response.value("id").toInt());
            const auto failure = response.value("error").toString();
            if (!failure.isEmpty()) emit error(failure);
            if (reply) reply(failure.isEmpty() ? response.value("result") : QJsonValue());
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit error(QStringLiteral("無法啟動本機管理器：%1").arg(process_.errorString()));
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        const auto detail = QString::fromUtf8(process_.readAllStandardError()).trimmed();
        emit error(QStringLiteral("本機管理器已結束%1").arg(detail.isEmpty() ? QString() : "：" + detail));
        const auto callbacks = std::exchange(replies_, {});
        for (const auto& reply : callbacks) if (reply) reply({});
        Q_UNUSED(code)
        Q_UNUSED(status)
    });
}

Backend::~Backend() {
    process_.disconnect(this);
    process_.closeWriteChannel();
    if (!process_.waitForFinished(3500)) {
        process_.terminate();
        if (!process_.waitForFinished(3500)) { process_.kill(); process_.waitForFinished(); }
    }
}

void Backend::start() {
    if (process_.state() == QProcess::NotRunning) process_.start(program_, arguments_);
}

void Backend::request(const QString& path, QJsonObject body, Reply reply, bool post) {
    if (process_.state() != QProcess::Running) {
        emit error(QStringLiteral("本機管理器尚未就緒"));
        if (reply) reply({});
        return;
    }
    const int id = ++serial_;
    replies_.insert(id, std::move(reply));
    QJsonObject request{{"id", id}, {"path", path}, {"method", post ? "POST" : "GET"}, {"body", body}};
    auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    process_.write(bytes);
    bytes.fill('\0');
}

} // namespace llavon::lora
