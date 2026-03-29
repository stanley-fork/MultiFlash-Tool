#ifndef DEVICEDIALOG_H
#define DEVICEDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QMap>
#include <QVector>

class QPaintEvent;
class QStackedWidget;
class QNetworkAccessManager;
class QShowEvent;

struct DeviceEntry {
    QString id;
    QString chipset;
    QString device;
    QString brand;
    QString firehose;
    /** none | xiaomi | realme | vip | oneplus */
    QString authKind;
    QMap<QString, QString> authParams;
    /** 非空：来自 EDL Admin 云端；用于下载 uploads 下文件 */
    QString cloudBaseUrl;
};

class DeviceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DeviceDialog(QWidget *parent = nullptr);

    DeviceEntry selectedDevice() const;

    void setNetworkAccessManager(QNetworkAccessManager *nam);
    void setCloudBaseUrl(const QString &baseUrl);

signals:
    void deviceSelected(const DeviceEntry &dev);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void done(int r) override;

private:
    void buildUi();
    void populateDevices();
    void rebuildBrandCombo();
    /** 搜索 + 品牌下拉，表格首列 UserRole 存 m_allDevices 下标（排序后仍可选中正确机型） */
    void applyFilter();
    void loadCloudDeviceList();
    /** 表格无行时根据云端状态设置 m_emptyHint 文案 */
    void updateEmptyHint();

    QLineEdit *m_searchEdit;
    QComboBox *m_brandCombo;
    QStackedWidget *m_deviceStack;
    QTableWidget *m_table;
    QLabel *m_emptyHint;
    QLabel *m_countLabel;
    QPushButton *m_selectBtn;
    QPushButton *m_cancelBtn;

    QVector<DeviceEntry> m_allDevices;
    QNetworkAccessManager *m_netManager = nullptr;
    QString m_cloudBaseUrl;
    bool m_cloudFetchStarted = false;
    /** 首次拉取是否已结束（成功或失败） */
    bool m_cloudFetchDone = false;
    bool m_cloudFetchError = false;
};

#endif // DEVICEDIALOG_H
