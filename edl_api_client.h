#ifndef EDL_API_CLIENT_H
#define EDL_API_CLIENT_H

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QObject;

/**
 * EDL Admin HTTP API（/api/v1）客户端封装，供 Qt 界面拉取机型、更新信息与上传文件缓存。
 */
namespace EdlApi {

/** 本地联调默认地址（与 edl-admin 默认监听端口一致） */
QString defaultLocalAdminBaseUrl();
/** 公网管理端默认 Base URL（未在设置中保存 cloud/edlBaseUrl 时使用） */
QString defaultCloudEdlBaseUrl();

QString normalizeBaseUrl(const QString &s);
QUrl apiUrl(const QString &base, const QString &pathSuffix);
/** 下载 data 下 uploads 相对路径，如 uploads/xxx.elf */
QUrl fileDownloadUrl(const QString &base, const QString &relativeStoragePath);

void getJson(QNetworkAccessManager *nam, const QUrl &url, QObject *context,
             std::function<void(QJsonDocument doc, QString error)> onDone);

void fetchHealth(QNetworkAccessManager *nam, const QString &baseUrl, QObject *context,
                 std::function<void(bool ok, QString error)> onDone);

void fetchUpdateInfo(QNetworkAccessManager *nam, const QString &baseUrl, QObject *context,
                     std::function<void(QJsonObject obj, QString error)> onDone);

void fetchDeviceModels(QNetworkAccessManager *nam, const QString &baseUrl, QObject *context,
                       std::function<void(QJsonArray arr, QString error)> onDone);

void downloadToFile(QNetworkAccessManager *nam, const QString &baseUrl, const QString &relativeStoragePath,
                    const QString &localFilePath, QObject *context,
                    std::function<void(QString error)> onDone);

} // namespace EdlApi

#endif
