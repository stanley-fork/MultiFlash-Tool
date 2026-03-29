#include "edl_api_client.h"

#include <QJsonParseError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QCoreApplication>

QString EdlApi::defaultLocalAdminBaseUrl()
{
    return QStringLiteral("http://127.0.0.1:8088");
}

QString EdlApi::defaultCloudEdlBaseUrl()
{
    return QStringLiteral("https://api.xiriacg.top");
}

QString EdlApi::normalizeBaseUrl(const QString &s)
{
    QString b = s.trimmed();
    while (b.endsWith(QLatin1Char('/')))
        b.chop(1);
    return b;
}

QUrl EdlApi::apiUrl(const QString &base, const QString &pathSuffix)
{
    QUrl b = QUrl::fromUserInput(normalizeBaseUrl(base));
    QString sp = pathSuffix.startsWith(QLatin1Char('/')) ? pathSuffix : (QLatin1Char('/') + pathSuffix);
    return b.resolved(QUrl(sp));
}

QUrl EdlApi::fileDownloadUrl(const QString &base, const QString &relativeStoragePath)
{
    QString rel = relativeStoragePath;
    while (rel.startsWith(QLatin1Char('/')))
        rel = rel.mid(1);
    return apiUrl(base, QStringLiteral("/api/v1/files/") + rel);
}

namespace {

void setCommonRequestHeaders(QNetworkRequest &req)
{
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("SAKURAEDL/%1 (Windows; Qt; EDL-Admin-Client)")
                      .arg(QCoreApplication::applicationVersion()));
    /* 允许 HTTP/2（服务端支持时减少往返延迟） */
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
}

/** 小 JSON 请求：较短超时 + 一次重试，避免偶发抖动时长时间等待 */
constexpr int kJsonTransferTimeoutMs = 20000;

static bool shouldRetryJsonRequest(QNetworkReply::NetworkError e)
{
    switch (e) {
    case QNetworkReply::TimeoutError:
    /* setTransferTimeout 触发时多为 OperationCanceledError */
    case QNetworkReply::OperationCanceledError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        return true;
    default:
        return false;
    }
}

static void getJsonAttempt(QNetworkAccessManager *nam, const QUrl &url, QObject *context,
                           int attempt,
                           std::function<void(QJsonDocument, QString)> onDone)
{
    QNetworkRequest req(url);
    setCommonRequestHeaders(req);
    req.setTransferTimeout(kJsonTransferTimeoutMs);
    QNetworkReply *reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, nam, url, context, attempt, onDone]() {
        const QNetworkReply::NetworkError ne = reply->error();
        if (ne != QNetworkReply::NoError) {
            const QString err = reply->errorString();
            reply->deleteLater();
            if (attempt == 0 && shouldRetryJsonRequest(ne)) {
                QTimer::singleShot(450, context, [nam, url, context, onDone]() {
                    getJsonAttempt(nam, url, context, 1, onDone);
                });
                return;
            }
            onDone(QJsonDocument(), err);
            return;
        }
        const QByteArray data = reply->readAll();
        reply->deleteLater();
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
        if (pe.error != QJsonParseError::NoError) {
            onDone(QJsonDocument(), pe.errorString());
            return;
        }
        onDone(doc, QString());
    });
}

} // namespace

void EdlApi::getJson(QNetworkAccessManager *nam, const QUrl &url, QObject *context,
                     std::function<void(QJsonDocument, QString)> onDone)
{
    getJsonAttempt(nam, url, context, 0, std::move(onDone));
}

void EdlApi::fetchHealth(QNetworkAccessManager *nam, const QString &baseUrl, QObject *context,
                         std::function<void(bool, QString)> onDone)
{
    const QUrl u = apiUrl(baseUrl, QStringLiteral("/api/v1/health"));
    getJson(nam, u, context, [onDone](QJsonDocument doc, QString err) {
        if (!err.isEmpty()) {
            onDone(false, err);
            return;
        }
        const QJsonObject o = doc.object();
        onDone(o.value(QStringLiteral("ok")).toBool(), QString());
    });
}

void EdlApi::fetchUpdateInfo(QNetworkAccessManager *nam, const QString &baseUrl, QObject *context,
                             std::function<void(QJsonObject, QString)> onDone)
{
    const QUrl u = apiUrl(baseUrl, QStringLiteral("/api/v1/update-info"));
    getJson(nam, u, context, [onDone](QJsonDocument doc, QString err) {
        if (!err.isEmpty()) {
            onDone(QJsonObject(), err);
            return;
        }
        if (!doc.isObject()) {
            onDone(QJsonObject(), QStringLiteral("invalid JSON"));
            return;
        }
        onDone(doc.object(), QString());
    });
}

void EdlApi::fetchDeviceModels(QNetworkAccessManager *nam, const QString &baseUrl, QObject *context,
                               std::function<void(QJsonArray, QString)> onDone)
{
    const QUrl u = apiUrl(baseUrl, QStringLiteral("/api/v1/device-models"));
    getJson(nam, u, context, [onDone](QJsonDocument doc, QString err) {
        if (!err.isEmpty()) {
            onDone(QJsonArray(), err);
            return;
        }
        if (!doc.isArray()) {
            onDone(QJsonArray(), QStringLiteral("not a JSON array"));
            return;
        }
        onDone(doc.array(), QString());
    });
}

void EdlApi::downloadToFile(QNetworkAccessManager *nam, const QString &baseUrl,
                            const QString &relativeStoragePath, const QString &localFilePath,
                            QObject *context, std::function<void(QString)> onDone)
{
    const QFileInfo outFi(localFilePath);
    QDir().mkpath(outFi.absolutePath());

    const QUrl u = fileDownloadUrl(baseUrl, relativeStoragePath);
    QNetworkRequest req(u);
    setCommonRequestHeaders(req);
    req.setTransferTimeout(300000); /* 大文件 Firehose，5 分钟 */
    QNetworkReply *reply = nam->get(req);
    auto *outFile = new QFile(localFilePath);

    QObject::connect(reply, &QNetworkReply::readyRead, context, [reply, outFile]() {
        if (!outFile->isOpen()) {
            if (!outFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                reply->abort();
                return;
            }
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty())
            outFile->write(chunk);
    });

    QObject::connect(reply, &QNetworkReply::finished, context,
                     [reply, outFile, localFilePath, onDone]() {
        auto fail = [&](const QString &msg) {
            if (outFile->isOpen())
                outFile->close();
            outFile->remove();
            delete outFile;
            onDone(msg);
            reply->deleteLater();
        };

        if (reply->error() != QNetworkReply::NoError) {
            QString es = reply->errorString();
            if (es.contains(QLatin1String("Not Found"), Qt::CaseInsensitive)
                || es.contains(QLatin1String("404"), Qt::CaseInsensitive)) {
                es += QStringLiteral(
                    "\n\n"
                    "（404：管理端 data/uploads 下没有该文件。请在 edl-admin 后台重新上传 Firehose，"
                    "或确认后端工作目录与 data/uploads 一致。）");
            }
            fail(es);
            return;
        }

        const int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpCode > 0 && (httpCode < 200 || httpCode >= 300)) {
            QString es = QStringLiteral("HTTP %1").arg(httpCode);
            if (httpCode == 404) {
                es += QStringLiteral(
                    " — 服务端未找到文件，请在管理后台重新上传或检查 data/uploads。");
            }
            fail(es);
            return;
        }

        if (!outFile->isOpen()) {
            if (!outFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                fail(outFile->errorString());
                return;
            }
        }
        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty())
            outFile->write(rest);
        outFile->flush();
        outFile->close();

        const qint64 sz = QFileInfo(localFilePath).size();
        if (sz <= 0) {
            outFile->remove();
            delete outFile;
            onDone(QStringLiteral("下载内容为空"));
            reply->deleteLater();
            return;
        }

        /* 部分 CDN/反代在异常时仍返回 200 + JSON，避免把 JSON 当二进制写入 .elf */
        QFile rd(localFilePath);
        if (rd.open(QIODevice::ReadOnly)) {
            const QByteArray head = rd.read(qMin(rd.size(), qint64(2048)));
            rd.close();
            if (head.trimmed().startsWith('{')) {
                QJsonParseError pe{};
                const QJsonDocument jd = QJsonDocument::fromJson(head, &pe);
                if (pe.error == QJsonParseError::NoError && jd.isObject()
                    && jd.object().contains(QStringLiteral("error"))) {
                    outFile->remove();
                    delete outFile;
                    onDone(QStringLiteral("服务端返回错误：%1")
                               .arg(jd.object().value(QStringLiteral("error")).toString()));
                    reply->deleteLater();
                    return;
                }
            }
        }

        delete outFile;
        onDone(QString());
        reply->deleteLater();
    });
}
