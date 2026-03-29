#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMouseEvent>
#include <QPixmap>
#include <QMenu>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QMutex>
#include <QFile>
#include <QTextStream>
#include <functional>
#include <atomic>

class QTimer;
class QCloseEvent;
class QThread;

#include "devicedialog.h"
#include "edl/edl_service.h"
#include "edl/edl_error.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void appendLog(const QString &text, const QString &color = "#3A3F4B");
    void appendLogHtml(const QString &html);
    void appendLogDetail(const QString &text);
    void setProgress(int value);
    /** EDL 核心在工作线程调用：按「当前分区/当前操作」的 current/total 显示进度并节流投递 UI */
    void relayEdlProgressFromCore(qint64 current, qint64 total);
    void endBatchProgress();
    void setWallpaper(const QString &imagePath);
    /** Realme 云端签名（供 edl_realme_sign_cb；可在工作线程调用，内含同步网络） */
    bool performRealmeCloudSign(const edl_realme_sign_material_t *material,
                                uint8_t *signature_out, int *signature_len);
    /** EDL 核心 is_cancelled 回调（C 函数指针）；须可从外部调用 */
    bool isCancelRequested() const { return m_cancelRequested.load(std::memory_order_relaxed); }

protected:
    void closeEvent(QCloseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private slots:
    void onPortWatchTimeout();

private:
    void resetProgressTracking();
    void clearBatchProgressContext();
    void beginBatchProgress(qint64 totalBytes);
    void setBatchProgressWindow(qint64 baseBytes, qint64 spanBytes);
    void setupTitleBar();
    void setupConnections();
    void setupTransparency();
    void setupPartitionTable();
    void setupDisabledTabs();
    void setupEdlService();
    void setupPortWatchTimer();
    void wireGeneralButtons();
    void enqueueMiscVendorWipe(const QString &vendorSlug);
    void wirePartitionButtons();
    void loadSettings();
    void loadApiTheme(const QString &url);
    void fetchUrl(const QUrl &url,
                  std::function<void(const QByteArray &data)> onSuccess,
                  std::function<void(const QString &err)> onError);
    /** 启动后预热到 EDL Admin 的 TCP/TLS，加快后续机型列表等请求 */
    void warmupCloudApiConnection();
    static QString extractImageUrl(const QJsonDocument &doc);
    static QString extractImageUrlFromHtml(const QByteArray &html);
    void populateDemoLog();

    /** 每项操作独立工作线程；串行执行，完成后方可下一命令，并输出耗时与 SAKURAEDL 完成行 */
    void runAsync(std::function<void()> task, const QString &operationLabel = QString());
    void setBusy(bool busy);
    bool ensureConnected(bool syncStorageInfoOnConnect = false);
    /** NOP 检测 Firehose；失败则按端口是否存在记录原因并 disconnect。须在持有 m_edlIoMutex 的线程中调用。 */
    bool preflightFirehose(const QString &contextLabel);
    /** 若 core 无 GPT 缓存则读盘并刷新界面分区表；须在持有 m_edlIoMutex 的工作线程中调用。 */
    bool ensureGptCacheReady();
    void displayChipInfo();
    void populatePartitionTable(const edl_partition_info_t *parts, int count);
    void applyPartitionTableFilter();
    void logEdlResult(const QString &action, edl_error_t err);
    void appendSectionLog(const QString &title);
    void appendInfoLog(const QString &text);
    void appendSuccessLog(const QString &text);
    void appendWarningLog(const QString &text);
    void appendErrorLog(const QString &text);
    void appendMutedLog(const QString &text);
    /** 主线程：输出来自分区的 EXT4/EROFS build.prop 解析结果 */
    void appendAndroidBuildPropsLog(const edl_android_props_t &ap);
    /** 主线程：根据 edl_service 当前存储类型刷新「存储类型」下拉框（连接后 / GetStorageInfo 后） */
    void applyDeviceStorageToUi(bool logLine);
    /** 端口断开 / 会话清除后恢复与启动时一致的 UI（标题、分区表、进度） */
    void resetUiAfterDisconnect();
    /** 异步任务失败（工作线程可调用）：进度条收尾时显示 error 态并停在当前/最近百分比 */
    void markAsyncTaskFailed();
    /** 主线程：断开 EDL、清分区/进度、解除 busy；I/O 忙时自动延后重试，避免拔线后界面卡死 */
    void forceDisconnectAndResetUi(const QString &reason);
    /** 云端选中机型后：下载 Firehose/认证文件到本机并同步认证选项 */
    void applyCloudDeviceEntry(const DeviceEntry &dev);
    /** 若设置中填写了 EDL Admin 地址，启动后检查 /api/v1/update-info */
    void checkCloudUpdateIfConfigured();

    /** 关闭窗口前：停定时器、等 runAsync 线程结束（避免析构与持锁工作线程死锁） */
    void waitForAsyncWorkerAndStopWatch();

    Ui::MainWindow *ui;
    QPixmap m_wallpaper;
    QNetworkAccessManager *m_netManager;

    edl_service_t *m_edlService = nullptr;
    /** runAsync 创建的工作线程；关闭时必须 join/terminate，否则析构抢锁会死锁 */
    QThread *m_asyncWorkerThread = nullptr;
    bool m_busy = false;
    std::atomic<bool> m_cancelRequested{false};
    /** runAsync 任务是否已标记失败（logEdlResult / markAsyncTaskFailed） */
    std::atomic<bool> m_asyncFail{false};
    /** 最近一次进度条整数百分比（用于失败时停在出错进度） */
    std::atomic<int> m_lastProgressPctAtomic{0};
    std::atomic<bool> m_closeCleanupDone{false};

    std::atomic<qint64> m_progUiLastMs{0};
    /** 当前分区/当前操作的起始时间（ms epoch），换分区时重置 */
    std::atomic<qint64> m_progOpStartMs{0};
    /** 检测分区切换：上次回调的 current/total（-1 未开始） */
    std::atomic<qint64> m_progLastReportedCurrent{-1};
    std::atomic<qint64> m_progLastReportedTotal{-1};
    /** 瞬时速度采样锚点（两次 UI 刷新之间） */
    std::atomic<qint64> m_progSpeedPrevMs{0};
    std::atomic<qint64> m_progSpeedPrevCurrent{0};
    /** 单调百分比（同一分区内不倒退） */
    int m_progLastPctShown = 0;
    /** 最近一次有效的实时速率（MB/s，两帧间隔内 Δ字节/Δt）；无新样本时用于延续显示 */
    double m_progDisplayMbps = -1.0;
    std::atomic<bool> m_progressUsesByteSemantics{true};
    std::atomic<bool> m_batchProgressActive{false};
    std::atomic<qint64> m_batchProgressBaseBytes{0};
    std::atomic<qint64> m_batchProgressSpanBytes{0};
    std::atomic<qint64> m_batchProgressTotalBytes{0};

    bool m_maximized = false;
    QRect m_normalGeometry;
    bool m_titleDragging = false;
    QPoint m_dragPos;

    QMutex m_edlIoMutex;
    QTimer *m_portWatchTimer = nullptr;
    /** 当前已连接会话对应的 COM 名（供无锁检测「系统是否仍有该串口」） */
    QString m_trackedComPort;
    /** 与端口检测定时器配合：隔次再发 NOP，减轻负载 */
    int m_portWatchPingPhase = 0;

    QFile   m_logFile;
    QTextStream m_logStream;
    bool    m_logAutoSave = true;
    bool    m_logShowDetail = false;
    QString m_lastLogText;
    QString m_lastLogColor;
    qint64  m_lastLogMs = 0;
    QString m_lastDetailLogText;
    qint64  m_lastDetailLogMs = 0;
    void    openLogFile();
    void    writeLogLine(const QString &plainText);
};

#endif // MAINWINDOW_H
