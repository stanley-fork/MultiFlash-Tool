#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QRadioButton>
#include <QCheckBox>
#include <QPushButton>

class QPaintEvent;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    bool isApiTheme() const;
    QString apiUrl() const;
    QString localWallpaperPath() const;
    bool autoSaveLog() const;
    QString logSavePath() const;
    bool showTimestamp() const;
    bool showDetailLog() const;
    bool autoDetectDevice() const;
    bool confirmBeforeAction() const;
    QString realmeApiUrl() const;
    QString realmeRcsmAccount() const;
    QString realmeRcsmKey() const;
    /** 写入 misc 后重启到 Fastboot / Recovery 的镜像路径（可为空则用程序目录默认名） */
    QString miscFastbootImagePath() const;
    QString miscRecoveryImagePath() const;
    /** EDL Admin 服务根地址，如 http://127.0.0.1:8088（机型列表 / 更新策略 / 文件缓存） */
    QString cloudEdlBaseUrl() const;

    void setApiUrl(const QString &url);
    void setLocalWallpaperPath(const QString &path);
    void setApiThemeMode(bool api);
    void setAutoSaveLog(bool on);
    void setLogSavePath(const QString &path);
    void setShowTimestamp(bool on);
    void setShowDetailLog(bool on);
    void setAutoDetectDevice(bool on);
    void setConfirmBeforeAction(bool on);
    void setRealmeApiUrl(const QString &url);
    void setRealmeRcsmAccount(const QString &acc);
    void setRealmeRcsmKey(const QString &key);
    void setMiscFastbootImagePath(const QString &path);
    void setMiscRecoveryImagePath(const QString &path);
    void setCloudEdlBaseUrl(const QString &url);

signals:
    void themeChanged();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void buildUi();

    QRadioButton *m_apiRadio;
    QRadioButton *m_localRadio;
    QLineEdit *m_apiUrlEdit;
    QLineEdit *m_localPathEdit;
    QPushButton *m_localBrowseBtn;

    QCheckBox *m_autoSaveLogCheck;
    QLineEdit *m_logPathEdit;
    QPushButton *m_logBrowseBtn;
    QCheckBox *m_timestampCheck;
    QCheckBox *m_detailLogCheck;

    QCheckBox *m_autoDetectCheck;
    QCheckBox *m_confirmCheck;

    QLineEdit *m_realmeApiUrlEdit;
    QLineEdit *m_realmeRcsmAccountEdit;
    QLineEdit *m_realmeRcsmKeyEdit;

    QLineEdit *m_miscFastbootPathEdit;
    QLineEdit *m_miscRecoveryPathEdit;
    QPushButton *m_miscFastbootBrowseBtn;
    QPushButton *m_miscRecoveryBrowseBtn;

    QLineEdit *m_cloudEdlBaseUrlEdit;

    QPushButton *m_saveBtn;
    QPushButton *m_cancelBtn;
};

#endif // SETTINGSDIALOG_H
