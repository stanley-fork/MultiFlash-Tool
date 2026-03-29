#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "settingsdialog.h"
#include "edl_api_client.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QScrollBar>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QSvgRenderer>
#include <QLinearGradient>
#include <QScreen>
#include <QApplication>
#include <QHeaderView>
#include <QDesktopServices>
#include <QUrl>
#include <QStandardPaths>
#include <QDir>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QProcess>
#include <QTemporaryFile>
#include <QThread>
#include <QCloseEvent>
#include <QSvgWidget>
#include <QMessageBox>
#include <QTime>
#include <QDateTime>
#include <QColor>
#include <QTimer>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProgressDialog>
#include <QNetworkRequest>
#include <limits>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QCoreApplication>
#include <QAction>
#include <QLabel>
#include <QStyle>
#include <cstring>

#include "edl/port_detect.h"
#include "edl/rawprogram.h"
#include "edl/sparse.h"
#include "edl/chip_db.h"

namespace {
static QString formatSize(int64_t sectors, int sectorSize)
{
    double bytes = static_cast<double>(sectors) * sectorSize;
    if (bytes >= 1073741824.0)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1048576.0)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
    if (bytes >= 1024.0)
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
    return QString("%1 B").arg(static_cast<int64_t>(bytes));
}

/** 与 Sakura 刷机末尾 / slot_detect 一致：UFS Boot A/B→LUN1/2；eMMC 单可启动路径→0。 */
static int lunForManualBootSlot(const QString &storageKind, bool bootA)
{
    if (storageKind == QLatin1String("emmc"))
        return 0;
    return bootA ? 1 : 2;
}

/** 优先已探测的存储类型，否则按界面「自动/emmc/ufs」下拉推断。 */
static QString resolveStorageKindForBoot(const char *svcStorage, int comboIndex)
{
    QString s = QString::fromUtf8(svcStorage ? svcStorage : "").trimmed().toLower();
    if (!s.isEmpty() && s != QLatin1String("auto"))
        return s;
    if (comboIndex == 1)
        return QStringLiteral("emmc");
    if (comboIndex == 2)
        return QStringLiteral("ufs");
    return QStringLiteral("ufs");
}

/** 毫秒 → mm:ss 或 h:mm:ss（用于已用/剩余时间） */
static QString formatClockFromMs(qint64 ms)
{
    if (ms < 0)
        return QStringLiteral("--:--");
    qint64 totalSec = qMin<qint64>(ms / 1000, 86400 * 2 - 1);
    if (totalSec < 0)
        totalSec = 0;
    const int h = static_cast<int>(totalSec / 3600);
    const int m = static_cast<int>((totalSec % 3600) / 60);
    const int s = static_cast<int>(totalSec % 60);
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

/** 将 qrc 内 SVG 栅格化为 QIcon（不依赖 QSvg 图像插件；避免 QIcon(":/…/x.svg") 在部分环境下为空）。 */
static QIcon iconFromSvgResource(const QString &resourcePath, QSize logicalSize)
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid())
        return {};
    QPixmap pm(logicalSize);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    renderer.render(&p);
    p.end();
    return QIcon(pm);
}

static QString miscBundledCacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/bundled");
}

static QString miscVendorCacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/misc_vendor");
}

/** 从 qrc 释放到指定缓存目录（与嵌入资源字节一致则跳过重写） */
static bool extractQrcToDir(const QString &qrcPath, const QString &destFileName, const QString &cacheDir,
                            QString *outFullPath)
{
    QFile qf(qrcPath);
    if (!qf.exists())
        return false;
    if (!qf.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = qf.readAll();
    qf.close();
    if (data.isEmpty())
        return false;

    QDir().mkpath(cacheDir);
    const QString dest = cacheDir + QLatin1Char('/') + destFileName;

    if (QFile::exists(dest)) {
        const qint64 ds = QFileInfo(dest).size();
        if (ds == data.size()) {
            *outFullPath = dest;
            return true;
        }
    }

    QFile dst(dest);
    if (!dst.open(QIODevice::WriteOnly))
        return false;
    if (dst.write(data) != data.size()) {
        dst.close();
        dst.remove();
        return false;
    }
    dst.close();
    *outFullPath = dest;
    return true;
}

/** 从 qrc 释放到 AppData/bundled（与嵌入资源字节一致则跳过重写） */
static bool extractBundledIfPresent(const QString &qrcPath, const QString &destFileName, QString *outFullPath)
{
    return extractQrcToDir(qrcPath, destFileName, miscBundledCacheDir(), outFullPath);
}

/** exe 旁优先；否则 :/misc_vendor/ 嵌入并释放到 AppData/misc_vendor */
static QString resolveMiscVendorPackagedPath(const QString &fileBaseName)
{
    const QString appPath = QCoreApplication::applicationDirPath() + QLatin1Char('/') + fileBaseName;
    if (QFile::exists(appPath))
        return appPath;
    const QString qrc = QStringLiteral(":/misc_vendor/") + fileBaseName;
    if (!QFile::exists(qrc))
        return QString();
    QString out;
    if (extractQrcToDir(qrc, fileBaseName, miscVendorCacheDir(), &out))
        return out;
    return QString();
}

/** Fastbootd / Recovery：设置路径 → 程序目录默认名 → 旧版 .bin 名 → 嵌入资源释放 */
static QString resolveMiscBootImage(bool toFastbootd)
{
    QSettings s;
    const QString key = toFastbootd ? QStringLiteral("reboot/miscFastbootPath")
                                    : QStringLiteral("reboot/miscRecoveryPath");
    QString p = s.value(key).toString().trimmed();
    if (!p.isEmpty() && QFile::exists(p))
        return p;
    const QString dir = QCoreApplication::applicationDirPath();
    const QString defNew = toFastbootd ? (dir + QStringLiteral("/misc_tofastbootd.img"))
                                       : (dir + QStringLiteral("/misc_torecovery.img"));
    if (QFile::exists(defNew))
        return defNew;
    const QString defLegacy = toFastbootd ? (dir + QStringLiteral("/misc_fastboot.bin"))
                                          : (dir + QStringLiteral("/misc_recovery.bin"));
    if (QFile::exists(defLegacy))
        return defLegacy;

    QString embedded;
    const QString qrc = toFastbootd ? QStringLiteral(":/bundled/misc_tofastbootd.img")
                                    : QStringLiteral(":/bundled/misc_torecovery.img");
    const QString fname = toFastbootd ? QStringLiteral("misc_tofastbootd.img")
                                      : QStringLiteral("misc_torecovery.img");
    if (extractBundledIfPresent(qrc, fname, &embedded))
        return embedded;
    return QString();
}

static QString resolveDwebpExecutablePath()
{
    const QString appDwebp = QCoreApplication::applicationDirPath() + QStringLiteral("/dwebp.exe");
    if (QFile::exists(appDwebp))
        return appDwebp;
    QString out;
    if (extractBundledIfPresent(QStringLiteral(":/bundled/dwebp.exe"), QStringLiteral("dwebp.exe"), &out))
        return out;
    return QString();
}

/** 用于进度与「是否 0 字节跳过」：sparse 取展开长度，否则取文件大小 */
static qint64 inferImageLogicalBytes(const QString &path)
{
    QByteArray pb = path.toUtf8();
    if (edl_sparse_is_sparse(pb.constData())) {
        edl_sparse_reader_t *r = edl_sparse_open(pb.constData());
        if (r) {
            const int64_t t = edl_sparse_total_size(r);
            edl_sparse_close(r);
            return static_cast<qint64>(t);
        }
    }
    return QFileInfo(path).size();
}

/**
 * 0 字节镜像若跳过写入，可能导致 BootROM 无法链式加载 → 反复复进 EDL/9008。
 * 列表为启发式（含你日志里曾跳过的 cdt/sec）；非穷举。
 */
/** LUN 列：支持十进制与 0x 前缀（避免仅显示文本解析失败） */
static bool parseTableLunCell(const QString &cellText, int *outLun)
{
    if (!outLun)
        return false;
    const QString t = cellText.trimmed();
    if (t.isEmpty())
        return false;
    bool ok = false;
    int v = 0;
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        v = t.mid(2).toInt(&ok, 16);
    else
        v = t.toInt(&ok);
    if (!ok)
        return false;
    *outLun = v;
    return true;
}


/** 写入任务：GPT 无此名时可用 rawprogram 表格中的 LUN/起始扇区（如 PrimaryGPT） */
struct WritePartitionTask {
    QString name;
    QString path;
    qint64 tableStartSector = 0;
    qint64 tableNumSectors = 0;
    int tableLun = 0;
    int tableSectorSize = 4096;
    QString tableStartSectorExpr;
    bool hasTableFallback = false;
};

/** 读取任务：与写入相同，在 GPT 缓存与界面不同步时可用分区表行内 LUN/扇区回退 */
struct ReadPartitionTask {
    QString name;
    QString path;
    qint64 tableStartSector = 0;
    qint64 tableNumSectors = 0;
    int tableLun = 0;
    int tableSectorSize = 4096;
    QString tableStartSectorExpr;
    bool hasTableFallback = false;
};

enum PartitionTableRole {
    RoleStartSector = Qt::UserRole + 1,
    RoleNumSectors,
    RoleSectorSize,
    RoleLun,
    RoleStartSectorExpr
};

enum PartCol {
    ColName         = 0,
    ColStartSector  = 1,
    ColSectorCount  = 2,
    ColSize         = 3,
    ColLun          = 4,
    ColFile         = 5,
    ColCount        = 6
};

static void fillReadPartitionTaskFromRow(QTableWidget *table, int r, ReadPartitionTask &wt)
{
    wt.hasTableFallback = false;
    auto *nameItem = table->item(r, ColName);
    auto *s0 = table->item(r, ColStartSector);
    auto *s3 = table->item(r, ColLun);
    if (!nameItem || !s0 || !s3)
        return;
    bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
    const qint64 startSectorRole = nameItem->data(RoleStartSector).toLongLong(&ok1);
    const qint64 numSectorsRole = nameItem->data(RoleNumSectors).toLongLong(&ok2);
    const int sectorSizeRole = nameItem->data(RoleSectorSize).toInt(&ok3);
    const int lunRole = nameItem->data(RoleLun).toInt(&ok4);

    wt.tableStartSector = ok1 ? startSectorRole : s0->text().trimmed().toLongLong(&ok1);
    wt.tableNumSectors = ok2 ? numSectorsRole : 0;
    wt.tableSectorSize = ok3 && sectorSizeRole > 0 ? sectorSizeRole : 4096;
    int lunParsed = 0;
    const bool lunFromText = parseTableLunCell(s3->text(), &lunParsed);
    wt.tableLun = ok4 ? lunRole : (lunFromText ? lunParsed : 0);
    wt.tableStartSectorExpr = nameItem->data(RoleStartSectorExpr).toString().trimmed();
    wt.hasTableFallback = ok1 && (ok4 || lunFromText) && wt.tableNumSectors > 0;
}

static void fillPartitionFallback(edl_partition_info_t *part,
                                  const QString &name,
                                  int lun,
                                  qint64 startSector,
                                  qint64 numSectors,
                                  int sectorSize,
                                  const QString &startSectorExpr)
{
    if (!part)
        return;
    std::memset(part, 0, sizeof(*part));
    part->lun = lun;
    part->start_sector = startSector;
    part->num_sectors = numSectors;
    part->sector_size = sectorSize > 0 ? sectorSize : 4096;
    const QByteArray nameUtf8 = name.toUtf8();
    std::strncpy(part->name, nameUtf8.constData(), sizeof(part->name) - 1);
    part->name[sizeof(part->name) - 1] = '\0';
    if (!startSectorExpr.isEmpty()) {
        const QByteArray exprUtf8 = startSectorExpr.toUtf8();
        std::strncpy(part->start_sector_expr,
                     exprUtf8.constData(),
                     sizeof(part->start_sector_expr) - 1);
        part->start_sector_expr[sizeof(part->start_sector_expr) - 1] = '\0';
    }
}

static const edl_partition_info_t *resolveReadPartitionTask(edl_service_t *svc,
                                                            const ReadPartitionTask &task,
                                                            edl_partition_info_t *scratch,
                                                            bool *usedTableFallback)
{
    if (usedTableFallback)
        *usedTableFallback = false;
    if (!svc)
        return nullptr;

    const QByteArray nameUtf8 = task.name.toUtf8();
    const edl_partition_info_t *partPtr =
        edl_service_find_partition(svc, nameUtf8.constData());
    if (!partPtr && task.hasTableFallback && scratch) {
        fillPartitionFallback(scratch,
                              task.name,
                              task.tableLun,
                              task.tableStartSector,
                              task.tableNumSectors,
                              task.tableSectorSize,
                              task.tableStartSectorExpr);
        partPtr = scratch;
        if (usedTableFallback)
            *usedTableFallback = true;
    }
    return partPtr;
}

static const edl_partition_info_t *resolveWritePartitionTask(edl_service_t *svc,
                                                             const WritePartitionTask &task,
                                                             edl_partition_info_t *scratch)
{
    if (!svc)
        return nullptr;
    if (task.hasTableFallback && scratch) {
        fillPartitionFallback(scratch,
                              task.name,
                              task.tableLun,
                              task.tableStartSector,
                              task.tableNumSectors,
                              task.tableSectorSize,
                              task.tableStartSectorExpr);
        return scratch;
    }

    const QByteArray nameUtf8 = task.name.toUtf8();
    return edl_service_find_partition(svc, nameUtf8.constData());
}

static qint64 safePartitionLogicalBytes(const edl_partition_info_t *part, int fallbackSectorSize = 4096)
{
    if (!part || part->num_sectors <= 0)
        return 0;
    const qint64 sectorSize = part->sector_size > 0 ? part->sector_size : fallbackSectorSize;
    if (sectorSize <= 0)
        return 0;
    if (part->num_sectors > ((std::numeric_limits<qint64>::max)() / sectorSize))
        return 0;
    return part->num_sectors * sectorSize;
}

static qint64 alignUpToSectorBytes(qint64 bytes, qint64 sectorSize)
{
    if (bytes <= 0 || sectorSize <= 0)
        return 0;
    const qint64 rem = bytes % sectorSize;
    if (rem == 0)
        return bytes;
    const qint64 add = sectorSize - rem;
    if (bytes > ((std::numeric_limits<qint64>::max)() - add))
        return (std::numeric_limits<qint64>::max)();
    return bytes + add;
}

static qint64 estimateWriteProgressBytes(qint64 logicalBytes,
                                         const edl_partition_info_t *part,
                                         bool fileLengthOnly,
                                         int fallbackSectorSize = 4096)
{
    if (logicalBytes <= 0)
        return 0;
    const qint64 sectorSize = part && part->sector_size > 0
        ? part->sector_size
        : (fallbackSectorSize > 0 ? fallbackSectorSize : 4096);
    const qint64 alignedImageBytes = alignUpToSectorBytes(logicalBytes, sectorSize);
    const qint64 partitionBytes = safePartitionLogicalBytes(part, fallbackSectorSize);
    if (partitionBytes <= 0)
        return alignedImageBytes;
    if (!fileLengthOnly)
        return partitionBytes;
    return alignedImageBytes > partitionBytes ? partitionBytes : alignedImageBytes;
}
} // namespace

namespace {

/** 未在设置中保存过 cloud/edlBaseUrl 时，使用公网管理端默认地址（本地联调可在设置中改为 http://127.0.0.1:8088） */
QString resolvedCloudEdlBaseUrl()
{
    QSettings s;
    if (!s.contains(QStringLiteral("cloud/edlBaseUrl")))
        return EdlApi::defaultCloudEdlBaseUrl();
    return s.value(QStringLiteral("cloud/edlBaseUrl")).toString().trimmed();
}

} // namespace

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->progressBar->setProperty("state", "idle");
    ui->progressBar->style()->polish(ui->progressBar);

    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    /* 勿对主窗启用 WA_TranslucentBackground：在 Windows 下层叠窗口上，QSS 的 rgba alpha
     * 会与「桌面后的其它程序」混合，导致透过窗口看到 VS Code 等；仅去掉分层即可与窗内壁纸正常叠化。 */
    setMinimumSize(960, 580);
    resize(1120, 680);

    m_netManager = new QNetworkAccessManager(this);
    /* HTTPS/CDN 常见 301/302；Qt 默认 Manual 不跟随，会导致下载失败或拿到空内容 */
    m_netManager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    setupPartitionTable();
    setupTransparency();
    setupTitleBar();
    setupConnections();
    setupDisabledTabs();
    /* 先于 EDL 回调，确保 m_logShowDetail 等已加载，避免「详细日志」关时仍落盘 [detail] */
    loadSettings();
    setupEdlService();
    setupPortWatchTimer();
    wireGeneralButtons();
    wirePartitionButtons();
    populateDemoLog();

    QTimer::singleShot(150, this, &MainWindow::warmupCloudApiConnection);
    QTimer::singleShot(2000, this, &MainWindow::checkCloudUpdateIfConfigured);
}

MainWindow::~MainWindow()
{
    /* 若未走 closeEvent（极少见），仍须等 runAsync 线程结束，否则持锁线程与析构抢 m_edlIoMutex 会死锁 */
    if (!m_closeCleanupDone.exchange(true))
        waitForAsyncWorkerAndStopWatch();
    {
        QMutexLocker lock(&m_edlIoMutex);
        if (m_edlService) {
            edl_service_destroy(m_edlService);
            m_edlService = nullptr;
        }
    }
    delete ui;
}

void MainWindow::waitForAsyncWorkerAndStopWatch()
{
    if (m_portWatchTimer)
        m_portWatchTimer->stop();
    m_cancelRequested.store(true, std::memory_order_relaxed);
    QThread *t = m_asyncWorkerThread;
    if (t && t->isRunning()) {
        /* 最长 2 分钟等待正常结束；仍阻塞则 terminate（避免关窗后进程残留） */
        if (!t->wait(120000)) {
            t->terminate();
            (void)t->wait(1000);
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!m_closeCleanupDone.exchange(true))
        waitForAsyncWorkerAndStopWatch();
    e->accept();
    QMainWindow::closeEvent(e);
}

void MainWindow::setupTransparency()
{
    auto makeTransparent = [](QWidget *w) {
        if (!w) return;
        w->setAttribute(Qt::WA_NoSystemBackground, false);
        w->setAttribute(Qt::WA_TranslucentBackground, false);
        w->setAutoFillBackground(false);
    };
    /* 毛玻璃：仅用 QSS rgba 与下方 paintEvent 壁纸叠化（非分层窗口，不透到桌面其它应用） */
    auto makeFrosted = [](QWidget *w) {
        if (!w) return;
        w->setAttribute(Qt::WA_NoSystemBackground, false);
        w->setAttribute(Qt::WA_TranslucentBackground, false);
        w->setAttribute(Qt::WA_StyledBackground, true);
        w->setAutoFillBackground(true);
    };

    makeTransparent(ui->centralwidget);
    makeTransparent(ui->contentSplitter);
    makeFrosted(ui->leftPanel);
    makeTransparent(ui->leftInnerSplitter);
    makeFrosted(ui->leftConfigWidget);
    makeFrosted(ui->rightPanel);
    makeFrosted(ui->funcTabWidget);
    for (auto *tab : {ui->generalTab, ui->partitionTab, ui->imeiTab, ui->firmwareTab})
        makeFrosted(tab);
    makeFrosted(ui->partitionTable);
    makeFrosted(ui->logConsole);
}

void MainWindow::applyPartitionTableFilter()
{
    auto *table = ui->partitionTable;
    const QString q = ui->partitionSearchEdit->text().trimmed();
    for (int r = 0; r < table->rowCount(); ++r) {
        auto *nameItem = table->item(r, ColName);
        const QString name = nameItem ? nameItem->text() : QString();
        const bool show = q.isEmpty() || name.contains(q, Qt::CaseInsensitive);
        table->setRowHidden(r, !show);
    }
}

void MainWindow::setupPartitionTable()
{
    auto *table = ui->partitionTable;
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(ColName,        QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(ColStartSector, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ColSectorCount, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ColSize,        QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ColLun,         QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ColFile,        QHeaderView::Stretch);
    table->verticalHeader()->setDefaultSectionSize(24);
    table->verticalHeader()->setVisible(false);
    table->setShowGrid(true);

#if 0
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || row >= ui->partitionTable->rowCount()) return;
        QString partName = ui->partitionTable->item(row, ColName)->text();
        QString path = QFileDialog::getOpenFileName(
            this, QString("选择 %1 的镜像文件").arg(partName), "",
            "镜像文件 (*.bin *.img *.mbn *.elf);;所有文件 (*.*)");
        if (path.isEmpty()) return;
        QFileInfo fi(path);
        auto *item = ui->partitionTable->item(row, ColFile);
        if (!item) {
            item = new QTableWidgetItem();
            ui->partitionTable->setItem(row, ColFile, item);
        }
        item->setText(fi.fileName());
        item->setToolTip(path);
        item->setData(Qt::UserRole, path);
        int sec = 4096;
        if (m_edlService && edl_service_is_connected(m_edlService)) {
            const int ds = edl_service_sector_size(m_edlService);
            if (ds > 0)
                sec = ds;
        }
        edl_flash_task_t infer = {};
        infer.type = EDL_TASK_PROGRAM;
        infer.sector_size = sec;
        {
            const QByteArray pb = path.toUtf8();
            std::strncpy(infer.filepath, pb.constData(), sizeof(infer.filepath) - 1);
            infer.filepath[sizeof(infer.filepath) - 1] = '\0';
        }
        edl_flash_task_infer_sectors_from_image(&infer);
        if (infer.num_sectors > 0) {
            auto *szItem = ui->partitionTable->item(row, ColSize);
            if (!szItem) {
                szItem = new QTableWidgetItem();
                ui->partitionTable->setItem(row, ColSize, szItem);
            } /*
            szItem->setText(formatSize(infer.num_sectors, sec));
        }
        appendLog(QString("%1 ← %2").arg(partName, fi.fileName()), "#6B7280");
        */ }, QStringLiteral("Read GPT"));
    });

    });
#endif
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || row >= ui->partitionTable->rowCount())
            return;
        QString partName = ui->partitionTable->item(row, ColName)->text();
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("选择 %1 的镜像").arg(partName),
            QString(),
            QStringLiteral("镜像文件 (*.bin *.img *.mbn *.elf);;所有文件 (*.*)"));
        if (path.isEmpty())
            return;
        QFileInfo fi(path);
        auto *item = ui->partitionTable->item(row, ColFile);
        if (!item) {
            item = new QTableWidgetItem();
            ui->partitionTable->setItem(row, ColFile, item);
        }
        item->setText(fi.fileName());
        item->setToolTip(path);
        item->setData(Qt::UserRole, path);

        int sec = 4096;
        if (m_edlService && edl_service_is_connected(m_edlService)) {
            const int ds = edl_service_sector_size(m_edlService);
            if (ds > 0)
                sec = ds;
        }

        edl_flash_task_t infer = {};
        infer.type = EDL_TASK_PROGRAM;
        infer.sector_size = sec;
        {
            const QByteArray pb = path.toUtf8();
            std::strncpy(infer.filepath, pb.constData(), sizeof(infer.filepath) - 1);
            infer.filepath[sizeof(infer.filepath) - 1] = '\0';
        }
        edl_flash_task_infer_sectors_from_image(&infer);
        if (infer.num_sectors > 0) {
            auto *szItem = ui->partitionTable->item(row, ColSize);
            if (!szItem) {
                szItem = new QTableWidgetItem();
                ui->partitionTable->setItem(row, ColSize, szItem);
            }
            szItem->setText(formatSize(infer.num_sectors, sec));
        }
        appendLog(QStringLiteral("%1 -> %2").arg(partName, fi.fileName()), "#6B7280");
    });

    connect(ui->partitionSearchEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        applyPartitionTableFilter();
    });
    connect(ui->partitionSelectAllBtn, &QPushButton::clicked, this, [this]() {
        auto *table = ui->partitionTable;
        for (int r = 0; r < table->rowCount(); ++r) {
            if (table->isRowHidden(r)) continue;
            auto *n = table->item(r, ColName);
            if (n)
                n->setCheckState(Qt::Checked);
        }
    });
    connect(ui->partitionDeselectAllBtn, &QPushButton::clicked, this, [this]() {
        auto *table = ui->partitionTable;
        for (int r = 0; r < table->rowCount(); ++r) {
            if (table->isRowHidden(r)) continue;
            auto *n = table->item(r, ColName);
            if (n)
                n->setCheckState(Qt::Unchecked);
        }
    });
    ui->logConsole->setTabStopDistance(28);
    ui->logConsole->document()->setMaximumBlockCount(8000);

    /* Windows 上默认 autoFillBackground 会用不透明调色板盖住 QSS 的 rgba，导致日志/表格/输入框看起来「死白」 */
    auto glassNoPaletteFill = [](QWidget *w) {
        if (w)
            w->setAutoFillBackground(false);
    };
    glassNoPaletteFill(ui->logConsole);
    glassNoPaletteFill(ui->logConsole->viewport());
    glassNoPaletteFill(ui->partitionTable);
    glassNoPaletteFill(ui->partitionTable->viewport());
    glassNoPaletteFill(ui->partitionTable->horizontalHeader());
    glassNoPaletteFill(ui->partitionTable->verticalHeader());
    glassNoPaletteFill(ui->leftConfigWidget);
    glassNoPaletteFill(ui->rightPanel);
    glassNoPaletteFill(ui->partitionSearchEdit);
    glassNoPaletteFill(ui->storageCombo);
    if (ui->leftConfigWidget) {
        for (QWidget *w : ui->leftConfigWidget->findChildren<QWidget *>())
            glassNoPaletteFill(w);
    }
    const QList<QPushButton *> glassPush = {
        ui->firehoseBrowseBtn,   ui->rawprogramBrowseBtn, ui->patchBrowseBtn,
        ui->digestBrowseBtn,     ui->signatureBrowseBtn,  ui->partitionSelectAllBtn,
        ui->partitionDeselectAllBtn, ui->readPartBtn,     ui->writePartBtn,
        ui->erasePartBtn,        ui->readInfoBtn,         ui->readStorageDeviceInfoBtn,
        ui->readGptBtn,          ui->writeGptXmlBtn,      ui->removeFrpBtn,
        ui->factoryResetBtn,     ui->rebootBtn,           ui->activateBootSlotBtn,
        ui->cancelButton,
    };
    for (QPushButton *b : glassPush)
        glassNoPaletteFill(b);
}

/* populateDemoPartitions removed — table populated from real GPT data */

void MainWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    if (!m_wallpaper.isNull()) {
        QPixmap scaled = m_wallpaper.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        p.drawPixmap(x, y, scaled);
    } else {
        /* 无壁纸时铺实底，避免未分层时客户区发灰/闪 */
        p.fillRect(rect(), QColor(0xEE, 0xF0, 0xF4));
    }
}

void MainWindow::setWallpaper(const QString &imagePath)
{
    if (imagePath.isEmpty()) {
        m_wallpaper = QPixmap();
    } else {
        m_wallpaper.load(imagePath);
    }
    update();
}

void MainWindow::loadSettings()
{
    QSettings s;

    m_logAutoSave   = s.value(QStringLiteral("log/autoSave"), true).toBool();
    m_logShowDetail = s.value(QStringLiteral("log/showDetail"), false).toBool();

    ui->readSysInfoCheckBox->setChecked(
        s.value(QStringLiteral("ui/readDeviceInfo"), true).toBool());

    ui->realmeProjectNumberLineEdit->setText(
        s.value(QStringLiteral("realme/projectNumber")).toString());

    ui->writeFileLengthOnlyCheckBox->setChecked(
        s.value(QStringLiteral("flash/writeFileLengthOnly"), true).toBool());

    bool useApi = s.value("theme/useApi", false).toBool();
    if (useApi) {
        QString apiUrl = s.value("theme/apiUrl").toString();
        if (!apiUrl.isEmpty())
            loadApiTheme(apiUrl);
    } else {
        QString localPath = s.value("theme/localPath").toString();
        if (!localPath.isEmpty())
            setWallpaper(localPath);
    }
}

QString MainWindow::extractImageUrl(const QJsonDocument &doc)
{
    // Recursively search JSON for a value that looks like an image URL
    static const QStringList imgKeys = {
        "url", "imgurl", "img", "image", "src", "href",
        "imageurl", "image_url", "img_url", "pic", "picture",
        "wallpaper", "background", "bg", "file", "path", "link",
        "thumb", "thumbnail", "original", "large", "raw"
    };
    static const QStringList imgExts = {
        ".jpg", ".jpeg", ".png", ".bmp", ".webp", ".gif"
    };

    auto looksLikeImageUrl = [](const QString &s) -> bool {
        if (!s.startsWith("http", Qt::CaseInsensitive)) return false;
        QString lower = s.toLower();
        for (const auto &ext : imgExts)
            if (lower.contains(ext)) return true;
        if (lower.contains("/image") || lower.contains("/photo") || lower.contains("/pic"))
            return true;
        return false;
    };

    std::function<QString(const QJsonValue &)> search = [&](const QJsonValue &val) -> QString {
        if (val.isString()) {
            if (looksLikeImageUrl(val.toString()))
                return val.toString();
        } else if (val.isObject()) {
            QJsonObject obj = val.toObject();
            // First pass: check known keys
            for (const auto &key : imgKeys) {
                if (obj.contains(key)) {
                    QString v = obj.value(key).toString();
                    if (!v.isEmpty() && v.startsWith("http"))
                        return v;
                }
            }
            // Second pass: recurse all values
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                QString r = search(it.value());
                if (!r.isEmpty()) return r;
            }
        } else if (val.isArray()) {
            for (const auto &item : val.toArray()) {
                QString r = search(item);
                if (!r.isEmpty()) return r;
            }
        }
        return {};
    };

    if (doc.isObject())
        return search(QJsonValue(doc.object()));
    if (doc.isArray())
        return search(QJsonValue(doc.array()));
    return {};
}

void MainWindow::fetchUrl(const QUrl &url,
                          std::function<void(const QByteArray &)> onSuccess,
                          std::function<void(const QString &)> onError)
{
    QNetworkRequest req{url};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) SAKURAEDL/%1")
                      .arg(QCoreApplication::applicationVersion()));
    /* JSON/HTML/图片 API 与 CDN 常见 301/302：由 Qt 自动跟随，避免手工递归多一次 RTT */
    req.setRawHeader("Accept",
                     "application/json;q=0.95, text/html;q=0.9, "
                     "image/png,image/jpeg,image/webp,image/gif,image/bmp,image/*;q=0.85, */*;q=0.5");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    req.setTransferTimeout(60000);
    QNetworkReply *reply = m_netManager->get(req);

    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            onError(reply->errorString());
            return;
        }

        onSuccess(reply->readAll());
    });
}

QString MainWindow::extractImageUrlFromHtml(const QByteArray &html)
{
    // Look for og:image, <img src=...>, or meta refresh url
    QString s = QString::fromUtf8(html);
    static QRegularExpression ogImage(R"(content=[\"']([^\"']+\.(jpg|jpeg|png|webp|gif|bmp)[^\"']*)[\"'])",
                                       QRegularExpression::CaseInsensitiveOption);
    auto m = ogImage.match(s);
    if (m.hasMatch()) return m.captured(1);

    static QRegularExpression imgSrc(R"(<img[^>]+src=[\"']([^\"']+\.(jpg|jpeg|png|webp|gif|bmp)[^\"']*)[\"'])",
                                      QRegularExpression::CaseInsensitiveOption);
    m = imgSrc.match(s);
    if (m.hasMatch()) return m.captured(1);

    return {};
}

static bool isWebP(const QByteArray &data) {
    return data.size() > 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP";
}

static QPixmap decodeWebP(const QByteArray &webpData) {
    const QString dwebp = resolveDwebpExecutablePath();
    if (dwebp.isEmpty())
        return {};

    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString inPath  = tmpDir + "/edl_theme_tmp.webp";
    QString outPath = tmpDir + "/edl_theme_tmp.png";

    QFile in(inPath);
    if (!in.open(QIODevice::WriteOnly))
        return {};
    in.write(webpData);
    in.close();

    QProcess proc;
    proc.setProgram(dwebp);
    proc.setArguments({inPath, "-o", outPath, "-quiet"});
    proc.start();
    proc.waitForFinished(10000);

    QPixmap pix;
    if (proc.exitCode() == 0)
        pix.load(outPath);

    QFile::remove(inPath);
    QFile::remove(outPath);
    return pix;
}

void MainWindow::loadApiTheme(const QString &url)
{
    auto applyImage = [this](const QByteArray &data) {
        QPixmap pix;
        if (pix.loadFromData(data)) {
            m_wallpaper = pix;
            update();
            appendLog(QStringLiteral("API 主题加载成功"), QStringLiteral("#2E8B3E"));
        } else if (isWebP(data)) {
            pix = decodeWebP(data);
            if (!pix.isNull()) {
                m_wallpaper = pix;
                update();
                appendLog(QStringLiteral("API 主题加载成功"), QStringLiteral("#2E8B3E"));
            } else {
                appendLog(QStringLiteral("API 主题加载失败：WebP 解码失败（请放置 dwebp.exe 或嵌入 bundled）"),
                          QStringLiteral("#C0392B"));
            }
        } else {
            appendLog(QStringLiteral("API 主题加载失败：图片数据无法解析"), QStringLiteral("#C0392B"));
        }
    };

    auto onError = [this](const QString &err) {
        appendLog("API 主题加载失败: " + err, "#C0392B");
    };

    fetchUrl(QUrl(url),
        [this, applyImage, onError](const QByteArray &data) {

            // 1) Directly loadable image (JPEG/PNG/BMP/GIF)
            QPixmap pix;
            if (pix.loadFromData(data)) {
                m_wallpaper = pix;
                update();
                appendLog(QStringLiteral("API 主题加载成功"), QStringLiteral("#2E8B3E"));
                return;
            }

            // 2) WebP → decode via dwebp.exe
            if (isWebP(data)) {
                QPixmap wp = decodeWebP(data);
                if (!wp.isNull()) {
                    m_wallpaper = wp;
                    update();
                    appendLog(QStringLiteral("API 主题加载成功"), QStringLiteral("#2E8B3E"));
                } else {
                    appendLog(QStringLiteral("API 主题加载失败：WebP 解码失败（请放置 dwebp.exe 或嵌入 bundled）"),
                              QStringLiteral("#C0392B"));
                }
                return;
            }

            // 3) JSON containing image URL
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (!doc.isNull()) {
                QString imgUrl = extractImageUrl(doc);
                if (!imgUrl.isEmpty()) {
                    fetchUrl(QUrl(imgUrl), applyImage, onError);
                    return;
                }
            }

            // 4) HTML containing image URL
            QString htmlImg = extractImageUrlFromHtml(data);
            if (!htmlImg.isEmpty()) {
                fetchUrl(QUrl(htmlImg), applyImage, onError);
                return;
            }

            appendLog(QStringLiteral("API 主题加载失败：返回数据无法识别"),
                      QStringLiteral("#C0392B"));
        },
        onError);
}

void MainWindow::setupTitleBar()
{
    {
        const QSize menuIconSize(20, 20);
        QIcon menuIcon = iconFromSvgResource(QStringLiteral(":/icons/icons/select_device.svg"), menuIconSize);
        if (menuIcon.isNull())
            menuIcon = iconFromSvgResource(QStringLiteral(":/icons/icons/menu.svg"), menuIconSize);
        ui->menuButton->setIcon(menuIcon);
    }
    ui->settingsButton->setIcon(QIcon(":/icons/icons/settings.svg"));
    ui->wallpaperButton->setIcon(QIcon(":/icons/icons/wallpaper.svg"));
    ui->logButton->setIcon(QIcon(":/icons/icons/log.svg"));
    ui->minimizeButton->setIcon(QIcon(":/icons/icons/minimize.svg"));
    ui->maximizeButton->setIcon(QIcon(":/icons/icons/maximize.svg"));
    ui->closeButton->setIcon(QIcon(":/icons/icons/close.svg"));

    ui->menuButton->setCursor(Qt::PointingHandCursor);
    ui->menuButton->setToolTip(tr("选择机型"));
    ui->settingsButton->setCursor(Qt::PointingHandCursor);
    ui->wallpaperButton->setCursor(Qt::PointingHandCursor);
    ui->logButton->setCursor(Qt::PointingHandCursor);
    ui->minimizeButton->setCursor(Qt::PointingHandCursor);
    ui->maximizeButton->setCursor(Qt::PointingHandCursor);
    ui->closeButton->setCursor(Qt::PointingHandCursor);
    ui->cancelButton->setCursor(Qt::PointingHandCursor);
    ui->cancelButton->setEnabled(false);

    #if 0
    connect(ui->cancelButton, &QPushButton::clicked, this, [this]() {
        if (!m_busy)
            return;
        m_cancelRequested.store(true, std::memory_order_relaxed);
        ui->cancelButton->setEnabled(false);
        ui->cancelButton->setText(QStringLiteral("取消中…"));
        appendLog("已请求取消，正在等待当前传输块结束…", "#7D6B3A");
    });

    #endif
    connect(ui->cancelButton, &QPushButton::clicked, this, [this]() {
        if (!m_busy)
            return;
        m_cancelRequested.store(true, std::memory_order_relaxed);
        ui->cancelButton->setEnabled(false);
        ui->cancelButton->setText(QStringLiteral("取消中…"));
        appendLog(QStringLiteral("已请求取消，正在等待当前传输块或当前请求结束…"), "#7D6B3A");
    });

    connect(ui->minimizeButton, &QPushButton::clicked, this, &QMainWindow::showMinimized);
    connect(ui->maximizeButton, &QPushButton::clicked, this, [this]() {
        if (m_maximized) {
            setGeometry(m_normalGeometry);
            m_maximized = false;
        } else {
            m_normalGeometry = geometry();
            QScreen *scr = screen();
            if (scr)
                setGeometry(scr->availableGeometry());
            m_maximized = true;
        }
    });
    connect(ui->closeButton, &QPushButton::clicked, this, &QMainWindow::close);

    // Menu button → Device selection
    connect(ui->menuButton, &QPushButton::clicked, this, [this]() {
        QSettings s;
        DeviceDialog dlg(this);
        dlg.setNetworkAccessManager(m_netManager);
        dlg.setCloudBaseUrl(resolvedCloudEdlBaseUrl());
        connect(&dlg, &DeviceDialog::deviceSelected, this, [this](const DeviceEntry &dev) {
            ui->titleLabel->setText(
                QStringLiteral("SAKURAEDL %1 | %2 : %3 (%4)")
                    .arg(QCoreApplication::applicationVersion(), dev.chipset, dev.device, dev.brand));
            applyCloudDeviceEntry(dev);
        });
        dlg.exec();
    });

    // Settings button → Settings dialog (load current values, save on accept)
    connect(ui->settingsButton, &QPushButton::clicked, this, [this]() {
        QSettings s;
        SettingsDialog dlg(this);
        dlg.setApiThemeMode(s.value("theme/useApi", false).toBool());
        dlg.setApiUrl(s.value("theme/apiUrl").toString());
        dlg.setLocalWallpaperPath(s.value("theme/localPath").toString());
        dlg.setAutoSaveLog(s.value("log/autoSave", true).toBool());
        dlg.setLogSavePath(s.value("log/savePath").toString());
        dlg.setShowTimestamp(s.value("log/timestamp", true).toBool());
        dlg.setAutoDetectDevice(s.value("general/autoDetect", true).toBool());
        dlg.setConfirmBeforeAction(s.value("general/confirm", true).toBool());
        dlg.setShowDetailLog(s.value("log/showDetail", false).toBool());
        dlg.setRealmeApiUrl(s.value("realme/signApiUrl").toString());
        dlg.setRealmeRcsmAccount(s.value("realme/rcsmAuthAccount").toString());
        dlg.setRealmeRcsmKey(s.value("realme/rcsmAuthKey").toString());
        dlg.setMiscFastbootImagePath(s.value("reboot/miscFastbootPath").toString());
        dlg.setMiscRecoveryImagePath(s.value("reboot/miscRecoveryPath").toString());
        dlg.setCloudEdlBaseUrl(resolvedCloudEdlBaseUrl());

        connect(&dlg, &SettingsDialog::themeChanged, this, [&dlg, this]() {
            QSettings ws;
            ws.setValue("theme/useApi", dlg.isApiTheme());
            ws.setValue("theme/apiUrl", dlg.apiUrl());
            ws.setValue("theme/localPath", dlg.localWallpaperPath());
            ws.setValue("log/autoSave", dlg.autoSaveLog());
            ws.setValue("log/savePath", dlg.logSavePath());
            ws.setValue("log/timestamp", dlg.showTimestamp());
            ws.setValue("log/showDetail", dlg.showDetailLog());
            ws.setValue("general/autoDetect", dlg.autoDetectDevice());
            ws.setValue("general/confirm", dlg.confirmBeforeAction());

            ws.setValue("realme/signApiUrl", dlg.realmeApiUrl());
            ws.setValue("realme/rcsmAuthAccount", dlg.realmeRcsmAccount());
            ws.setValue("realme/rcsmAuthKey", dlg.realmeRcsmKey());
            ws.setValue("reboot/miscFastbootPath", dlg.miscFastbootImagePath());
            ws.setValue("reboot/miscRecoveryPath", dlg.miscRecoveryImagePath());
            ws.setValue(QStringLiteral("cloud/edlBaseUrl"), dlg.cloudEdlBaseUrl());

            m_logAutoSave = dlg.autoSaveLog();
            m_logShowDetail = dlg.showDetailLog();

            if (dlg.isApiTheme()) {
                m_wallpaper = QPixmap();
                update();
                if (!dlg.apiUrl().isEmpty())
                    loadApiTheme(dlg.apiUrl());
            } else {
                setWallpaper(dlg.localWallpaperPath());
            }
        });
        dlg.exec();
    });

    // 标题栏：按设置中的 API 地址重新拉取主题图（与「设置」里 API 主题一致）
    connect(ui->wallpaperButton, &QPushButton::clicked, this, [this]() {
        QSettings s;
        const QString url = s.value("theme/apiUrl").toString().trimmed();
        if (url.isEmpty()) {
            QMessageBox::information(this, "主题",
                "请先在「设置」中填写 API 主题地址并保存，再点此按钮刷新背景图。");
            return;
        }
        s.setValue("theme/useApi", true);
        loadApiTheme(url);
    });

    connect(ui->logButton, &QPushButton::clicked, this, [this]() {
        QSettings s;
        QString dir = s.value(QStringLiteral("log/savePath")).toString().trimmed();
        if (dir.isEmpty())
            dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    // General tab icons
    ui->readInfoBtn->setIcon(QIcon(":/icons/icons/read_info.svg"));
    ui->readStorageDeviceInfoBtn->setIcon(QIcon(":/icons/icons/read_storage_device.svg"));
    ui->readGptBtn->setIcon(QIcon(":/icons/icons/read_gpt.svg"));
    ui->writeGptXmlBtn->setIcon(QIcon(":/icons/icons/write_gpt_xml.svg"));
    ui->removeFrpBtn->setIcon(QIcon(":/icons/icons/remove_frp.svg"));
    ui->factoryResetBtn->setIcon(QIcon(QStringLiteral(":/icons/icons/factory_userdata.svg")));
    ui->rebootBtn->setIcon(QIcon(":/icons/icons/reboot.svg"));
    ui->activateBootSlotBtn->setIcon(QIcon(QStringLiteral(":/icons/icons/partition.svg")));

    const QIcon browseIcon(QStringLiteral(":/icons/icons/browse.svg"));
    for (QPushButton *browseBtn : {ui->firehoseBrowseBtn, ui->rawprogramBrowseBtn, ui->patchBrowseBtn,
                                   ui->digestBrowseBtn, ui->signatureBrowseBtn}) {
        browseBtn->setIcon(browseIcon);
        browseBtn->setText(QString());
        browseBtn->setToolTip(QStringLiteral("浏览…"));
    }

    ui->partitionSelectAllBtn->setIcon(QIcon(":/icons/icons/select_all.svg"));
    ui->partitionDeselectAllBtn->setIcon(QIcon(":/icons/icons/deselect_all.svg"));
    ui->cancelButton->setIcon(QIcon(":/icons/icons/close.svg"));

    // 重启菜单：QAction 在 mainwindow.ui 中定义（文本+SVG 图标），此处组装 QMenu 并设置 data（供 setupConnections）
    auto *rebootMenu = new QMenu(this);
    rebootMenu->setObjectName(QStringLiteral("rebootMenu"));
    rebootMenu->addAction(ui->actionRebootSys);
    rebootMenu->addSeparator();
    rebootMenu->addAction(ui->actionRebootFastboot);
    rebootMenu->addAction(ui->actionRebootRecovery);
    rebootMenu->addSeparator();
    rebootMenu->addAction(ui->actionRebootEdl);
    rebootMenu->addSeparator();
    rebootMenu->addAction(ui->actionRebootOff);
    ui->rebootBtn->setMenu(rebootMenu);

    ui->actionRebootSys->setData(QStringLiteral("reset"));
    ui->actionRebootFastboot->setData(QStringLiteral("misc_fastbootd"));
    ui->actionRebootRecovery->setData(QStringLiteral("misc_recovery"));
    ui->actionRebootEdl->setData(QStringLiteral("edl_download"));
    ui->actionRebootOff->setData(QStringLiteral("off"));

    /* 恢复出厂：与「重启设备」相同，主按钮 + 下拉选厂商 */
    auto *factoryMenu = new QMenu(this);
    factoryMenu->setObjectName(QStringLiteral("factoryResetMenu"));
    static const char *const kFacSlugs[] = {
        "common1",   "huawei_lowlevel", "lenovo", "lg",       "meizu",
        "meizu2",    "mi",              "oneplus", "oppo",    "zte",
    };
    const QString kFacTitles[] = {
        QStringLiteral("通用"), QStringLiteral("华为"), QStringLiteral("联想"), QStringLiteral("LG"),
        QStringLiteral("魅族"), QStringLiteral("魅族②"), QStringLiteral("小米"), QStringLiteral("一加"),
        QStringLiteral("OPPO"), QStringLiteral("中兴"),
    };
    constexpr int kFacN = int(sizeof(kFacSlugs) / sizeof(kFacSlugs[0]));
    for (int i = 0; i < kFacN; ++i) {
        QAction *a = factoryMenu->addAction(kFacTitles[i]);
        a->setData(QString::fromLatin1(kFacSlugs[i]));
        a->setToolTip(tr("写入 misc_wipedata_%1.img 并重启").arg(QString::fromLatin1(kFacSlugs[i])));
    }
    ui->factoryResetBtn->setMenu(factoryMenu);
    connect(factoryMenu, &QMenu::triggered, this, [this](QAction *a) {
        const QString slug = a->data().toString();
        if (!slug.isEmpty())
            enqueueMiscVendorWipe(slug);
    });

    /* 激活启动分区：与「重启设备」相同，主按钮 + 下拉菜单（QAction 在 .ui 中带图标） */
    auto *activateBootSlotMenu = new QMenu(this);
    activateBootSlotMenu->setObjectName(QStringLiteral("activateBootMenu"));
    activateBootSlotMenu->addAction(ui->actionActivateBootA);
    activateBootSlotMenu->addAction(ui->actionActivateBootB);
    ui->activateBootSlotBtn->setMenu(activateBootSlotMenu);

    // Partition tab bottom buttons
    ui->readPartBtn->setIcon(QIcon(":/icons/icons/read_part.svg"));
    ui->writePartBtn->setIcon(QIcon(":/icons/icons/write_part.svg"));
    ui->erasePartBtn->setIcon(QIcon(":/icons/icons/erase_part.svg"));

    auto setCursorForAll = [](QPushButton *btn) {
        btn->setCursor(Qt::PointingHandCursor);
    };
    setCursorForAll(ui->readInfoBtn);
    setCursorForAll(ui->readStorageDeviceInfoBtn);
    setCursorForAll(ui->readGptBtn);
    setCursorForAll(ui->writeGptXmlBtn);
    setCursorForAll(ui->removeFrpBtn);
    setCursorForAll(ui->factoryResetBtn);
    setCursorForAll(ui->rebootBtn);
    setCursorForAll(ui->activateBootSlotBtn);
    setCursorForAll(ui->readPartBtn);
    setCursorForAll(ui->writePartBtn);
    setCursorForAll(ui->erasePartBtn);
    setCursorForAll(ui->partitionSelectAllBtn);
    setCursorForAll(ui->partitionDeselectAllBtn);
    setCursorForAll(ui->firehoseBrowseBtn);
    setCursorForAll(ui->rawprogramBrowseBtn);
    setCursorForAll(ui->patchBrowseBtn);
    setCursorForAll(ui->digestBrowseBtn);
    setCursorForAll(ui->signatureBrowseBtn);
    setCursorForAll(ui->cancelButton);

    /* 略增左侧宽度便于日志阅读；右侧分区表仍占主要空间 */
    ui->contentSplitter->setStretchFactor(0, 5);
    ui->contentSplitter->setStretchFactor(1, 6);
    ui->leftInnerSplitter->setStretchFactor(0, 0);
    ui->leftInnerSplitter->setStretchFactor(1, 1);
    ui->leftInnerSplitter->setSizes({210, 520});
}

void MainWindow::setupConnections()
{
    auto browseFn = [this](QLineEdit *edit, const QString &filter) {
        QString path = QFileDialog::getOpenFileName(this, "选择文件", "", filter);
        if (!path.isEmpty()) edit->setText(path);
    };

    auto logSelectedLoaderInfo = [this](const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return;
        const QByteArray data = f.readAll();
        if (data.isEmpty())
            return;

        auto readU16Le = [](const uchar *p) -> quint16 {
            return quint16(p[0]) | (quint16(p[1]) << 8);
        };
        auto readU32Le = [](const uchar *p) -> quint32 {
            return quint32(p[0]) | (quint32(p[1]) << 8)
                | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
        };
        auto readU64Le = [&](const uchar *p) -> quint64 {
            return quint64(readU32Le(p)) | (quint64(readU32Le(p + 4)) << 32);
        };
        auto findAsciiTag = [&](const QByteArray &tag) -> QString {
            const int pos = data.indexOf(tag);
            if (pos < 0)
                return QString();
            QByteArray out;
            for (int i = pos + tag.size(); i < data.size(); ++i) {
                const uchar ch = static_cast<uchar>(data.at(i));
                if (ch == 0 || ch == '\r' || ch == '\n' || !isprint(ch))
                    break;
                out.append(char(ch));
            }
            return QString::fromLatin1(out);
        };

        QString cls = QStringLiteral("原始/未知格式");
        QString arch = QStringLiteral("未知架构");

        if (data.size() >= 20
            && uchar(data.at(0)) == 0x7F
            && data.at(1) == 'E'
            && data.at(2) == 'L'
            && data.at(3) == 'F') {
            const uchar elfClass = uchar(data.at(4));
            const uchar elfData = uchar(data.at(5));
            cls = elfClass == 2 ? QStringLiteral("ELF64")
                : elfClass == 1 ? QStringLiteral("ELF32")
                : QStringLiteral("ELF");
            if (elfData == 1) {
                const quint16 machine = readU16Le(reinterpret_cast<const uchar *>(data.constData()) + 18);
                arch = machine == 183 ? QStringLiteral("ARM64")
                    : machine == 40 ? QStringLiteral("ARM")
                    : QStringLiteral("未知架构");
            }
        }

        QString summary = arch.isEmpty()
            ? cls
            : QStringLiteral("%1/%2").arg(cls, arch);
        appendLog(QStringLiteral("已解析: %1 | %2 KB")
                      .arg(summary)
                      .arg((data.size() + 1023) / 1024),
                  QStringLiteral("#6B7280"));

        QStringList versionParts;
        const QString qc = findAsciiTag("QC_IMAGE_VERSION_STRING=");
        const QString oem = findAsciiTag("OEM_IMAGE_VERSION_STRING=");
        const QString variant = findAsciiTag("IMAGE_VARIANT_STRING=");
        if (!qc.isEmpty())
            versionParts << QStringLiteral("QC %1").arg(qc);
        if (!variant.isEmpty())
            versionParts << variant;
        if (versionParts.isEmpty() && !oem.isEmpty())
            versionParts << QStringLiteral("OEM %1").arg(oem);
        if (!versionParts.isEmpty())
            appendLog(QStringLiteral("加载器版本: %1")
                          .arg(versionParts.join(QStringLiteral(" | "))),
                      QStringLiteral("#6B7280"));
    };

    connect(ui->firehoseLineEdit, &QLineEdit::textChanged, this, [this, logSelectedLoaderInfo](const QString &text) {
        const QString path = text.trimmed();
        if (path.isEmpty())
            return;
        QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile())
            return;
        static QString lastLoggedPath;
        if (lastLoggedPath == fi.absoluteFilePath())
            return;
        lastLoggedPath = fi.absoluteFilePath();
        logSelectedLoaderInfo(lastLoggedPath);
    });

    connect(ui->firehoseBrowseBtn, &QPushButton::clicked, this, [=]() {
        browseFn(ui->firehoseLineEdit,
                 "Firehose 加载器 (*.mbn *.bin *.elf *.melf prog_*.mbn prog_*.elf);;所有文件 (*.*)");
    });
    connect(ui->rawprogramBrowseBtn, &QPushButton::clicked, this, [this]() {
        QStringList paths = QFileDialog::getOpenFileNames(
            this, "选择 Rawprogram XML（可多选）", "",
            "Rawprogram XML (rawprogram*.xml);;所有文件 (*.*)");
        if (paths.isEmpty()) return;
        ui->rawprogramLineEdit->setText(paths.join(";"));

        QVector<edl_flash_task_t> allTasks;
        for (const QString &p : paths) {
            edl_flash_task_t tasks[512];
            QFileInfo fi(p);
            int n = edl_rawprogram_parse(p.toUtf8().constData(),
                                         fi.absolutePath().toUtf8().constData(),
                                         tasks, 512);
            if (n > 0) {
                appendLog(QString("%1 → %2 个任务").arg(fi.fileName()).arg(n), "#2E8B3E");
                for (int i = 0; i < n; ++i)
                    allTasks.append(tasks[i]);
            } else {
                appendLog(QString("%1 → 解析失败").arg(fi.fileName()), "#C0392B");
            }
        }

        if (!allTasks.isEmpty()) {
            if (edl_service_is_connected(m_edlService) && ui->readSysInfoCheckBox->isChecked()) {
                const int devSec = edl_service_sector_size(m_edlService);
                if (devSec > 0) {
                    for (int ti = 0; ti < allTasks.size(); ++ti)
                        allTasks[ti].sector_size = devSec;
                    appendLog(QStringLiteral("【配置】已用设备扇区大小 %1 B 计算 rawprogram 分区大小（勾选「读取设备信息」）")
                                    .arg(devSec),
                                "#6B7280");
                }
            }
            for (int ti = 0; ti < allTasks.size(); ++ti)
                edl_flash_task_infer_sectors_from_image(&allTasks[ti]);

            auto *table = ui->partitionTable;
            table->setRowCount(allTasks.size());
            for (int i = 0; i < allTasks.size(); ++i) {
                const auto &t = allTasks[i];
                QString label = QString::fromUtf8(t.label);
                if (label.isEmpty()) label = QString::fromUtf8(t.filename);
                const char *typeTag = (t.type == EDL_TASK_PROGRAM) ? "" :
                                      (t.type == EDL_TASK_ERASE)   ? " [擦除]" : " [清零]";

                auto *nameItem = new QTableWidgetItem(label + QString::fromUtf8(typeTag));
                nameItem->setFlags(nameItem->flags() | Qt::ItemIsUserCheckable);
                nameItem->setCheckState(Qt::Checked);
                nameItem->setData(RoleStartSector, QVariant::fromValue<qint64>(t.start_sector));
                nameItem->setData(RoleNumSectors, QVariant::fromValue<qint64>(t.num_sectors));
                nameItem->setData(RoleSectorSize, t.sector_size > 0 ? t.sector_size : 4096);
                nameItem->setData(RoleLun, t.lun);
                nameItem->setData(RoleStartSectorExpr, QString::fromUtf8(t.start_sector_expr));
                table->setItem(i, ColName, nameItem);
                table->setItem(i, ColStartSector, new QTableWidgetItem(QString::number(t.start_sector)));
                table->setItem(i, ColSectorCount, new QTableWidgetItem(QString::number(t.num_sectors)));
                table->setItem(i, ColSize, new QTableWidgetItem(
                    formatSize(t.num_sectors, t.sector_size > 0 ? t.sector_size : 4096)));
                table->setItem(i, ColLun, new QTableWidgetItem(QString::number(t.lun)));

                auto *fileItem = new QTableWidgetItem(QString::fromUtf8(t.filename));
                fileItem->setToolTip(QString::fromUtf8(t.filepath));
                fileItem->setData(Qt::UserRole, QString::fromUtf8(t.filepath));
                if (t.type != EDL_TASK_PROGRAM)
                    fileItem->setForeground(QColor("#D97706"));
                table->setItem(i, ColFile, fileItem);
            }
            applyPartitionTableFilter();

            appendLog(QString("已加载 %1 个 XML，共 %2 个任务 → 已填充分区表")
                      .arg(paths.size()).arg(allTasks.size()), "#2E8B3E");

            ui->funcTabWidget->setCurrentWidget(ui->partitionTab);
        }
    });
    connect(ui->patchBrowseBtn, &QPushButton::clicked, this, [this]() {
        QStringList paths = QFileDialog::getOpenFileNames(
            this, "选择 Patch XML（可多选）", "",
            "Patch XML (patch*.xml);;所有文件 (*.*)");
        if (paths.isEmpty()) return;
        ui->patchLineEdit->setText(paths.join(";"));

        int totalPatches = 0;
        for (const QString &p : paths) {
            edl_patch_entry_t patches[256];
            int n = edl_patch_parse(p.toUtf8().constData(), patches, 256);
            if (n > 0) {
                appendLog(QString("%1 → %2 个补丁").arg(QFileInfo(p).fileName()).arg(n), "#2E8B3E");
                totalPatches += n;
            } else {
                appendLog(QString("%1 → 解析失败").arg(QFileInfo(p).fileName()), "#C0392B");
            }
        }
        if (totalPatches > 0)
            appendLog(QString("已加载 %1 个 XML，共 %2 个补丁")
                      .arg(paths.size()).arg(totalPatches), "#6B7280");
    });
    connect(ui->digestBrowseBtn, &QPushButton::clicked, this, [=]() {
        browseFn(ui->digestLineEdit, "Digest 文件 (*.bin *.mbn *.hash);;所有文件 (*.*)");
    });
    connect(ui->signatureBrowseBtn, &QPushButton::clicked, this, [=]() {
        browseFn(ui->signatureLineEdit, "Signature 文件 (*.bin *.sig);;所有文件 (*.*)");
    });

    auto updateAuthVisibility = [this]() {
        bool realme  = ui->realmeAuthCheckBox->isChecked();
        bool oplus   = ui->oplusAuthCheckBox->isChecked();
        bool oneplus = ui->oneplusAuthCheckBox->isChecked();
        bool xiaomi  = ui->xiaomiAuthCheckBox->isChecked();
        bool anyAuth = realme || oplus || oneplus || xiaomi;

        ui->realmeProjectLabel->setVisible(realme);
        ui->realmeProjectNumberLineEdit->setVisible(realme);
        ui->realmeProjectBrowsePlaceholder->setVisible(realme);

        bool showDigestRow = anyAuth;
        bool showSigRow    = anyAuth;

        ui->digestLabel->setVisible(showDigestRow);
        ui->digestLineEdit->setVisible(showDigestRow);
        ui->digestBrowseBtn->setVisible(showDigestRow);
        ui->signatureLabel->setVisible(showSigRow);
        ui->signatureLineEdit->setVisible(showSigRow);
        ui->signatureBrowseBtn->setVisible(showSigRow);

        if (realme) {
            ui->digestLabel->setText(QStringLiteral("Digest"));
            ui->signatureLabel->setText(QStringLiteral("Signature"));
            ui->digestLineEdit->setEnabled(true);
            ui->digestBrowseBtn->setEnabled(true);
            /* Realme 云端签名 ≠ VIP：仅 Legacy+initdigest 需 digest；签名由云端返回，勿选 VIP 的 sig 文件 */
            ui->digestLineEdit->setPlaceholderText(
                QStringLiteral("Realme：仅旧协议+initdigest 时需要（多数新机可空）"));
            ui->signatureLineEdit->setEnabled(false);
            ui->signatureBrowseBtn->setEnabled(false);
            ui->signatureLineEdit->setText("");
            ui->signatureLineEdit->setPlaceholderText(
                QStringLiteral("Realme：云端签名，无需本地 Signature 文件"));
        } else if (oplus) {
            ui->digestLabel->setText(QStringLiteral("Digest"));
            ui->signatureLabel->setText(QStringLiteral("Signature"));
            ui->digestLineEdit->setEnabled(true);
            ui->digestBrowseBtn->setEnabled(true);
            ui->digestLineEdit->setPlaceholderText(
                QStringLiteral("OPLUS VIP：Digest 文件 (Hash Segment)"));
            ui->signatureLineEdit->setEnabled(true);
            ui->signatureBrowseBtn->setEnabled(true);
            ui->signatureLineEdit->setPlaceholderText(
                QStringLiteral("OPLUS VIP：Signature 文件 (RSA-2048)"));
        } else if (oneplus || xiaomi) {
            ui->digestLineEdit->setEnabled(false);
            ui->digestBrowseBtn->setEnabled(false);
            ui->digestLineEdit->setText("");
            ui->digestLineEdit->setPlaceholderText("无需选择");
            ui->signatureLineEdit->setEnabled(false);
            ui->signatureBrowseBtn->setEnabled(false);
            ui->signatureLineEdit->setText("");
            ui->signatureLineEdit->setPlaceholderText("无需选择");
        }
    };
    connect(ui->realmeAuthCheckBox, &QCheckBox::toggled, this, updateAuthVisibility);
    connect(ui->oplusAuthCheckBox, &QCheckBox::toggled, this, updateAuthVisibility);
    connect(ui->oneplusAuthCheckBox, &QCheckBox::toggled, this, updateAuthVisibility);
    connect(ui->xiaomiAuthCheckBox, &QCheckBox::toggled, this, updateAuthVisibility);
    updateAuthVisibility();

    auto exclusiveAuth = [this](QCheckBox *checked) {
        QCheckBox *boxes[] = {
            ui->realmeAuthCheckBox, ui->oplusAuthCheckBox,
            ui->oneplusAuthCheckBox, ui->xiaomiAuthCheckBox
        };
        for (auto *box : boxes) {
            if (box != checked) box->setChecked(false);
        }
    };
    connect(ui->realmeAuthCheckBox, &QCheckBox::toggled, this, [=](bool on) { if (on) exclusiveAuth(ui->realmeAuthCheckBox); });
    connect(ui->oplusAuthCheckBox, &QCheckBox::toggled, this, [=](bool on) { if (on) exclusiveAuth(ui->oplusAuthCheckBox); });
    connect(ui->oneplusAuthCheckBox, &QCheckBox::toggled, this, [=](bool on) { if (on) exclusiveAuth(ui->oneplusAuthCheckBox); });
    connect(ui->xiaomiAuthCheckBox, &QCheckBox::toggled, this, [=](bool on) { if (on) exclusiveAuth(ui->xiaomiAuthCheckBox); });
    connect(ui->readSysInfoCheckBox, &QCheckBox::toggled, this, [=](bool on) {
        QSettings ss;
        ss.setValue(QStringLiteral("ui/readDeviceInfo"), on);
    });
    connect(ui->writeFileLengthOnlyCheckBox, &QCheckBox::toggled, this, [=](bool on) {
        QSettings ss;
        ss.setValue(QStringLiteral("flash/writeFileLengthOnly"), on);
    });
    ui->readSysInfoCheckBox->setToolTip(
        QStringLiteral("勾选后：连接成功时会读取 Firehose 存储信息，并同步「存储类型」与扇区大小；"
                       "已连接且勾选时，解析 rawprogram XML 会用设备扇区大小计算分区显示大小。"));
}

namespace {
constexpr auto kLogColorSection = "#5B6B8C";
constexpr auto kLogColorSuccess = "#2E8B3E";
constexpr auto kLogColorWarning = "#C48A0E";
constexpr auto kLogColorError = "#C0392B";
constexpr auto kLogColorMuted = "#6B7280";
constexpr auto kLogColorDetail = "#A0A8B8";

QString normalizeUiLogLine(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QChar(0x3010))) {
        const int close = text.indexOf(QChar(0x3011));
        if (close > 0 && close <= 16)
            text.remove(0, close + 1);
    }
    return text.trimmed();
}

bool shouldPromoteDetailLogToMainUi(const QString &line)
{
    return line.startsWith(QStringLiteral("开始读取分区表"))
        || line.startsWith(QStringLiteral("正在检测高通 EDL 设备"))
        || line.startsWith(QStringLiteral("检测到 EDL 设备:"))
        || line.startsWith(QStringLiteral("GPT 扫描范围已按 storage-info 限制为 LUN"))
        || line.startsWith(QStringLiteral("读取 LUN"))
        || line.startsWith(QStringLiteral("解析 LUN"))
        || line.startsWith(QStringLiteral("正在读取 LUN"))
        || (line.startsWith(QStringLiteral("LUN")) && line.contains(QStringLiteral("不存在，已跳过")))
        || (line.startsWith(QStringLiteral("LUN")) && line.contains(QStringLiteral("GPT 扫描失败")))
        || line.startsWith(QStringLiteral("UFS 上 GPT 扫描为空"))
        || line.startsWith(QStringLiteral("GPT 分区总数:"))
        || line.startsWith(QStringLiteral("【耗时】全 LUN 读取 GPT"));
}

bool shouldHideDetailLogInUi(const QString &line)
{
    if (line.startsWith(QLatin1String("[partition] read ")))
        return true;
    if (line.startsWith(QLatin1String("[partition] 读取 ")))
        return true;
    if (line.startsWith(QLatin1String("[partition] no build.prop")))
        return true;
    if (line.startsWith(QLatin1String("[props] ext4: build.prop loaded from ")))
        return true;
    if (line.startsWith(QLatin1String("[props] erofs: build.prop loaded from ")))
        return true;
    if (line.startsWith(QLatin1String("[props] ext4：已从 ")))
        return true;
    if (line.startsWith(QLatin1String("[props] erofs：已从 ")))
        return true;
    if (line.startsWith(QLatin1String("LUN")) && line.contains(QLatin1String(" read cmd: ")))
        return true;
    if (line.startsWith(QLatin1String("LUN")) && line.contains(QLatin1String(" XML resp: ")))
        return true;
    if (line.startsWith(QLatin1String("LUN")) && line.contains(QLatin1String(": rawmode ACK")))
        return true;
    if (line.startsWith(QLatin1String("[Firehose] Configure request:")))
        return true;
    if (line.startsWith(QLatin1String("[Firehose] Configure fallback:")))
        return true;
    if (line.startsWith(QLatin1String("program(raw):")))
        return true;
    return false;
}
} // namespace

void MainWindow::openLogFile()
{
    if (m_logFile.isOpen()) return;
    QSettings s;
    QString dir = s.value(QStringLiteral("log/savePath")).toString().trimmed();
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
              + QStringLiteral("/logs");
    QDir().mkpath(dir);
    const QString name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    m_logFile.setFileName(dir + QStringLiteral("/edl_%1.log").arg(name));
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        m_logStream.setDevice(&m_logFile);
        m_logStream << "=== SAKURAEDL session " << name << " ===" << Qt::endl;
    }
}

void MainWindow::writeLogLine(const QString &plainText)
{
    if (!m_logAutoSave) return;
    if (!m_logFile.isOpen()) openLogFile();
    if (m_logFile.isOpen()) {
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        m_logStream << stamp << "  " << plainText << Qt::endl;
    }
}

void MainWindow::appendSectionLog(const QString &title)
{
    appendLog(QStringLiteral("──────── %1 ────────").arg(title), QString::fromLatin1(kLogColorSection));
}

void MainWindow::appendInfoLog(const QString &text)
{
    appendLog(text, QString::fromLatin1(kLogColorMuted));
}

void MainWindow::appendSuccessLog(const QString &text)
{
    appendLog(text, QString::fromLatin1(kLogColorSuccess));
}

void MainWindow::appendWarningLog(const QString &text)
{
    appendLog(text, QString::fromLatin1(kLogColorWarning));
}

void MainWindow::appendErrorLog(const QString &text)
{
    appendLog(text, QString::fromLatin1(kLogColorError));
}

void MainWindow::appendMutedLog(const QString &text)
{
    appendLog(text, QString::fromLatin1(kLogColorMuted));
}

void MainWindow::appendLog(const QString &text, const QString &color)
{
    const QString line = normalizeUiLogLine(text);
    if (line.isEmpty())
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (line == m_lastLogText && color == m_lastLogColor && (nowMs - m_lastLogMs) < 1200)
        return;
    m_lastLogText = line;
    m_lastLogColor = color;
    m_lastLogMs = nowMs;

    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    QString html = QStringLiteral("<span style=\"color:#8B939E;\">%1</span> "
                                  "<span style=\"color:%2;\">%3</span>")
                       .arg(stamp, color, line.toHtmlEscaped());
    appendLogHtml(html);
    writeLogLine(line);
}

void MainWindow::appendLogHtml(const QString &html)
{
    ui->logConsole->appendHtml(html);
    auto *sb = ui->logConsole->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::appendLogDetail(const QString &text)
{
    const QString line = normalizeUiLogLine(text);
    if (line.isEmpty())
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (line == m_lastDetailLogText && (nowMs - m_lastDetailLogMs) < 800)
        return;
    m_lastDetailLogText = line;
    m_lastDetailLogMs = nowMs;

    writeLogLine(QStringLiteral("[detail] ") + line);
    if (shouldPromoteDetailLogToMainUi(line)) {
        appendMutedLog(line);
        return;
    }
    if (!m_logShowDetail || shouldHideDetailLogInUi(line))
        return;
    {
        const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
        QString html = QStringLiteral("<span style=\"color:#8B939E;\">%1</span> "
                                      "<span style=\"color:%2;\">%3</span>")
                           .arg(stamp, QString::fromLatin1(kLogColorDetail), line.toHtmlEscaped());
        appendLogHtml(html);
    }
}

static QString localizedLogAction(const QString &action)
{
    if (action == QLatin1String("Read info"))
        return QStringLiteral("读取信息");
    if (action == QLatin1String("Read info / GetStorageInfo"))
        return QStringLiteral("读取信息 / 获取字库信息");
    if (action == QLatin1String("Read storage device info"))
        return QStringLiteral("读取字库设备信息");
    if (action == QLatin1String("Read GPT"))
        return QStringLiteral("读取分区表");
    if (action == QLatin1String("Write GPT"))
        return QStringLiteral("写 GPT");
    if (action == QLatin1String("Erase FRP"))
        return QStringLiteral("移除 FRP");
    if (action == QLatin1String("Factory reset"))
        return QStringLiteral("恢复出厂设置");
    if (action == QLatin1String("Activate Boot A"))
        return QStringLiteral("激活 Boot A");
    if (action == QLatin1String("Activate Boot B"))
        return QStringLiteral("激活 Boot B");
    if (action == QLatin1String("GetStorageInfo (connect sync)"))
        return QStringLiteral("连接后同步存储信息");
    if (action == QLatin1String("Power command"))
        return QStringLiteral("电源指令");
    if (action == QLatin1String("power download (EDL)")
        || action == QLatin1String("power download(EDL)"))
        return QStringLiteral("重启到 EDL (9008)");
    if (action == QLatin1String("Reboot to system"))
        return QStringLiteral("重启到系统");
    if (action == QLatin1String("Power off"))
        return QStringLiteral("关机");
    if (action == QLatin1String("Reboot to EDL (9008)"))
        return QStringLiteral("重启到 EDL (9008)");
    if (action == QLatin1String("Fastbootd (MISC)"))
        return QStringLiteral("重启到 Fastbootd (MISC)");
    if (action == QLatin1String("Recovery (MISC)"))
        return QStringLiteral("重启到 Recovery (MISC)");
    if (action == QLatin1String("Read partition"))
        return QStringLiteral("读取分区");
    if (action == QLatin1String("Write partition"))
        return QStringLiteral("写入分区");
    if (action == QLatin1String("Erase partition"))
        return QStringLiteral("擦除分区");
    if (action == QLatin1String("Apply patch"))
        return QStringLiteral("应用补丁");
    if (action == QLatin1String("Fix GPT"))
        return QStringLiteral("修复 GPT");
    if (action.startsWith(QLatin1String("Batch read item ")))
        return QStringLiteral("批量读取第 %1 项").arg(action.mid(16));
    if (action.startsWith(QLatin1String("Batch write item ")))
        return QStringLiteral("批量写入第 %1 项").arg(action.mid(17));
    if (action.startsWith(QLatin1String("Batch erase item ")))
        return QStringLiteral("批量擦除第 %1 项").arg(action.mid(17));
    if (action.startsWith(QLatin1String("Factory reset (")) && action.endsWith(QLatin1Char(')')))
        return QStringLiteral("恢复出厂设置（%1）").arg(action.mid(15, action.size() - 16));
    return action;
}

static bool progressOperationUsesByteSemantics(const QString &label)
{
    return label != QStringLiteral("读取信息")
        && label != QStringLiteral("读取信息 / 获取字库信息")
        && label != QStringLiteral("读取字库设备信息")
        && label != QStringLiteral("读取分区表");
}

void MainWindow::logEdlResult(const QString &action, edl_error_t err)
{
    const QString actionText = localizedLogAction(action);
    if (err == EDL_OK) {
        appendSuccessLog(actionText + QStringLiteral(" 成功"));
        return;
    }

    const QString detail = QString::fromUtf8(edl_error_str(err));
    if (err == EDL_ERR_CANCELLED) {
        appendLog(QStringLiteral("%1 已取消").arg(actionText), "#7D6B3A");
        return;
    }
    if (err == EDL_ERR_FH_LUN_ABSENT || err == EDL_ERR_FH_FIXGPT_UNSUPPORTED) {
        appendLog(QStringLiteral("%1 提示：%2").arg(actionText, detail), "#C48A0E");
        return;
    }

    markAsyncTaskFailed();
    if (edl_error_is_fail(err))
        appendErrorLog(QStringLiteral("%1失败：%2").arg(actionText, detail));
    else
        appendErrorLog(QStringLiteral("%1错误：%2").arg(actionText, detail));
}

void MainWindow::markAsyncTaskFailed()
{
    m_asyncFail.store(true, std::memory_order_relaxed);
}

void MainWindow::applyDeviceStorageToUi(bool logLine)
{
    if (!m_edlService || !edl_service_is_connected(m_edlService))
        return;
    const QString stor = QString::fromUtf8(edl_service_storage_type(m_edlService));
    ui->storageCombo->blockSignals(true);
    if (stor.compare(QLatin1String("emmc"), Qt::CaseInsensitive) == 0)
        ui->storageCombo->setCurrentIndex(1);
    else if (stor.contains(QLatin1String("ufs"), Qt::CaseInsensitive))
        ui->storageCombo->setCurrentIndex(2);
    else
        ui->storageCombo->setCurrentIndex(0);
    ui->storageCombo->blockSignals(false);

    if (logLine) {
        const int sec = edl_service_sector_size(m_edlService);
        const int pay = edl_service_max_payload_bytes(m_edlService);
        appendLog(QStringLiteral("【配置】存储类型已与设备对齐: %1 | 扇区 %2 B | 载荷 %3 KB")
                      .arg(stor.isEmpty() ? QStringLiteral("(未知)") : stor)
                      .arg(sec)
                      .arg(pay > 0 ? pay / 1024 : 0),
                  "#6B7280");
    }
}

void MainWindow::setProgress(int value)
{
    ui->progressBar->setValue(value);
    ui->progressBar->setFormat(QStringLiteral("%1%").arg(static_cast<double>(value), 0, 'f', 1));
    m_lastProgressPctAtomic.store(qBound(0, value, 100), std::memory_order_relaxed);
}

void MainWindow::clearBatchProgressContext()
{
    m_batchProgressActive.store(false, std::memory_order_relaxed);
    m_batchProgressBaseBytes.store(0, std::memory_order_relaxed);
    m_batchProgressSpanBytes.store(0, std::memory_order_relaxed);
    m_batchProgressTotalBytes.store(0, std::memory_order_relaxed);
}

void MainWindow::beginBatchProgress(qint64 totalBytes)
{
    clearBatchProgressContext();
    if (totalBytes <= 0)
        return;
    m_batchProgressTotalBytes.store(totalBytes, std::memory_order_relaxed);
    m_batchProgressActive.store(true, std::memory_order_relaxed);
}

void MainWindow::setBatchProgressWindow(qint64 baseBytes, qint64 spanBytes)
{
    const qint64 totalBytes = m_batchProgressTotalBytes.load(std::memory_order_relaxed);
    if (totalBytes <= 0 || spanBytes <= 0) {
        clearBatchProgressContext();
        return;
    }
    if (baseBytes < 0)
        baseBytes = 0;
    if (baseBytes > totalBytes)
        baseBytes = totalBytes;
    m_batchProgressBaseBytes.store(baseBytes, std::memory_order_relaxed);
    m_batchProgressSpanBytes.store(spanBytes, std::memory_order_relaxed);
    m_batchProgressActive.store(true, std::memory_order_relaxed);
}

void MainWindow::resetProgressTracking()
{
    clearBatchProgressContext();
    m_progressUsesByteSemantics.store(true, std::memory_order_relaxed);
    m_progOpStartMs.store(0, std::memory_order_relaxed);
    m_progUiLastMs.store(0, std::memory_order_relaxed);
    m_progLastReportedCurrent.store(-1, std::memory_order_relaxed);
    m_progLastReportedTotal.store(-1, std::memory_order_relaxed);
    m_progSpeedPrevMs.store(0, std::memory_order_relaxed);
    m_progSpeedPrevCurrent.store(0, std::memory_order_relaxed);
    m_progLastPctShown = 0;
    m_progDisplayMbps = -1.0;
}

void MainWindow::endBatchProgress()
{
    resetProgressTracking();
}

void MainWindow::resetUiAfterDisconnect()
{
    m_trackedComPort.clear();
#if 0
    ui->titleLabel->setText(QStringLiteral("SAKURAEDL %1 | Port released") /*
        QStringLiteral("SAKURAEDL %1 | QQ群 612824646")
            */ .arg(QCoreApplication::applicationVersion()));
#endif
    ui->titleLabel->setText(QStringLiteral("SAKURAEDL %1 | 未连接")
                                .arg(QCoreApplication::applicationVersion()));
    ui->partitionTable->setRowCount(0);
    endBatchProgress();
    ui->progressBar->setValue(0);
    ui->progressBar->setFormat(QStringLiteral("%p%"));
    ui->progressBar->setProperty("state", "idle");
    ui->progressBar->style()->unpolish(ui->progressBar);
    ui->progressBar->style()->polish(ui->progressBar);
    ui->progressBar->setToolTip(QString());
    ui->speedValueLabel->setText(QStringLiteral("-- MB/s"));
    ui->timeValueLabel->setText(QStringLiteral("已用 00:00"));
}

void MainWindow::forceDisconnectAndResetUi(const QString &reason)
{
    if (!m_edlIoMutex.tryLock()) {
        QTimer::singleShot(120, this, [this, reason]() { forceDisconnectAndResetUi(reason); });
        return;
    }
    if (!edl_service_is_connected(m_edlService)) {
        m_edlIoMutex.unlock();
        return;
    }
    const bool wasBusy = m_busy;
    appendLog(reason, "#C0392B");
    edl_service_disconnect(m_edlService);
    m_edlIoMutex.unlock();

    resetUiAfterDisconnect();
    m_busy = false;
    /* 拔线时若仍有任务线程在跑，置 cancel 促其尽快退出；空闲断开则清零以免误挡下次连接 */
    if (wasBusy)
        m_cancelRequested.store(true, std::memory_order_relaxed);
    else
        m_cancelRequested.store(false, std::memory_order_relaxed);
    setBusy(false);
}

#if 0
void MainWindow::releaseEdlPortAfterOperation()
{
    QMutexLocker lock(&m_edlIoMutex);
    if (!edl_service_is_connected(m_edlService)) {
        m_trackedComPort.clear();
        return;
    }
    edl_service_disconnect(m_edlService);
    m_trackedComPort.clear();
    lock.unlock();

    ui->titleLabel->setText(
        QStringLiteral("SAKURAEDL %1 | QQ群 612824646 | 串口已释放")
            .arg(QCoreApplication::applicationVersion()));
}

#endif
void MainWindow::relayEdlProgressFromCore(qint64 current, qint64 total)
{
    if (total <= 0)
        return;

    qint64 done = current < 0 ? 0 : current;
    qint64 tot = total;
    const bool batchActive = m_batchProgressActive.load(std::memory_order_relaxed)
        && m_batchProgressTotalBytes.load(std::memory_order_relaxed) > 0
        && m_batchProgressSpanBytes.load(std::memory_order_relaxed) > 0;
    if (batchActive) {
        const qint64 baseBytes = m_batchProgressBaseBytes.load(std::memory_order_relaxed);
        const qint64 spanBytes = m_batchProgressSpanBytes.load(std::memory_order_relaxed);
        const qint64 batchTotalBytes = m_batchProgressTotalBytes.load(std::memory_order_relaxed);
        const qint64 boundedCurrent = qBound<qint64>(0, current, total);
        const long double ratio =
            static_cast<long double>(boundedCurrent) / static_cast<long double>(total);
        const qint64 mappedBytes =
            baseBytes + static_cast<qint64>(ratio * static_cast<long double>(spanBytes));
        done = qBound<qint64>(0, mappedBytes, batchTotalBytes);
        tot = batchTotalBytes;
    }
    const bool progressUsesBytes = batchActive
        || m_progressUsesByteSemantics.load(std::memory_order_relaxed);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const qint64 lastCur = m_progLastReportedCurrent.load(std::memory_order_relaxed);
    const qint64 lastTot = m_progLastReportedTotal.load(std::memory_order_relaxed);
    const bool newOp = (lastTot < 0) || (tot != lastTot)
        || (lastCur >= 0 && done < lastCur);
    if (newOp) {
        m_progLastPctShown = 0;
        m_progOpStartMs.store(now, std::memory_order_relaxed);
        m_progDisplayMbps = -1.0;
        m_progSpeedPrevMs.store(now, std::memory_order_relaxed);
        m_progSpeedPrevCurrent.store(done, std::memory_order_relaxed);
    }
    m_progLastReportedCurrent.store(done, std::memory_order_relaxed);
    m_progLastReportedTotal.store(tot, std::memory_order_relaxed);

    const qint64 t0 = m_progOpStartMs.load(std::memory_order_relaxed);
    const qint64 elapsed = now - t0;

    const qint64 lastUiMs = m_progUiLastMs.load(std::memory_order_relaxed);
    const double pctFloat = 100.0 * static_cast<double>(done) / static_cast<double>(tot);
    int pctDisp = static_cast<int>(qMin(100.0, pctFloat));
    const bool force = (done >= tot) || (pctDisp >= 100);

    if (!force && (now - lastUiMs) < 33)
        return;

    const qint64 pvMs = m_progSpeedPrevMs.load(std::memory_order_relaxed);
    const qint64 pvCur = m_progSpeedPrevCurrent.load(std::memory_order_relaxed);
    const qint64 dt = now - pvMs;
    const qint64 delta = done - pvCur;

    /* 实时速度：本帧与上一帧之间的瞬时 MB/s（不做滑动平均） */
    double rawMbps = -1.0;
    if (progressUsesBytes && dt >= 25 && delta > 0) {
        rawMbps = (static_cast<double>(delta) / 1048576.0) / (static_cast<double>(dt) / 1000.0);
        m_progDisplayMbps = rawMbps;
    }

    m_progSpeedPrevMs.store(now, std::memory_order_relaxed);
    m_progSpeedPrevCurrent.store(done, std::memory_order_relaxed);
    m_progUiLastMs.store(now, std::memory_order_relaxed);

    if (!force && pctDisp < m_progLastPctShown)
        pctDisp = m_progLastPctShown;
    else
        m_progLastPctShown = pctDisp;
    if (force) {
        m_progLastPctShown = 100;
        pctDisp = 100;
    }

    const QString elapsedStr = formatClockFromMs(elapsed);
    QString speedTxt;
    QString timeLine;
    QString barFmt;
    QString progressTip;
    if (progressUsesBytes) {
        const double dispM = (rawMbps >= 0.0) ? rawMbps : m_progDisplayMbps;
        speedTxt = (dispM >= 0.0)
            ? QStringLiteral("%1 MB/s").arg(dispM, 0, 'f', 2)
            : QStringLiteral("-- MB/s");
        const double etaRate = dispM;
        timeLine = QStringLiteral("耗时 %1").arg(elapsedStr);

        if (!force && etaRate >= 0.05 && tot > done) {
            const qint64 rem = tot - done;
            const double sec = static_cast<double>(rem) / (etaRate * 1048576.0);
            if (sec >= 0.0 && sec < 86400.0 * 2.0) {
                const qint64 etaMs = static_cast<qint64>(sec * 1000.0);
                timeLine += QStringLiteral(" · 预计剩余 %1").arg(formatClockFromMs(etaMs));
            }
        } else if (force) {
            timeLine += QStringLiteral(" · 已完成");
        }

        barFmt = QStringLiteral("%1%  ·  %2")
                     .arg(pctFloat, 0, 'f', 1)
                     .arg(speedTxt);
        progressTip = batchActive
            ? QStringLiteral("总进度 %1 / %2 · 实时 %3")
                  .arg(done)
                  .arg(tot)
                  .arg(dispM >= 0.0
                           ? QString::number(dispM, 'f', 2) + QStringLiteral(" MB/s")
                           : QStringLiteral("-- MB/s"))
            : QStringLiteral("当前进度 %1 / %2 · 实时 %3")
                  .arg(done)
                  .arg(tot)
                  .arg(dispM >= 0.0
                           ? QString::number(dispM, 'f', 2) + QStringLiteral(" MB/s")
                           : QStringLiteral("-- MB/s"));
    } else {
        speedTxt = QStringLiteral("阶段 %1 / %2").arg(done).arg(tot);
        timeLine = QStringLiteral("耗时 %1").arg(elapsedStr);
        if (force)
            timeLine += QStringLiteral(" · 已完成");

        barFmt = QStringLiteral("%1%  ·  阶段 %2 / %3")
                     .arg(pctFloat, 0, 'f', 1)
                     .arg(done)
                     .arg(tot);
        progressTip = QStringLiteral("阶段进度 %1 / %2").arg(done).arg(tot);
    }

    QMetaObject::invokeMethod(this, [this, pctDisp, barFmt, speedTxt, timeLine, progressTip]() {
        ui->progressBar->setValue(pctDisp);
        ui->progressBar->setFormat(barFmt);
        ui->progressBar->setToolTip(progressTip);
        m_lastProgressPctAtomic.store(pctDisp, std::memory_order_relaxed);
        ui->speedValueLabel->setText(speedTxt);
        ui->timeValueLabel->setText(timeLine);
    }, Qt::QueuedConnection);
}

void MainWindow::populateDemoLog()
{
    appendLog(QStringLiteral("SAKURAEDL v%1 就绪").arg(QCoreApplication::applicationVersion()),
              "#2D3748");
    appendLog("等待 EDL 模式设备连接...", "#6B7280");
}

/* ===== Disabled Tabs (IMEI / Firmware) ===== */

void MainWindow::setupDisabledTabs()
{
    auto setupDisabled = [](QLabel *label) {
        QPixmap lockPix(":/icons/icons/disabled.svg");
        if (lockPix.isNull()) {
            label->setText("已禁用");
            return;
        }
        QPixmap scaled = lockPix.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmap faded(scaled.size());
        faded.fill(Qt::transparent);
        QPainter p(&faded);
        p.setOpacity(0.35);
        p.drawPixmap(0, 0, scaled);
        p.end();

        label->setPixmap(faded);
        label->setAlignment(Qt::AlignCenter);
    };

    setupDisabled(ui->imeiDisabledLabel);
    setupDisabled(ui->firmwareDisabledLabel);

    auto addOverlayText = [](QLabel *label, const QString &text) {
        label->setText(QString("<div style='text-align:center;'>"
                               "<br/><br/><br/>"
                               "<span style='font-size:16px; color:rgba(100,110,130,0.45); font-weight:600;'>%1</span>"
                               "</div>").arg(text));
        label->setTextFormat(Qt::RichText);
        label->setAlignment(Qt::AlignCenter);
    };

    addOverlayText(ui->imeiDisabledLabel, "已禁用");
    addOverlayText(ui->firmwareDisabledLabel, "已禁用");
}

/* ===== EDL Service Setup ===== */

static void edlLogCb(const char *msg, void *user_data)
{
    auto *w = static_cast<MainWindow *>(user_data);
    QString text = QString::fromUtf8(msg);
    QMetaObject::invokeMethod(w, [w, text]() {
        w->appendLog(text);
    }, Qt::QueuedConnection);
}

static void edlLogDetailCb(const char *msg, void *user_data)
{
    auto *w = static_cast<MainWindow *>(user_data);
    QString text = QString::fromUtf8(msg);
    QMetaObject::invokeMethod(w, [w, text]() {
        w->appendLogDetail(text);
    }, Qt::QueuedConnection);
}

namespace {
void edlProgressTrampoline(int64_t current, int64_t total, void *user_data)
{
    auto *w = static_cast<MainWindow *>(user_data);
    w->relayEdlProgressFromCore(static_cast<qint64>(current), static_cast<qint64>(total));
}

bool edlCancelCheck(void *user_data)
{
    auto *w = static_cast<MainWindow *>(user_data);
    return w->isCancelRequested();
}
} // namespace

void MainWindow::setupEdlService()
{
    edl_callbacks_t cb = {};
    cb.log = edlLogCb;
    cb.log_detail = edlLogDetailCb;
    cb.progress = edlProgressTrampoline;
    cb.is_cancelled = edlCancelCheck;
    cb.user_data = this;
    m_edlService = edl_service_create(&cb);
}

void MainWindow::setupPortWatchTimer()
{
    m_portWatchTimer = new QTimer(this);
    /* 500ms：先查系统是否仍有 COM（无 I/O 锁），再隔次 NOP 验 Firehose */
    m_portWatchTimer->setInterval(500);
    connect(m_portWatchTimer, &QTimer::timeout, this, &MainWindow::onPortWatchTimeout);
    m_portWatchTimer->start();
}

#if 0
void MainWindow::onPortWatchTimeout()
{
    if (!edl_service_is_connected(m_edlService))
        return;

    QString port = m_trackedComPort;
    if (port.isEmpty()) {
        port = QString::fromUtf8(edl_service_port_name(m_edlService));
        if (!port.isEmpty())
            m_trackedComPort = port;
    }
    if (port.isEmpty())
        return;

    /* 无串口句柄、不抢 I/O 锁：仅查 Windows 设备树里是否仍有该 COM（拔线可立即发现） */
    if (!edl_port_detect_has_edl_port(port.toUtf8().constData())) {
        forceDisconnectAndResetUi(
            QStringLiteral("COM 端口已断开：%1（系统已无此串口），已清除 Firehose 会话与分区表。").arg(port));
        return;
    }

    /* 隔次 NOP，减轻与读写并发时的串口压力；约 1s 一次 */
    m_portWatchPingPhase ^= 1;
    if (m_portWatchPingPhase != 0)
        return;

    QThread *t = QThread::create([this]() {
        if (!m_edlIoMutex.tryLock())
            return;
        if (!edl_service_is_connected(m_edlService)) {
            m_edlIoMutex.unlock();
            return;
        }
        edl_error_t e = edl_service_ping(m_edlService);
        char portBuf[32] = {0};
        const char *pn = edl_service_port_name(m_edlService);
        if (pn)
            strncpy(portBuf, pn, sizeof(portBuf) - 1);
        m_edlIoMutex.unlock();

        if (e == EDL_OK)
            return;

        const bool present = edl_port_detect_has_edl_port(portBuf);

        const QString p = QString::fromUtf8(portBuf);
        QMetaObject::invokeMethod(
            this,
            [this, present, p]() {
                if (present) {
                    forceDisconnectAndResetUi(
                        QStringLiteral("后台检测：Firehose 无响应（NOP），端口 %1 仍在线，设备可能已回到 Sahara，会话已清除。")
                            .arg(p));
                } else {
                    forceDisconnectAndResetUi(
                        QStringLiteral("后台检测：NOP 失败且端口 %1 已不在系统，会话已清除。").arg(p));
                }
            },
            Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

bool MainWindow::preflightFirehose(const QString &contextLabel)
{
    if (isCancelRequested())
        return false;

    const QByteArray port = QByteArray(edl_service_port_name(m_edlService));
    if (!port.isEmpty() && !edl_port_detect_has_edl_port(port.constData())) {
        edl_service_disconnect(m_edlService);
        m_trackedComPort.clear();
        const QString ctx = localizedLogAction(
            contextLabel.isEmpty() ? QStringLiteral("操作前") : contextLabel);
        const QString msg = QStringLiteral("COM 端口已断开（%1）：%2 未在系统中找到，已清除会话。")
                                .arg(ctx, QString::fromUtf8(port));
        QMetaObject::invokeMethod(this, [this, msg]() {
            appendLog(msg, "#C0392B");
            resetUiAfterDisconnect();
            m_cancelRequested.store(false, std::memory_order_relaxed);
        }, Qt::QueuedConnection);
        markAsyncTaskFailed();
        return false;
    }

    edl_error_t e = edl_service_ping(m_edlService);
    if (e == EDL_OK)
        return true;

    const bool present = edl_port_detect_has_edl_port(port.constData());
    const QString ctx = localizedLogAction(
        contextLabel.isEmpty() ? QStringLiteral("操作前") : contextLabel);
    const QString msg = present
        ? QStringLiteral("NOP 无响应（%1）：端口 %2 仍在线，设备可能已回到 Sahara，已清除会话，请重新连接。")
              .arg(ctx, QString::fromUtf8(port))
        : QStringLiteral("NOP 无响应（%1）：端口 %2 未在系统中找到，已清除会话。").arg(ctx, QString::fromUtf8(port));

    edl_service_disconnect(m_edlService);
    m_trackedComPort.clear();
    QMetaObject::invokeMethod(this, [this, msg]() {
        appendLog(msg, "#C0392B");
        resetUiAfterDisconnect();
        m_cancelRequested.store(false, std::memory_order_relaxed);
    }, Qt::QueuedConnection);
    markAsyncTaskFailed();
    return false;
}

#endif
void MainWindow::onPortWatchTimeout()
{
    if (!edl_service_is_connected(m_edlService))
        return;

    QString port = m_trackedComPort;
    if (port.isEmpty()) {
        port = QString::fromUtf8(edl_service_port_name(m_edlService));
        if (!port.isEmpty())
            m_trackedComPort = port;
    }
    if (port.isEmpty())
        return;

    if (!edl_port_detect_has_edl_port(port.toUtf8().constData())) {
        forceDisconnectAndResetUi(QStringLiteral("COM 端口已断开：%1").arg(port));
        return;
    }

    m_portWatchPingPhase ^= 1;
    if (m_portWatchPingPhase != 0)
        return;

    QThread *t = QThread::create([this]() {
        if (!m_edlIoMutex.tryLock())
            return;
        if (!edl_service_is_connected(m_edlService)) {
            m_edlIoMutex.unlock();
            return;
        }

        edl_error_t err = edl_service_ping(m_edlService);
        char portBuf[32] = {0};
        const char *portName = edl_service_port_name(m_edlService);
        if (portName)
            std::strncpy(portBuf, portName, sizeof(portBuf) - 1);
        m_edlIoMutex.unlock();

        if (err == EDL_OK)
            return;

        const bool present = edl_port_detect_has_edl_port(portBuf);
        const QString portText = QString::fromUtf8(portBuf);
        QMetaObject::invokeMethod(this, [this, present, portText]() {
            if (present) {
                forceDisconnectAndResetUi(
                    QStringLiteral("后台检测：Firehose 无响应，设备可能已回到 Sahara"));
            } else {
                forceDisconnectAndResetUi(QStringLiteral("COM 端口已断开：%1").arg(portText));
            }
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

bool MainWindow::preflightFirehose(const QString &contextLabel)
{
    if (isCancelRequested())
        return false;

    const QByteArray port = QByteArray(edl_service_port_name(m_edlService));
    if (!port.isEmpty() && !edl_port_detect_has_edl_port(port.constData())) {
        edl_service_disconnect(m_edlService);
        m_trackedComPort.clear();
        const QString ctx = localizedLogAction(
            contextLabel.isEmpty() ? QStringLiteral("当前操作") : contextLabel);
        const QString msg = QStringLiteral("%1期间 COM 端口已断开：%2")
                                .arg(ctx, QString::fromUtf8(port));
        QMetaObject::invokeMethod(this, [this, msg]() {
            appendLog(msg, "#C0392B");
            resetUiAfterDisconnect();
            m_cancelRequested.store(false, std::memory_order_relaxed);
        }, Qt::QueuedConnection);
        markAsyncTaskFailed();
        return false;
    }

    edl_error_t err = edl_service_ping(m_edlService);
    if (err == EDL_OK)
        return true;

    const bool present = edl_port_detect_has_edl_port(port.constData());
    const QString ctx = localizedLogAction(
        contextLabel.isEmpty() ? QStringLiteral("当前操作") : contextLabel);
    const QString msg = present
        ? QStringLiteral("%1期间 Firehose 无响应：设备可能已回到 Sahara，请重新连接").arg(ctx)
        : QStringLiteral("%1期间 COM 端口已断开：%2").arg(ctx, QString::fromUtf8(port));

    edl_service_disconnect(m_edlService);
    m_trackedComPort.clear();
    QMetaObject::invokeMethod(this, [this, msg]() {
        appendLog(msg, "#C0392B");
        resetUiAfterDisconnect();
        m_cancelRequested.store(false, std::memory_order_relaxed);
    }, Qt::QueuedConnection);
    markAsyncTaskFailed();
    return false;
}

bool MainWindow::ensureGptCacheReady()
{
    if (!m_edlService || !edl_service_is_connected(m_edlService)) {
        markAsyncTaskFailed();
        return false;
    }
    if (isCancelRequested())
        return false;
    if (edl_service_is_gpt_cache_loaded(m_edlService))
        return true;

    QMetaObject::invokeMethod(
        this,
        [this]() {
            appendLog(QStringLiteral("【分区表】本地无 GPT 缓存，正在读取分区表…（仅缓存分区信息，不会激活启动分区）"),
                      QStringLiteral("#6B7280"));
        },
        Qt::QueuedConnection);

    const edl_error_t err = edl_service_ensure_gpt_cache_ex(m_edlService, 0u);
    if (err == EDL_ERR_CANCELLED)
        return false;
    if (err != EDL_OK) {
        const QString detail = QString::fromUtf8(edl_error_str(err));
        markAsyncTaskFailed();
        QMetaObject::invokeMethod(
            this,
            [this, detail]() {
                appendLog(QStringLiteral("本地无 GPT 缓存，自动读盘失败：%1（可手动点「读取分区表」）").arg(detail),
                          QStringLiteral("#C0392B"));
            },
            Qt::QueuedConnection);
        return false;
    }

    edl_partition_info_t parts[256];
    int count = 256;
    if (edl_service_copy_cached_gpt(m_edlService, parts, &count) != EDL_OK) {
        markAsyncTaskFailed();
        QMetaObject::invokeMethod(
            this,
            [this]() {
                appendLog(QStringLiteral("GPT 缓存复制失败（内部状态异常）"), QStringLiteral("#C0392B"));
            },
            Qt::QueuedConnection);
        return false;
    }

    QVector<edl_partition_info_t> partVec(parts, parts + count);
    QMetaObject::invokeMethod(
        this,
        [this, partVec]() {
            populatePartitionTable(partVec.data(), partVec.size());
            appendLog(QStringLiteral("已自动读取分区表并载入缓存（%1 个分区）").arg(partVec.size()),
                      QStringLiteral("#6B7280"));
        },
        Qt::QueuedConnection);
    return true;
}

#if 0
static QString formatElapsedMs(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    if (ms < 60000) {
        const double s = ms / 1000.0;
        return QStringLiteral("%1 s").arg(s, 0, 'f', 2);
    }
    const int m = static_cast<int>(ms / 60000);
    const int s = static_cast<int>((ms % 60000) / 1000);
    return QStringLiteral("%1 分 %2 秒").arg(m).arg(s);
}

#endif
static QString formatElapsedMs(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    if (ms < 60000) {
        const double s = ms / 1000.0;
        return QStringLiteral("%1 s").arg(s, 0, 'f', 2);
    }
    const int m = static_cast<int>(ms / 60000);
    const int s = static_cast<int>((ms % 60000) / 1000);
    return QStringLiteral("%1 分 %2 秒").arg(m).arg(s);
}

#if 0
void MainWindow::runAsync(std::function<void()> task, const QString &operationLabel)
{
    if (m_busy) {
        appendLog("上一任务线程仍在运行，请等待其完成（SAKURAEDL 结束行出现）后再执行下一命令", "#C0392B");
        return;
    }
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_asyncFail.store(false, std::memory_order_relaxed);
    m_lastProgressPctAtomic.store(0, std::memory_order_relaxed);
    m_busy = true;
    setBusy(true);

    resetProgressTracking();
    ui->speedValueLabel->setText(QStringLiteral("-- MB/s"));
    ui->timeValueLabel->setText(QStringLiteral("已用 00:00"));

    const QString label = operationLabel.isEmpty()
                              ? QStringLiteral("未命名任务")
                              : localizedLogAction(operationLabel);

    /* 新操作独占日志区：清空历史，仅显示本轮输出 */
    const bool progressUsesBytes = progressOperationUsesByteSemantics(label);
    m_progressUsesByteSemantics.store(progressUsesBytes, std::memory_order_relaxed);
    ui->speedValueLabel->setText(progressUsesBytes ? QStringLiteral("-- MB/s")
                                                   : QStringLiteral("阶段进度"));
    ui->logConsole->clear();

    appendLog(QStringLiteral("──────── SAKURAEDL │ 任务线程已启动 │ %1 ────────").arg(label), "#5B6B8C");

    QMetaObject::invokeMethod(
        this,
        [this]() {
            ui->progressBar->setValue(0);
            ui->progressBar->setFormat(QStringLiteral("%p%"));
            ui->progressBar->setProperty("state", "running");
            ui->progressBar->style()->unpolish(ui->progressBar);
            ui->progressBar->style()->polish(ui->progressBar);
        },
        Qt::QueuedConnection);

    auto *thread = QThread::create([this, task, label]() {
        QElapsedTimer timer;
        timer.start();
        {
            QMutexLocker lock(&m_edlIoMutex);
            task();
        }
        const qint64 elapsedMs = timer.elapsed();

        QMetaObject::invokeMethod(this, [this, label, elapsedMs]() {
            m_busy = false;
            setBusy(false);
            const QString t = formatElapsedMs(elapsedMs);
            const bool cancelled = isCancelRequested();
            const bool fail = m_asyncFail.load(std::memory_order_relaxed);

            if (cancelled) {
                const int p = qBound(0, m_lastProgressPctAtomic.load(std::memory_order_relaxed), 100);
                const double pctDisp = static_cast<double>(p);
                ui->progressBar->setValue(p);
                ui->progressBar->setFormat(QStringLiteral("%1%  ·  已取消").arg(pctDisp, 0, 'f', 1));
                ui->progressBar->setProperty("state", "cancelled");
                ui->progressBar->style()->unpolish(ui->progressBar);
                ui->progressBar->style()->polish(ui->progressBar);
                appendLog(QStringLiteral("──────── SAKURAEDL │ 操作已取消 │ %1 │ 耗时 %2 │ 可执行下一命令 ────────")
                              .arg(label, t),
                          QStringLiteral("#7D6B3A"));
            } else if (fail) {
                const int p = qBound(0, m_lastProgressPctAtomic.load(std::memory_order_relaxed), 100);
                const double pctDisp = static_cast<double>(p);
                ui->progressBar->setValue(p);
                ui->progressBar->setFormat(QStringLiteral("%1%  ·  失败").arg(pctDisp, 0, 'f', 1));
                ui->progressBar->setProperty("state", "error");
                ui->progressBar->style()->unpolish(ui->progressBar);
                ui->progressBar->style()->polish(ui->progressBar);
                appendLog(QStringLiteral("──────── SAKURAEDL │ 操作完成（失败）│ %1 │ 耗时 %2 │ 可执行下一命令 ────────")
                              .arg(label, t),
                          QStringLiteral("#C0392B"));
            } else {
                appendLog(QStringLiteral("──────── SAKURAEDL │ 操作完成 │ %1 │ 耗时 %2 │ 可执行下一命令 ────────")
                              .arg(label, t),
                          QStringLiteral("#2E8B3E"));
                ui->progressBar->setValue(100);
                ui->progressBar->setFormat(QStringLiteral("%1%  ·  完成").arg(100.0, 0, 'f', 1));
                ui->progressBar->setProperty("state", "success");
                ui->progressBar->style()->unpolish(ui->progressBar);
                ui->progressBar->style()->polish(ui->progressBar);
            }
            /* 任务线程已结束，立即释放串口，避免空闲等待用户时仍占用 COM；下次命令会重连 */
        }, Qt::QueuedConnection);
    });
    m_asyncWorkerThread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_asyncWorkerThread = nullptr;
    }, Qt::QueuedConnection);
    thread->start();
}

#endif
void MainWindow::runAsync(std::function<void()> task, const QString &operationLabel)
{
    if (m_busy) {
        appendLog(QStringLiteral("另一项任务仍在运行，请等待其完成后再开始新任务"), "#C0392B");
        return;
    }
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_asyncFail.store(false, std::memory_order_relaxed);
    m_lastProgressPctAtomic.store(0, std::memory_order_relaxed);
    m_busy = true;
    setBusy(true);

    m_progLastPctShown = 0;
    m_progDisplayMbps = -1.0;
    m_progOpStartMs.store(0, std::memory_order_relaxed);
    m_progLastReportedCurrent.store(-1, std::memory_order_relaxed);
    m_progLastReportedTotal.store(-1, std::memory_order_relaxed);
    m_progSpeedPrevMs.store(0, std::memory_order_relaxed);
    m_progSpeedPrevCurrent.store(0, std::memory_order_relaxed);
    ui->speedValueLabel->setText(QStringLiteral("-- MB/s"));
    ui->timeValueLabel->setText(QStringLiteral("耗时 00:00"));

    const QString label = operationLabel.isEmpty()
                              ? QStringLiteral("未命名任务")
                              : localizedLogAction(operationLabel);
    ui->logConsole->clear();
    appendLog(QStringLiteral("======== SAKURAEDL 任务开始 | %1 ========").arg(label), "#5B6B8C");

    QMetaObject::invokeMethod(
        this,
        [this]() {
            ui->progressBar->setValue(0);
            ui->progressBar->setFormat(QStringLiteral("%p%"));
            ui->progressBar->setProperty("state", "running");
            ui->progressBar->style()->unpolish(ui->progressBar);
            ui->progressBar->style()->polish(ui->progressBar);
        },
        Qt::QueuedConnection);

    auto *thread = QThread::create([this, task, label]() {
        QElapsedTimer timer;
        timer.start();
        {
            QMutexLocker lock(&m_edlIoMutex);
            task();
        }
        const qint64 elapsedMs = timer.elapsed();

        QMetaObject::invokeMethod(this, [this, label, elapsedMs]() {
            m_busy = false;
            setBusy(false);
            const QString elapsedText = formatElapsedMs(elapsedMs);
            const bool cancelled = isCancelRequested();
            const bool fail = m_asyncFail.load(std::memory_order_relaxed);

            if (cancelled) {
                const int p = qBound(0, m_lastProgressPctAtomic.load(std::memory_order_relaxed), 100);
                const double pctDisp = static_cast<double>(p);
                ui->progressBar->setValue(p);
                ui->progressBar->setFormat(QStringLiteral("%1%  |  已取消").arg(pctDisp, 0, 'f', 1));
                ui->progressBar->setProperty("state", "cancelled");
                ui->progressBar->style()->unpolish(ui->progressBar);
                ui->progressBar->style()->polish(ui->progressBar);
                appendLog(QStringLiteral("======== SAKURAEDL 已取消 | %1 | 耗时 %2 ========")
                              .arg(label, elapsedText),
                          "#7D6B3A");
            } else if (fail) {
                const int p = qBound(0, m_lastProgressPctAtomic.load(std::memory_order_relaxed), 100);
                const double pctDisp = static_cast<double>(p);
                ui->progressBar->setValue(p);
                ui->progressBar->setFormat(QStringLiteral("%1%  |  失败").arg(pctDisp, 0, 'f', 1));
                ui->progressBar->setProperty("state", "error");
                ui->progressBar->style()->unpolish(ui->progressBar);
                ui->progressBar->style()->polish(ui->progressBar);
                appendLog(QStringLiteral("======== SAKURAEDL 失败 | %1 | 耗时 %2 ========")
                              .arg(label, elapsedText),
                          "#C0392B");
            } else {
                appendLog(QStringLiteral("======== SAKURAEDL 完成 | %1 | 耗时 %2 ========")
                              .arg(label, elapsedText),
                          "#2E8B3E");
                ui->progressBar->setValue(100);
                ui->progressBar->setFormat(QStringLiteral("%1%  |  完成").arg(100.0, 0, 'f', 1));
                ui->progressBar->setProperty("state", "success");
                ui->progressBar->style()->unpolish(ui->progressBar);
                ui->progressBar->style()->polish(ui->progressBar);
            }
        }, Qt::QueuedConnection);
    });
    m_asyncWorkerThread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_asyncWorkerThread = nullptr;
    }, Qt::QueuedConnection);
    thread->start();
}

void MainWindow::setBusy(bool busy)
{
    ui->readInfoBtn->setEnabled(!busy);
    ui->readStorageDeviceInfoBtn->setEnabled(!busy);
    ui->readGptBtn->setEnabled(!busy);
    ui->writeGptXmlBtn->setEnabled(!busy);
    ui->removeFrpBtn->setEnabled(!busy);
    ui->factoryResetBtn->setEnabled(!busy);
    ui->rebootBtn->setEnabled(!busy);
    ui->activateBootSlotBtn->setEnabled(!busy);
    ui->readPartBtn->setEnabled(!busy);
    ui->writePartBtn->setEnabled(!busy);
    ui->erasePartBtn->setEnabled(!busy);
    ui->partitionSelectAllBtn->setEnabled(!busy);
    ui->partitionDeselectAllBtn->setEnabled(!busy);
    ui->setBootableAfterFlashCheckBox->setEnabled(!busy);
    ui->writeFileLengthOnlyCheckBox->setEnabled(!busy);
    ui->cancelButton->setEnabled(busy);
    if (!busy) {
        ui->cancelButton->setText(QStringLiteral("取消"));
        endBatchProgress();
        QTimer::singleShot(280, this, [this]() {
            if (!m_busy) {
                /* 成功/失败/取消时保持进度条终态，勿自动清零 */
                const QString st = ui->progressBar->property("state").toString();
                if (st == QLatin1String("success") || st == QLatin1String("error")
                    || st == QLatin1String("cancelled"))
                    return;
                ui->progressBar->setValue(0);
                ui->progressBar->setFormat(QStringLiteral("%p%"));
                ui->speedValueLabel->setText(QStringLiteral("-- MB/s"));
                ui->timeValueLabel->setText(QStringLiteral("已用 00:00"));
            }
        });
    }
}

namespace {

static QString firstNonEmptyCStr(const char *c, const QString &fb = QString())
{
    if (c && c[0])
        return QString::fromUtf8(c);
    return fb;
}

static QByteArray tryDecodeRealmeSignatureCandidate(const QString &s)
{
    QString c = s;
    c.remove(QRegularExpression(QStringLiteral("\\s")));
    if (c.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        c = c.mid(2);
    static const QRegularExpression hexRe(QStringLiteral("\\A[0-9a-fA-F]+\\z"));
    if (c.size() >= 256 && (c.size() % 2 == 0) && hexRe.match(c).hasMatch()) {
        QByteArray h = QByteArray::fromHex(c.toLatin1());
        if (h.size() >= 128)
            return h;
    }
    QByteArray b64 = QByteArray::fromBase64(c.toUtf8());
    if (b64.size() >= 128)
        return b64;
    return {};
}

static QByteArray findRealmeSignatureInJson(const QJsonValue &v)
{
    if (v.isString())
        return tryDecodeRealmeSignatureCandidate(v.toString());
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        static const char *keys[] = {"signature", "sign", "sig", "signData", "signatureData",
                                     "data", "result", "value", "content"};
        for (const char *k : keys) {
            const QString kk = QString::fromLatin1(k);
            if (o.contains(kk))
                return findRealmeSignatureInJson(o.value(kk));
        }
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            QByteArray r = findRealmeSignatureInJson(it.value());
            if (!r.isEmpty())
                return r;
        }
    }
    if (v.isArray()) {
        for (const QJsonValue &e : v.toArray()) {
            QByteArray r = findRealmeSignatureInJson(e);
            if (!r.isEmpty())
                return r;
        }
    }
    return {};
}

} // namespace

namespace {

/** Qt QNetworkReply::NetworkError，常见：4=TimeoutError（传输超时） */
static QString realmeNetworkErrorHint(QNetworkReply::NetworkError e)
{
    switch (e) {
    case QNetworkReply::NoError:
        return QStringLiteral("无错误");
    case QNetworkReply::TimeoutError:
        return QStringLiteral("传输超时（云端响应慢或网络差；已自动加长超时并重试）");
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("请求被中止（可能是用户取消或达到单次等待上限）");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("连接被拒绝（检查 API 地址/端口）");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("对端关闭连接");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("DNS 无法解析主机名");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("TLS/SSL 握手失败（代理/证书）");
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        return QStringLiteral("临时网络故障");
    default:
        return QStringLiteral("网络错误");
    }
}

static bool realmeSignNetworkRetryable(QNetworkReply::NetworkError e)
{
    switch (e) {
    case QNetworkReply::TimeoutError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::SslHandshakeFailedError:
        return true;
    default:
        return false;
    }
}

static bool realmeSignHttpRetryable(int httpStatus)
{
    switch (httpStatus) {
    case 408:
    case 409:
    case 425:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
        return true;
    default:
        return false;
    }
}

static QString realmeHttpErrorHint(int httpStatus)
{
    switch (httpStatus) {
    case 400:
        return QStringLiteral("请求参数无效");
    case 401:
        return QStringLiteral("未授权");
    case 403:
        return QStringLiteral("服务端拒绝访问");
    case 404:
        return QStringLiteral("签名接口不存在");
    case 408:
        return QStringLiteral("请求超时");
    case 409:
        return QStringLiteral("服务端状态冲突");
    case 425:
        return QStringLiteral("服务端要求稍后重试");
    case 429:
        return QStringLiteral("请求过于频繁");
    case 500:
        return QStringLiteral("服务端内部错误");
    case 502:
        return QStringLiteral("网关错误");
    case 503:
        return QStringLiteral("服务暂时不可用");
    case 504:
        return QStringLiteral("网关超时");
    default:
        return QStringLiteral("HTTP 异常");
    }
}

static bool realmeSignServiceRetryable(const QString &code, const QString &detail)
{
    if (code == QStringLiteral("408") || code == QStringLiteral("409")
        || code == QStringLiteral("425") || code == QStringLiteral("429")
        || code == QStringLiteral("500") || code == QStringLiteral("502")
        || code == QStringLiteral("503") || code == QStringLiteral("504")) {
        return true;
    }

    const QString text = (code + QLatin1Char(' ') + detail).toLower();
    return text.contains(QStringLiteral("timeout"))
        || text.contains(QStringLiteral("timed out"))
        || text.contains(QStringLiteral("tempor"))
        || text.contains(QStringLiteral("busy"))
        || text.contains(QStringLiteral("later"))
        || text.contains(QStringLiteral("rate"))
        || text.contains(QStringLiteral("gateway"))
        || text.contains(QStringLiteral("unavailable"))
        || text.contains(QStringLiteral("超时"))
        || text.contains(QStringLiteral("繁忙"))
        || text.contains(QStringLiteral("稍后"))
        || text.contains(QStringLiteral("频繁"))
        || text.contains(QStringLiteral("网关"))
        || text.contains(QStringLiteral("不可用"));
}

static QString realmeResponseExcerpt(const QByteArray &bytes, int maxBytes = 200)
{
    QString text = QString::fromUtf8(bytes.left(maxBytes)).trimmed();
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text;
}

static bool realmeSleepWithCancel(MainWindow *w, int totalMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < totalMs) {
        if (w->isCancelRequested())
            return false;
        const int remaining = totalMs - int(timer.elapsed());
        const int slice = qMin(100, qMax(remaining, 1));
        QThread::msleep(static_cast<unsigned long>(slice));
    }
    return true;
}

} // namespace

static bool mainwindow_realme_sign_bridge(const edl_realme_sign_material_t *material,
                                          uint8_t *signature_out, int *signature_len, void *user_data)
{
    auto *w = static_cast<MainWindow *>(user_data);
    return w->performRealmeCloudSign(material, signature_out, signature_len);
}

bool MainWindow::performRealmeCloudSign(const edl_realme_sign_material_t *material,
                                        uint8_t *signature_out, int *signature_len)
{
    if (!material || !signature_out || !signature_len)
        return false;
    if (isCancelRequested())
        return false;

    /* RCSMAUTH 内置默认；可用 QSettings realme/rcsmAuthAccount、realme/rcsmAuthKey 覆盖 */
    static const QString kDefRcsmAccount = QStringLiteral("RCSMAUTH");
    static const QString kDefRcsmAuthKey =
        QStringLiteral("fcd1eb18-dbe9-451b-87b7-c724a9d2fa78");

    QSettings s;
    QString apiUrl = s.value(QStringLiteral("realme/signApiUrl")).toString().trimmed();
    if (apiUrl.isEmpty())
        apiUrl = QStringLiteral("https://opluspro.top/api/sign/sign");

    QString projectNumber = s.value(QStringLiteral("realme/projectNumber")).toString().trimmed();
    if (projectNumber.isEmpty())
        projectNumber = QString::fromUtf8(material->requested_project);

    QString rcsmAcc = s.value(QStringLiteral("realme/rcsmAuthAccount")).toString().trimmed();
    QString rcsmKey = s.value(QStringLiteral("realme/rcsmAuthKey")).toString().trimmed();
    if (rcsmAcc.isEmpty())
        rcsmAcc = kDefRcsmAccount;
    if (rcsmKey.isEmpty())
        rcsmKey = kDefRcsmAuthKey;
    const bool useRcsm = true;
    QString diskId = s.value(QStringLiteral("realme/diskId")).toString();
    if (diskId.isEmpty())
        diskId = QStringLiteral(
            "E823_8FA6_BF53_0001_001B_448B_4D70_96A9.E823_8FA6_BF53_0001_001B_448B_4DD5_F913.");

    QString nvPlatform = s.value(QStringLiteral("realme/nvPlatform")).toString();
    QString nvCode = s.value(QStringLiteral("realme/nvCode")).toString();
    if (nvCode.isEmpty())
        nvCode = QStringLiteral("00000000");

    if (projectNumber.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            appendLog("【认证】Realme：请填写主界面「项目号」（必填），或由设备回传 ProjectID 写入配置",
                      "#C0392B");
        }, Qt::QueuedConnection);
        return false;
    }

    QString subPlatform = firstNonEmptyCStr(material->platform2);
    if (subPlatform.isEmpty())
        subPlatform = firstNonEmptyCStr(material->platform1, QStringLiteral("QCOM"));
    if (nvPlatform.isEmpty())
        nvPlatform = subPlatform;

    QString newSw = firstNonEmptyCStr(material->digest_write);
    if (newSw.isEmpty())
        newSw = QString::fromUtf8(material->digest_read);
    QString oldSw = firstNonEmptyCStr(material->digest_read);
    if (oldSw.isEmpty())
        oldSw = QString::fromUtf8(material->digest_write);

    QJsonObject o;
    o[QStringLiteral("account")] = rcsmAcc;
    o[QStringLiteral("agreementVer")] = QString();
    o[QStringLiteral("chipSn")] = QString::fromUtf8(material->chip_sn);
    o[QStringLiteral("daVer")] = QString();
    o[QStringLiteral("deviceType")] = QStringLiteral("1");
    o[QStringLiteral("diskId")] = diskId;
    o[QStringLiteral("extIp")] = QString();
    o[QStringLiteral("lockVer")] = QStringLiteral("1");
    o[QStringLiteral("loginType")] = QStringLiteral("1");
    o[QStringLiteral("mac")] = QString();
    o[QStringLiteral("mainPlatform")] = firstNonEmptyCStr(material->platform1, QStringLiteral("QCOM"));
    o[QStringLiteral("metaVer")] = QStringLiteral("0");
    o[QStringLiteral("newProjectNo")] = firstNonEmptyCStr(material->project_write, QStringLiteral("00000"));
    o[QStringLiteral("newRemake")] = QStringLiteral("???");
    o[QStringLiteral("newSwNameSign")] = newSw;
    o[QStringLiteral("nvCheck")] = material->nv_check;
    o[QStringLiteral("oldProjectNo")] = firstNonEmptyCStr(material->project_read, o[QStringLiteral("newProjectNo")].toString());
    o[QStringLiteral("oldSwNameSign")] = oldSw;
    o[QStringLiteral("plVer")] = QString();
    o[QStringLiteral("projectNumber")] = projectNumber;
    o[QStringLiteral("randomNum")] = QString::fromUtf8(material->rand);
    o[QStringLiteral("readWriteMode")] = QString::fromUtf8(material->mode);
    o[QStringLiteral("subPlatform")] = subPlatform;
    o[QStringLiteral("toolDeviceId")] = s.value(QStringLiteral("realme/toolDeviceId")).toString();
    o[QStringLiteral("toolHash")] = s.value(QStringLiteral("realme/toolHash")).toString();
    o[QStringLiteral("toolVersion")] = s.value(QStringLiteral("realme/toolVersion")).toString();
    o[QStringLiteral("version")] = firstNonEmptyCStr(material->version, QStringLiteral("0"));
    o[QStringLiteral("workerOrder")] = QString();
    o[QStringLiteral("nvPlatForm")] = nvPlatform;
    o[QStringLiteral("nvCode")] = firstNonEmptyCStr(material->nv_code, nvCode);
    if (useRcsm) {
        o[QStringLiteral("rcsmAuthAccount")] = rcsmAcc;
        o[QStringLiteral("rcsmAuthKey")] = rcsmKey;
    }

    const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);

    /* 云端签名需要允许慢响应，但取消必须立即生效；因此采用较长传输超时 + 更短的轮询/外层上限。 */
    constexpr int kRealmeSignMaxAttempts   = 3;
    constexpr int kRealmeTransferTimeoutMs = 75000;
    constexpr int kRealmeAttemptCapMs      = 80000;
    constexpr int kRealmeRetryBackoffMs    = 800;
    constexpr int kRealmeCancelPollMs      = 150;
    constexpr int kRealmeAbortSettleMs     = 600;

    QByteArray respBytes;
    int httpStatus = 0;
    QNetworkReply::NetworkError netErr = QNetworkReply::NoError;
    QString errDetail;

    auto queueLog = [this](const QString &msg, const QString &color) {
        QMetaObject::invokeMethod(this, [this, msg, color]() {
            appendLog(msg, color);
        }, Qt::QueuedConnection);
    };
    auto queueRetry = [&, this](int nextAttempt, const QString &reason) {
        queueLog(QStringLiteral("【认证】Realme 云端签名准备重试（第 %1/%2 次）：%3")
                     .arg(nextAttempt)
                     .arg(kRealmeSignMaxAttempts)
                     .arg(reason),
                 QStringLiteral("#6B7280"));
    };

    QNetworkAccessManager nam;
    for (int attempt = 0; attempt < kRealmeSignMaxAttempts; ++attempt) {
        if (isCancelRequested())
            return false;

        respBytes.clear();
        httpStatus = 0;
        netErr = QNetworkReply::NoError;
        errDetail.clear();

        QNetworkRequest req{QUrl(apiUrl)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("User-Agent",
                         QStringLiteral("SAKURAEDL/%1 (RealmeSign)")
                             .arg(QCoreApplication::applicationVersion())
                             .toUtf8());
        /* 与浏览器常见行为一致：对 opluspro 等签名接口强制 HTTP/1.1（禁用 HTTP/2） */
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        req.setTransferTimeout(kRealmeTransferTimeoutMs);

        QNetworkReply *reply = nam.post(req, body);

        QEventLoop loop;
        QTimer timer;
        QTimer cancelTimer;
        bool userCancelled = false;
        bool hitLoopCap = false;
        timer.setSingleShot(true);
        cancelTimer.setInterval(kRealmeCancelPollMs);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
            hitLoopCap = true;
            if (reply->isRunning())
                reply->abort();
            loop.quit();
        });
        QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&]() {
            if (!userCancelled && isCancelRequested()) {
                userCancelled = true;
                if (reply->isRunning())
                    reply->abort();
                loop.quit();
            }
        });
        timer.start(kRealmeAttemptCapMs);
        cancelTimer.start();
        loop.exec();
        cancelTimer.stop();
        timer.stop();

        if (reply->isRunning()) {
            reply->abort();
            QEventLoop settleLoop;
            QTimer settleTimer;
            settleTimer.setSingleShot(true);
            QObject::connect(reply, &QNetworkReply::finished, &settleLoop, &QEventLoop::quit);
            QObject::connect(&settleTimer, &QTimer::timeout, &settleLoop, &QEventLoop::quit);
            settleTimer.start(kRealmeAbortSettleMs);
            settleLoop.exec();
        }

        respBytes = reply->readAll();
        httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        netErr = reply->error();
        errDetail = reply->errorString();
        reply->deleteLater();

        if (userCancelled || isCancelRequested())
            return false;

        if (hitLoopCap && netErr == QNetworkReply::OperationCanceledError) {
            netErr = QNetworkReply::TimeoutError;
            errDetail = QStringLiteral("达到单次请求等待上限（%1 秒）").arg(kRealmeAttemptCapMs / 1000);
        }

        if (netErr != QNetworkReply::NoError && httpStatus == 0 && respBytes.isEmpty()) {
            const QString reason = QStringLiteral("%1 | %2")
                                       .arg(realmeNetworkErrorHint(netErr))
                                       .arg(errDetail);
            if (attempt + 1 < kRealmeSignMaxAttempts && realmeSignNetworkRetryable(netErr)) {
                queueRetry(attempt + 2, reason);
                if (!realmeSleepWithCancel(this, kRealmeRetryBackoffMs))
                    return false;
                continue;
            }
            queueLog(QStringLiteral("【认证】Realme 网络错误：%1").arg(reason), QStringLiteral("#C0392B"));
            return false;
        }

        if (httpStatus != 0 && (httpStatus < 200 || httpStatus >= 300)) {
            const QString excerpt = realmeResponseExcerpt(respBytes);
            const QString reason = excerpt.isEmpty()
                ? QStringLiteral("HTTP %1：%2").arg(httpStatus).arg(realmeHttpErrorHint(httpStatus))
                : QStringLiteral("HTTP %1：%2 | %3")
                      .arg(httpStatus)
                      .arg(realmeHttpErrorHint(httpStatus))
                      .arg(excerpt);
            if (attempt + 1 < kRealmeSignMaxAttempts && realmeSignHttpRetryable(httpStatus)) {
                queueRetry(attempt + 2, reason);
                if (!realmeSleepWithCancel(this, kRealmeRetryBackoffMs))
                    return false;
                continue;
            }
            queueLog(QStringLiteral("【认证】Realme %1").arg(reason), QStringLiteral("#C0392B"));
            return false;
        }

        QJsonParseError pe{};
        const QJsonDocument rd = QJsonDocument::fromJson(respBytes, &pe);
        if (rd.isObject()) {
            const QJsonObject rob = rd.object();
            const QJsonValue codeValue = rob.value(QStringLiteral("code"));
            QString code;
            if (codeValue.isString())
                code = codeValue.toString().trimmed();
            else if (codeValue.isDouble())
                code = QString::number(qRound64(codeValue.toDouble()));

            if (!code.isEmpty() && code != QStringLiteral("0") && code != QStringLiteral("000000")) {
                const QString msg = rob.value(QStringLiteral("msg")).toString().trimmed();
                const QString hint = rob.value(QStringLiteral("ai_hint")).toString().trimmed();
                QString detail = !hint.isEmpty() ? hint : msg;
                if (detail.isEmpty())
                    detail = realmeResponseExcerpt(respBytes);
                if (attempt + 1 < kRealmeSignMaxAttempts && realmeSignServiceRetryable(code, detail)) {
                    queueRetry(attempt + 2,
                               QStringLiteral("服务端暂时不可用 [%1]：%2").arg(code).arg(detail));
                    if (!realmeSleepWithCancel(this, kRealmeRetryBackoffMs))
                        return false;
                    continue;
                }
                queueLog(QStringLiteral("【认证】Realme 服务端错误 [%1]：%2")
                             .arg(code)
                             .arg(detail.isEmpty() ? QStringLiteral("未知错误") : detail),
                         QStringLiteral("#C0392B"));
                return false;
            }
        }

        QByteArray sig = findRealmeSignatureInJson(rd.isObject() ? QJsonValue(rd.object()) : QJsonValue());
        if (sig.isEmpty()) {
            const QString t = QString::fromUtf8(respBytes).trimmed();
            if (!t.startsWith(QLatin1Char('{')))
                sig = tryDecodeRealmeSignatureCandidate(t);
        }
        if (sig.isEmpty()) {
            const QString excerpt = realmeResponseExcerpt(respBytes, 240);
            const bool maybeTransientBody =
                respBytes.trimmed().isEmpty() || realmeSignServiceRetryable(QString(), excerpt);
            if (attempt + 1 < kRealmeSignMaxAttempts
                && (realmeSignNetworkRetryable(netErr) || maybeTransientBody)) {
                queueRetry(attempt + 2,
                           excerpt.isEmpty() ? QStringLiteral("响应为空或尚未返回签名")
                                             : QStringLiteral("响应未返回有效签名：%1").arg(excerpt));
                if (!realmeSleepWithCancel(this, kRealmeRetryBackoffMs))
                    return false;
                continue;
            }
            queueLog(excerpt.isEmpty()
                         ? QStringLiteral("【认证】Realme：响应中未解析到签名数据")
                         : QStringLiteral("【认证】Realme：响应中未解析到签名数据 | %1").arg(excerpt),
                     QStringLiteral("#C0392B"));
            return false;
        }

        /* realme_auth 侧缓冲区 4096 字节 */
        if (sig.size() > 4096) {
            queueLog(QStringLiteral("【认证】Realme：签名数据超出 4096 字节缓冲区"),
                     QStringLiteral("#C0392B"));
            return false;
        }
        memcpy(signature_out, sig.constData(), size_t(sig.size()));
        *signature_len = sig.size();
        return true;
    }

    return false;
}

bool MainWindow::ensureConnected(bool syncStorageInfoOnConnect)
{
    if (m_cancelRequested.load(std::memory_order_relaxed))
        return false;
    if (edl_service_is_connected(m_edlService))
        return true;

    QString loaderPath;
    QString storageStr;
    QString digestPath;
    QString sigPath;
    QString realmeProject;
    bool useRealme = false;
    bool useOplus = false;
    bool useOneplus = false;
    bool useXiaomi = false;
    bool readDeviceInfo = false;
    QMetaObject::invokeMethod(this, [this, &loaderPath, &storageStr, &digestPath, &sigPath, &realmeProject,
                                    &useRealme, &useOplus, &useOneplus, &useXiaomi, &readDeviceInfo]() {
        loaderPath = ui->firehoseLineEdit->text();
        int idx = ui->storageCombo->currentIndex();
        if (idx == 1) storageStr = "emmc";
        else if (idx == 2) storageStr = "ufs";
        digestPath = ui->digestLineEdit->text();
        sigPath = ui->signatureLineEdit->text();
        realmeProject = ui->realmeProjectNumberLineEdit->text().trimmed();
        useRealme = ui->realmeAuthCheckBox->isChecked();
        useOplus = ui->oplusAuthCheckBox->isChecked();
        useOneplus = ui->oneplusAuthCheckBox->isChecked();
        useXiaomi = ui->xiaomiAuthCheckBox->isChecked();
        readDeviceInfo = ui->readSysInfoCheckBox->isChecked();
    }, Qt::BlockingQueuedConnection);

    #if 0
    if (loaderPath.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            appendLog("请先选择 Firehose 加载器", "#C0392B");
        }, Qt::QueuedConnection);
        markAsyncTaskFailed();
        return false;
    }

    }
    #endif
    if (loaderPath.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            appendLog(QStringLiteral("请先选择 Firehose 加载器"), "#C0392B");
        }, Qt::QueuedConnection);
        markAsyncTaskFailed();
        return false;
    }

    #if 0
    if (useRealme && realmeProject.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            appendLog("【认证】Realme：请填写主界面「项目号」（必填）", "#C0392B");
        }, Qt::QueuedConnection);
        markAsyncTaskFailed();
        return false;
    }

    }
    #endif
    if (useRealme && realmeProject.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            appendLog(QStringLiteral("【认证】Realme：必须填写项目号"), "#C0392B");
        }, Qt::QueuedConnection);
        markAsyncTaskFailed();
        return false;
    }

    edl_svc_auth_options_t auth;
    memset(&auth, 0, sizeof(auth));
    const edl_svc_auth_options_t *authPtr = nullptr;
    auto copyPath = [](char *dst, size_t dstSize, const QString &src) {
        if (dstSize == 0) return;
        dst[0] = '\0';
        if (src.isEmpty()) return;
        QByteArray b = src.toUtf8();
        size_t n = (size_t)b.size();
        if (n >= dstSize) n = dstSize - 1;
        memcpy(dst, b.constData(), n);
        dst[n] = '\0';
    };

    if (useOplus) {
        auth.mode = EDL_SVC_AUTH_OPLUS_VIP;
        copyPath(auth.digest_path, sizeof(auth.digest_path), digestPath);
        copyPath(auth.signature_path, sizeof(auth.signature_path), sigPath);
        authPtr = &auth;
    } else if (useRealme) {
        auth.mode = EDL_SVC_AUTH_REALME;
        copyPath(auth.digest_path, sizeof(auth.digest_path), digestPath);
        {
            QSettings rs;
            rs.setValue(QStringLiteral("realme/projectNumber"), realmeProject);
            copyPath(auth.project_id, sizeof(auth.project_id), realmeProject);
        }
        auth.realme_sign_cb = mainwindow_realme_sign_bridge;
        auth.realme_sign_user = this;
        authPtr = &auth;
    } else if (useOneplus) {
        auth.mode = EDL_SVC_AUTH_ONEPLUS;
        authPtr = &auth;
    } else if (useXiaomi) {
        auth.mode = EDL_SVC_AUTH_XIAOMI;
        authPtr = &auth;
    }

    edl_error_t err = edl_service_connect_ex(
        m_edlService,
        loaderPath.toUtf8().constData(),
        storageStr.isEmpty() ? nullptr : storageStr.toUtf8().constData(),
        authPtr);

    if (err == EDL_OK) {
        m_trackedComPort = QString::fromUtf8(edl_service_port_name(m_edlService));
        m_portWatchPingPhase = 0;
        /*
         * 连接后仅 GetStorageInfo 对齐 UI，勿在此 probe build.prop：
         * probe 会 svc_ensure_gpt，UFS 上常在认证后首扫失败并刷屏；用户点「读取信息」时
         * ensureGptCacheReady 先读 GPT 再 probe，一条路径即可。
         */
        if (syncStorageInfoOnConnect && readDeviceInfo) {
            edl_error_t ge = edl_service_get_storage_info(m_edlService);
            #if 0
            QMetaObject::invokeMethod(this, [this, ge]() {
                applyDeviceStorageToUi(ge == EDL_OK);
                if (ge != EDL_OK)
                    logEdlResult(QStringLiteral("GetStorageInfo（连接后同步）"), ge);
            }, Qt::QueuedConnection);
            #endif
            QMetaObject::invokeMethod(this, [this, ge]() {
                applyDeviceStorageToUi(ge == EDL_OK);
                if (ge != EDL_OK)
                    logEdlResult(QStringLiteral("连接后同步存储信息"), ge);
            }, Qt::QueuedConnection);
        }
    }

    if (err != EDL_OK)
        markAsyncTaskFailed();
    return err == EDL_OK;
}

void MainWindow::appendAndroidBuildPropsLog(const edl_android_props_t &ap)
{
    if (!ap.source_partition[0])
        return;
    appendSectionLog(QStringLiteral("Android 系统信息"));
    auto one = [this](const QString &label, const QString &value) {
        if (!value.trimmed().isEmpty())
            appendLog(QStringLiteral("%1 : %2")
                          .arg(label.leftJustified(14, QLatin1Char(' ')))
                          .arg(value),
                      "#6B7280");
    };
    const auto utf8 = [](const char *v) { return QString::fromUtf8(v ? v : ""); };

    QString userdataSize;
    if (m_edlService) {
        const edl_partition_info_t *userdata = edl_service_find_partition(m_edlService, "userdata");
        if (!userdata)
            userdata = edl_service_find_partition(m_edlService, "USERDATA");
        if (userdata && userdata->num_sectors > 0)
            userdataSize = formatSize(userdata->num_sectors, edl_service_sector_size(m_edlService));
    }

    one(QStringLiteral("用户数据大小"), userdataSize);
    one(QStringLiteral("厂商"), utf8(ap.manufacturer));
    one(QStringLiteral("品牌"), utf8(ap.brand));
    one(QStringLiteral("机型"), utf8(ap.market_name));
    const QString locale = utf8(ap.locale);
    const QString regionMark = utf8(ap.region_mark);
    const QString regionType = utf8(ap.region_type);
    QString countryRegion = regionMark.trimmed();
    if (countryRegion.isEmpty() && !locale.trimmed().isEmpty()) {
        QString norm = locale.trimmed();
        norm.replace(QLatin1Char('_'), QLatin1Char('-'));
        const QStringList parts = norm.split(QLatin1Char('-'), Qt::SkipEmptyParts);
        if (parts.size() >= 2)
            countryRegion = parts.last().trimmed();
    }
    if (countryRegion.isEmpty() && !regionType.trimmed().isEmpty()) {
        const int sep = regionType.indexOf(QLatin1Char('_'));
        countryRegion = (sep > 0 ? regionType.left(sep) : regionType).trimmed();
    }
    QString regionMarkDisplay = regionMark.trimmed();
    if (regionMarkDisplay.isEmpty())
        regionMarkDisplay = countryRegion;

    auto normalizedLocale = [](QString value) {
        value = value.trimmed();
        if (value.isEmpty())
            return value;
        value.replace(QLatin1Char('_'), QLatin1Char('-'));
        const QStringList parts = value.split(QLatin1Char('-'), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            return value;
        QString language = parts.first().toLower();
        if (parts.size() == 1)
            return language;
        QString region = parts.at(1).toUpper();
        return language + QLatin1Char('-') + region;
    };

    auto inferredLocaleFromRegion = [](const QString &regionValue) {
        const QString code = regionValue.trimmed().toUpper();
        if (code.isEmpty())
            return QString();
        if (code == QLatin1String("CN"))
            return QStringLiteral("zh-CN");
        if (code == QLatin1String("TW"))
            return QStringLiteral("zh-TW");
        if (code == QLatin1String("HK"))
            return QStringLiteral("zh-HK");
        if (code == QLatin1String("MO"))
            return QStringLiteral("zh-MO");
        if (code == QLatin1String("JP"))
            return QStringLiteral("ja-JP");
        if (code == QLatin1String("KR"))
            return QStringLiteral("ko-KR");
        if (code == QLatin1String("RU"))
            return QStringLiteral("ru-RU");
        if (code == QLatin1String("TH"))
            return QStringLiteral("th-TH");
        if (code == QLatin1String("VN"))
            return QStringLiteral("vi-VN");
        if (code == QLatin1String("ID"))
            return QStringLiteral("id-ID");
        if (code == QLatin1String("MY"))
            return QStringLiteral("ms-MY");
        if (code == QLatin1String("IN"))
            return QStringLiteral("en-IN");
        if (code == QLatin1String("PH"))
            return QStringLiteral("en-PH");
        if (code == QLatin1String("EUEX") || code == QLatin1String("EEA") || code == QLatin1String("EU"))
            return QStringLiteral("en-GB");
        if (code.size() == 2)
            return QStringLiteral("en-%1").arg(code);
        return QString();
    };

    QString localeDisplay = normalizedLocale(locale);
    if (localeDisplay.isEmpty())
        localeDisplay = inferredLocaleFromRegion(countryRegion);

    QString displayVersion = utf8(ap.display_ota).trimmed();
    const QString modelText = utf8(ap.model).trimmed();
    const QString productText = utf8(ap.product).trimmed();
    if (displayVersion.isEmpty()
        || (!modelText.isEmpty() && displayVersion.compare(modelText, Qt::CaseInsensitive) == 0)
        || (!productText.isEmpty() && displayVersion.compare(productText, Qt::CaseInsensitive) == 0)) {
        const QString displayOta = utf8(ap.display_ota).trimmed();
        const QString displayId = utf8(ap.display_id).trimmed();
        const QString displayFull = utf8(ap.display_full_id).trimmed();
        if (!displayOta.isEmpty())
            displayVersion = displayOta;
        else if (!displayId.isEmpty())
            displayVersion = displayId;
        else if (!displayFull.isEmpty())
            displayVersion = displayFull;
    }
    one(QStringLiteral("语言地区"), localeDisplay);
    one(QStringLiteral("国家/地区"), countryRegion);
    one(QStringLiteral("区域标记"), regionMarkDisplay);
    one(QStringLiteral("区域类型"), regionType);
    one(QStringLiteral("型号"), utf8(ap.model));
    one(QStringLiteral("产品"), utf8(ap.product));
    one(QStringLiteral("设备代号"), utf8(ap.device));
    one(QStringLiteral("Android 版本"), utf8(ap.android_release));
    one(QStringLiteral("OTA 版本"), utf8(ap.ota_version));
    one(QStringLiteral("OTA 显示版本"), utf8(ap.display_ota));
    one(QStringLiteral("完整显示版本"), utf8(ap.display_full_id));
    one(QStringLiteral("OTA 机型代号"), utf8(ap.common_ota));
    if (ap.miui_version[0])
        one(QStringLiteral("MIUI 版本"), utf8(ap.miui_version));
    one(QStringLiteral("构建 ID"), utf8(ap.build_id));
    one(QStringLiteral("增量版本"), utf8(ap.incremental));
    one(QStringLiteral("显示版本"), displayVersion);
    one(QStringLiteral("指纹"), utf8(ap.fingerprint));
    one(QStringLiteral("安全补丁"), utf8(ap.security_patch));
    one(QStringLiteral("构建日期"), utf8(ap.build_date));
    one(QStringLiteral("构建日期 UTC"), utf8(ap.build_date_utc));
    one(QStringLiteral("构建类型"), utf8(ap.build_type));
    one(QStringLiteral("构建标签"), utf8(ap.build_tags));
    one(QStringLiteral("SDK 版本"), utf8(ap.sdk));
    one(QStringLiteral("项目号"), utf8(ap.project_number));
    one(QStringLiteral("认证项目号"), utf8(ap.auth_project));
    one(QStringLiteral("硬件代号"), utf8(ap.hardware_code));
    one(QStringLiteral("NV ID"), utf8(ap.nv_id));
    one(QStringLiteral("渠道代号"), utf8(ap.pipeline_key));
    one(QStringLiteral("基线版本"), utf8(ap.base_version));

    QString source = utf8(ap.source_partition);
    QString fsType = utf8(ap.fs_type);
    if (!source.isEmpty() || !fsType.isEmpty()) {
        QString sourceText = source;
        if (!fsType.isEmpty())
            sourceText += sourceText.isEmpty() ? fsType : QStringLiteral(" (%1)").arg(fsType);
        if (ap.fs_embed_offset != 0)
            sourceText += QStringLiteral(" @ 0x%1").arg(QString::number(ap.fs_embed_offset, 16).toUpper());
        one(QStringLiteral("信息来源"), sourceText);
    }
}

void MainWindow::displayChipInfo()
{
    const edl_chip_info_t *chip = edl_service_chip_info(m_edlService);
    if (!chip) return;

    edl_platform_profile_t profile = {};
    const bool haveProfile = edl_query_platform_profile(chip->msm_id,
                                                        chip->oem_id,
                                                        chip->model_id,
                                                        chip->pk_hash,
                                                        &profile);
    const QString chipNameRaw = QString::fromUtf8(chip->chip_name);
    const QString chipName = chipNameRaw == QLatin1String("Unknown")
                                 ? QStringLiteral("未知")
                                 : chipNameRaw;
    const QString platformCode = haveProfile && profile.soc_code[0]
        ? QString::fromUtf8(profile.soc_code)
        : QString::fromUtf8(edl_chip_codename(chip->msm_id) ? edl_chip_codename(chip->msm_id) : "");
    const QString codename = haveProfile && profile.precise_codename[0]
        ? QString::fromUtf8(profile.precise_codename)
        : haveProfile && profile.codename[0]
            ? QString::fromUtf8(profile.codename)
            : QString::fromUtf8(edl_chip_codename_precise(chip->msm_id)
                                ? edl_chip_codename_precise(chip->msm_id) : "");
    const QString serial = QString::fromUtf8(chip->serial_hex);
    edl_brand_source_t brandSource = EDL_BRAND_SOURCE_NONE;
    const char *brandC = edl_brand_by_ids_ex(chip->oem_id, chip->model_id, chip->pk_hash, &brandSource);
    const char *oemVendorC = edl_vendor_by_oem(chip->oem_id);
    const QString brand = QString::fromUtf8(brandC ? brandC : (chip->vendor[0] ? chip->vendor : ""));
    const QString brandSourceText = QString::fromUtf8(edl_brand_source_name(brandSource));
    const QString oemVendor = QString::fromUtf8(oemVendorC ? oemVendorC : "");
    const QString hwid = QString::fromUtf8(chip->hwid_hex);
    const QString pkHash = QString::fromUtf8(chip->pk_hash);
    const uint16_t oemId = chip->oem_id;
    const uint16_t modelId = chip->model_id;
    const uint32_t msmId = chip->msm_id;
    const uint32_t sahVer = chip->sahara_version;
    const QString sessionMode = QString::fromUtf8(edl_service_session_mode(m_edlService));
    const edl_loader_info_t *loader = edl_service_loader_info(m_edlService);
    const bool showPreciseBrand = !brand.isEmpty()
                                  && brandSource != EDL_BRAND_SOURCE_OEM_FAMILY
                                  && brand != oemVendor;
    QString marketingName = haveProfile && profile.marketing_name[0]
        ? QString::fromUtf8(profile.marketing_name)
        : chipName;
    if (marketingName.isEmpty())
        marketingName = chipName;
    const QString loaderHint = haveProfile
        ? QString::fromUtf8(edl_loader_arch_hint_name(profile.loader_arch_hint))
        : QString();
    const QString authHint = haveProfile
        ? QString::fromUtf8(edl_auth_hint_name(profile.auth_hint))
        : QString();

    auto loaderClassText = [](const edl_loader_info_t *li) -> QString {
        if (!li || !li->valid)
            return QString();
        if (!li->is_elf)
            return QStringLiteral("原始/未知格式");
        if (li->elf_class == 2)
            return QStringLiteral("ELF64");
        if (li->elf_class == 1)
            return QStringLiteral("ELF32");
        return QStringLiteral("ELF");
    };
    auto loaderMachineText = [](const edl_loader_info_t *li) -> QString {
        if (!li || !li->valid)
            return QString();
        if (li->machine == 183)
            return QStringLiteral("ARM64");
        if (li->machine == 40)
            return QStringLiteral("ARM");
        return QStringLiteral("未知架构");
    };
    auto loaderEndianText = [](const edl_loader_info_t *li) -> QString {
        if (!li || !li->valid)
            return QString();
        if (li->elf_data == 1)
            return QStringLiteral("小端");
        if (li->elf_data == 2)
            return QStringLiteral("大端");
        return QStringLiteral("未知端序");
    };
    QString loaderSummary;
    QString loaderVersionText;
    if (loader && loader->valid) {
        if (loader->is_elf) {
            const QString bits = loader->elf_class == 2 ? QStringLiteral("64 位")
                                  : loader->elf_class == 1 ? QStringLiteral("32 位")
                                                           : QString();
            QStringList parts{loaderClassText(loader), loaderMachineText(loader)};
            if (!bits.isEmpty())
                parts << bits;
            const QString endian = loaderEndianText(loader);
            if (!endian.isEmpty())
                parts << endian;
            loaderSummary = parts.join(QStringLiteral(" | "));
        } else {
            loaderSummary = QStringLiteral("原始/未知格式");
        }

        QStringList versionParts;
        if (loader->qc_version[0])
            versionParts << QStringLiteral("QC %1").arg(QString::fromUtf8(loader->qc_version));
        if (loader->oem_version[0])
            versionParts << QStringLiteral("OEM %1").arg(QString::fromUtf8(loader->oem_version));
        if (loader->variant[0])
            versionParts << QStringLiteral("变体 %1").arg(QString::fromUtf8(loader->variant));
        loaderVersionText = versionParts.join(QStringLiteral(" | "));
    }

    QMetaObject::invokeMethod(this, [=]() {
        appendSectionLog(QStringLiteral("芯片与引导信息"));
        appendLog(QString("当前会话模式    : %1")
                      .arg(sessionMode.isEmpty() ? QStringLiteral("未知") : sessionMode),
                  "#5B6B8C");
        appendLog(QStringLiteral("芯片信息来源    : Sahara 握手"), "#6B7280");
        appendLog(QString("CPU 识别        : %1")
                      .arg(marketingName.isEmpty() ? QStringLiteral("未知") : marketingName),
                  "#2E8B3E");
        if (!platformCode.isEmpty())
            appendLog(QString("平台代号        : %1").arg(platformCode), "#2E8B3E");
        appendLog(QString("芯片代号        : %1")
                      .arg(codename.isEmpty() ? QStringLiteral("(库中暂无精确代号)") : codename),
                  "#2E8B3E");
        if (!loaderSummary.isEmpty())
            appendLog(QString("Firehose 加载器  : %1").arg(loaderSummary), "#6B7280");
        if (!loaderVersionText.isEmpty())
            appendLog(QString("加载器版本      : %1").arg(loaderVersionText), "#6B7280");
        if (!loaderHint.isEmpty() && loaderHint != QStringLiteral("unknown loader"))
            appendLog(QString("加载器建议      : %1").arg(loaderHint), "#6B7280");
        if (!authHint.isEmpty() && authHint != QStringLiteral("未知"))
            appendLog(QString("认证策略        : %1").arg(authHint), "#6B7280");
        appendLog(QString("Sahara 版本     : %1").arg(sahVer), "#6B7280");
        appendLog(QString("硬件 ID         : %1").arg(hwid.isEmpty() ? QStringLiteral("(未知)") : hwid),
                  "#6B7280");
        appendLog(QString("MSM ID          : 0x%1").arg(msmId, 8, 16, QChar('0')),
                  "#6B7280");
        if (!oemVendor.isEmpty()) {
            appendLog(QString("OEM ID          : 0x%1 [%2]")
                          .arg(oemId, 4, 16, QChar('0'))
                          .arg(oemVendor),
                      "#6B7280");
        } else {
            appendLog(QString("OEM ID          : 0x%1").arg(oemId, 4, 16, QChar('0')),
                      "#6B7280");
        }
        appendLog(QString("机型 ID         : 0x%1").arg(modelId, 4, 16, QChar('0')),
                  "#6B7280");
        if (showPreciseBrand)
            appendLog(QString("品牌            : %1 (%2)").arg(brand, brandSourceText), "#2E8B3E");
        for (int i = 0, off = 0; off < pkHash.size(); off += 32, ++i) {
            const QString chunk = pkHash.mid(off, 32);
            if (!chunk.isEmpty())
                appendLog(QString("PK_HASH[%1]        : 0x%2").arg(i).arg(chunk), "#6B7280");
        }
        appendLog(QString("序列号          : %1").arg(serial.isEmpty() ? QStringLiteral("(未知)") : serial),
                  "#6B7280");
    }, Qt::QueuedConnection);
}

void MainWindow::populatePartitionTable(const edl_partition_info_t *parts, int count)
{
    auto *table = ui->partitionTable;
    table->setRowCount(count);
    for (int i = 0; i < count; ++i) {
        const auto &p = parts[i];
        auto *nameItem = new QTableWidgetItem(QString::fromUtf8(p.name));
        nameItem->setFlags(nameItem->flags() | Qt::ItemIsUserCheckable);
        nameItem->setCheckState(Qt::Unchecked);
        nameItem->setData(RoleStartSector, QVariant::fromValue<qint64>(p.start_sector));
        nameItem->setData(RoleNumSectors, QVariant::fromValue<qint64>(p.num_sectors));
        nameItem->setData(RoleSectorSize, p.sector_size);
        nameItem->setData(RoleLun, p.lun);
        nameItem->setData(RoleStartSectorExpr, QString::fromUtf8(p.start_sector_expr));
        table->setItem(i, ColName, nameItem);
        table->setItem(i, ColStartSector, new QTableWidgetItem(QString::number(p.start_sector)));
        table->setItem(i, ColSectorCount, new QTableWidgetItem(QString::number(p.num_sectors)));
        table->setItem(i, ColSize, new QTableWidgetItem(formatSize(p.num_sectors, p.sector_size)));
        table->setItem(i, ColLun, new QTableWidgetItem(QString::number(p.lun)));
        auto *fileItem = new QTableWidgetItem();
        fileItem->setForeground(QColor("#9CA3AF"));
        table->setItem(i, ColFile, fileItem);
    }
    applyPartitionTableFilter();
}

/* ===== Wire General Tab Buttons ===== */

void MainWindow::wireGeneralButtons()
{
#if 0
    connect(ui->readInfoBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected(true)) {
                const bool cancelled = isCancelRequested();
                QMetaObject::invokeMethod(this, [this, cancelled]() {
                    if (cancelled)
                        appendLog("操作已取消", "#7D6B3A");
                    else
                        appendLog(QStringLiteral("读取信息错误：未连接设备或握手失败"), "#C0392B");
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() { /*
                    appendLog("操作已取消", "#7D6B3A");
                */    appendLog(QStringLiteral("读取分区表过程中已取消操作"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            /* Sahara（引导层）；Firehose GetStorageInfo 受「读取设备信息」复选框控制 */
            displayChipInfo();
            bool wantDevInfo = true;
            QMetaObject::invokeMethod(this, [this, &wantDevInfo]() {
                wantDevInfo = ui->readSysInfoCheckBox->isChecked();
            }, Qt::BlockingQueuedConnection); /*
            if (!wantDevInfo) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog("【配置】未勾选「读取设备信息」，已跳过 GetStorageInfo（仅已显示 Sahara 芯片信息）",
                              "#6B7280");
                }, Qt::QueuedConnection);
            */ if (!preflightFirehose("Read GPT"))
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog("操作已取消（已显示 Sahara，未执行存储信息读取）", "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose("读取信息 / GetStorageInfo"))
                return;
            if (!ensureGptCacheReady())
                return;
            char devInfoBuf[16384] = {};
            edl_error_t fhErr = edl_service_get_storage_device_report(m_edlService, devInfoBuf, sizeof(devInfoBuf));
            if (isCancelRequested())
                return;
            edl_android_props_t adbProps{};
            edl_error_t propErr = edl_service_probe_android_build_props(m_edlService, &adbProps);
            const QString devInfoReport = QString::fromUtf8(devInfoBuf);
            QMetaObject::invokeMethod(this, [this, fhErr, propErr, adbProps, devInfoReport]() {
                if (fhErr == EDL_OK) {
                    appendSectionLog(QStringLiteral("字库设备信息"));
                    for (const QString &line : devInfoReport.split(QLatin1Char('\n'))) {
                        if (!line.trimmed().isEmpty())
                            appendInfoLog(line);
                    }
                }
                logEdlResult("读取信息", fhErr);
                if (fhErr == EDL_OK)
                    applyDeviceStorageToUi(false);
                if (fhErr == EDL_OK && propErr == EDL_OK && adbProps.source_partition[0])
                    appendAndroidBuildPropsLog(adbProps);
                else if (fhErr == EDL_OK && propErr != EDL_OK)
                    appendLog(QStringLiteral("分区解析（build.prop）: %1")
                                  .arg(QString::fromUtf8(edl_error_str(propErr))),
                              "#6B7280");
            }, Qt::QueuedConnection);
        }, QStringLiteral("读取信息"));
    });

    });
#endif
    connect(ui->readInfoBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected(true)) {
                const bool cancelled = isCancelRequested();
                QMetaObject::invokeMethod(this, [this, cancelled]() {
                    if (cancelled)
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    else
                        appendLog(QStringLiteral("读取信息错误：设备未连接或握手失败"), "#C0392B");
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            displayChipInfo();

            bool wantDevInfo = true;
            QMetaObject::invokeMethod(this, [this, &wantDevInfo]() {
                wantDevInfo = ui->readSysInfoCheckBox->isChecked();
            }, Qt::BlockingQueuedConnection);
            if (!wantDevInfo) {
                QMetaObject::invokeMethod(this, [this]() {
                appendLog(QStringLiteral("【配置】未勾选“读取设备信息”，已跳过存储信息读取"), "#6B7280");
                }, Qt::QueuedConnection);
                return;
            }

            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(QStringLiteral("读取信息 / 获取字库信息")))
                return;
            {
                char devInfoBuf[16384] = {};
                edl_android_props_t adbProps{};
                edl_device_query_options_t queryOptions;
                edl_device_query_options_init(&queryOptions);
                queryOptions.read_storage_report = true;
                queryOptions.ensure_gpt_cache = true;
                queryOptions.read_android_props = true;

                edl_device_query_result_t queryResult = {};
                queryResult.storage_report = devInfoBuf;
                queryResult.storage_report_size = sizeof(devInfoBuf);
                queryResult.android_props = &adbProps;

                const edl_error_t queryErr =
                    edl_service_collect_device_query(m_edlService, &queryOptions, &queryResult);
                if (isCancelRequested())
                    return;

                const edl_error_t infoErr = devInfoBuf[0] ? EDL_OK : queryErr;
                const edl_error_t propErr = adbProps.source_partition[0]
                    ? EDL_OK
                    : (queryErr != EDL_OK && infoErr == EDL_OK ? queryErr : EDL_OK);
                const QString devInfoReport = QString::fromUtf8(devInfoBuf);

                QMetaObject::invokeMethod(this, [this, infoErr, propErr, adbProps, devInfoReport]() {
                    if (infoErr == EDL_OK) {
                        appendSectionLog(QStringLiteral("字库设备信息"));
                        for (const QString &line : devInfoReport.split(QLatin1Char('\n'))) {
                            if (!line.trimmed().isEmpty())
                                appendInfoLog(line);
                        }
                    }
                    logEdlResult(QStringLiteral("读取信息"), infoErr);
                    if (infoErr == EDL_OK)
                        applyDeviceStorageToUi(false);
                    if (infoErr == EDL_OK && propErr == EDL_OK && adbProps.source_partition[0]) {
                        appendAndroidBuildPropsLog(adbProps);
                    } else if (infoErr == EDL_OK && propErr != EDL_OK) {
                        appendLog(QStringLiteral("build.prop 探测：%1")
                                      .arg(QString::fromUtf8(edl_error_str(propErr))),
                                  "#6B7280");
                    }
                }, Qt::QueuedConnection);
                return;
            }
            if (!ensureGptCacheReady())
                return;

            QMetaObject::invokeMethod(this, [this]() {
                appendLog(QStringLiteral("【读取信息】正在读取字库设备信息…"), "#6B7280");
            }, Qt::QueuedConnection);
            char devInfoBuf[16384] = {};
            edl_error_t fhErr =
                edl_service_get_storage_device_report(m_edlService, devInfoBuf, sizeof(devInfoBuf));
            if (isCancelRequested())
                return;

            edl_android_props_t adbProps{};
            edl_error_t propErr = fhErr;
            if (fhErr == EDL_OK) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("【读取信息】正在探测 Android 系统属性…"), "#6B7280");
                }, Qt::QueuedConnection);
                propErr = edl_service_probe_android_build_props(m_edlService, &adbProps);
            }
            const QString devInfoReport = QString::fromUtf8(devInfoBuf);
            QMetaObject::invokeMethod(this, [this, fhErr, propErr, adbProps, devInfoReport]() {
                if (fhErr == EDL_OK) {
                    appendSectionLog(QStringLiteral("字库设备信息"));
                    for (const QString &line : devInfoReport.split(QLatin1Char('\n'))) {
                        if (!line.trimmed().isEmpty())
                            appendInfoLog(line);
                    }
                }
                logEdlResult(QStringLiteral("读取信息"), fhErr);
                if (fhErr == EDL_OK)
                    applyDeviceStorageToUi(false);
                if (fhErr == EDL_OK && propErr == EDL_OK && adbProps.source_partition[0])
                    appendAndroidBuildPropsLog(adbProps);
                else if (fhErr == EDL_OK && propErr != EDL_OK)
                    appendLog(QStringLiteral("build.prop 探测：%1")
                                  .arg(QString::fromUtf8(edl_error_str(propErr))),
                              "#6B7280");
            }, Qt::QueuedConnection);
        }, QStringLiteral("读取信息"));
    });

    #if 0
    #if 0
    connect(ui->readStorageDeviceInfoBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected(true)) {
                QMetaObject::invokeMethod(this, [this]() {
                    if (isCancelRequested())
                        appendLog("操作已取消", "#7D6B3A");
                    else
                        appendLog(QStringLiteral("读取字库设备信息错误：未连接设备或握手失败"), "#C0392B");
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog("操作已取消", "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(QStringLiteral("读取字库设备信息")))
                return;
            {
                char buf[16384] = {};
                edl_android_props_t adbProps{};
                edl_device_query_options_t queryOptions;
                edl_device_query_options_init(&queryOptions);
                queryOptions.read_storage_report = true;
                queryOptions.ensure_gpt_cache = true;
                queryOptions.read_android_props = true;

                edl_device_query_result_t queryResult = {};
                queryResult.storage_report = buf;
                queryResult.storage_report_size = sizeof(buf);
                queryResult.android_props = &adbProps;

                const edl_error_t queryErr =
                    edl_service_collect_device_query(m_edlService, &queryOptions, &queryResult);
                if (isCancelRequested())
                    return;

                const edl_error_t infoErr = buf[0] ? EDL_OK : queryErr;
                const edl_error_t propErr = adbProps.source_partition[0]
                    ? EDL_OK
                    : (queryErr != EDL_OK && infoErr == EDL_OK ? queryErr : EDL_OK);
                const QString report = QString::fromUtf8(buf);

                QMetaObject::invokeMethod(this, [this, infoErr, propErr, adbProps, report]() {
                    if (infoErr == EDL_OK) {
                        appendSectionLog(QStringLiteral("字库设备信息"));
                        for (const QString &line : report.split(QLatin1Char('\n'))) {
                            if (!line.trimmed().isEmpty())
                                appendInfoLog(line);
                        }
                    }
                    if (propErr == EDL_OK && adbProps.source_partition[0]) {
                        appendAndroidBuildPropsLog(adbProps);
                    } else if (infoErr == EDL_OK && propErr != EDL_OK) {
                        appendLog(QStringLiteral("设备系统信息解析：%1")
                                      .arg(QString::fromUtf8(edl_error_str(propErr))),
                                  "#6B7280");
                    }
                    logEdlResult(QStringLiteral("读取字库设备信息"), infoErr);
                }, Qt::QueuedConnection);
                return;
            }
            if (!ensureGptCacheReady())
                return;
            char buf[16384] = {};
            edl_error_t err = edl_service_get_storage_device_report(m_edlService, buf, sizeof(buf));
            edl_android_props_t adbProps{};
            edl_error_t propErr = edl_service_probe_android_build_props(m_edlService, &adbProps);
            const QString report = QString::fromUtf8(buf);
            QMetaObject::invokeMethod(this, [this, err, propErr, adbProps, report]() {
                if (err == EDL_OK) {
                    appendSectionLog(QStringLiteral("字库设备信息"));
                    for (const QString &line : report.split(QLatin1Char('\n'))) {
                        if (!line.trimmed().isEmpty())
                            appendInfoLog(line);
                    }
                }
                if (err == EDL_OK && propErr == EDL_OK && adbProps.source_partition[0])
                    appendAndroidBuildPropsLog(adbProps);
                logEdlResult(QStringLiteral("读取字库设备信息"), err);
            }, Qt::QueuedConnection);
        }, QStringLiteral("读取字库设备信息"));
    });

    #endif

    #endif

    #if 0
    connect(ui->readStorageDeviceInfoBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected(true)) {
                QMetaObject::invokeMethod(this, [this]() {
                    if (isCancelRequested())
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    else
                        appendLog(QStringLiteral("读取字库设备信息错误：设备未连接或握手失败"), "#C0392B");
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(QStringLiteral("读取字库设备信息")))
                return;

            char buf[16384] = {};
            edl_device_query_options_t queryOptions;
            edl_device_query_options_init(&queryOptions);
            queryOptions.read_storage_report = true;
            queryOptions.ensure_gpt_cache = true;
            queryOptions.read_android_props = true;

            edl_device_query_result_t queryResult = {};
            queryResult.storage_report = buf;
            queryResult.storage_report_size = sizeof(buf);
            edl_android_props_t adbProps{};
            queryResult.android_props = &adbProps;

            const edl_error_t queryErr =
                edl_service_collect_device_query(m_edlService, &queryOptions, &queryResult);
            if (isCancelRequested())
                return;

            const edl_error_t infoErr = buf[0] ? EDL_OK : queryErr;
            const edl_error_t propErr = adbProps.source_partition[0]
                ? EDL_OK
                : (queryErr != EDL_OK && infoErr == EDL_OK ? queryErr : EDL_OK);
            const QString report = QString::fromUtf8(buf);

            QMetaObject::invokeMethod(this, [this, infoErr, propErr, adbProps, report]() {
                if (infoErr == EDL_OK) {
                    appendSectionLog(QStringLiteral("字库设备信息"));
                    for (const QString &line : report.split(QLatin1Char('\n'))) {
                        if (!line.trimmed().isEmpty())
                            appendInfoLog(line);
                    }
                    applyDeviceStorageToUi(false);
                }
                logEdlResult(QStringLiteral("读取字库设备信息"), infoErr);
            }, Qt::QueuedConnection);
        }, QStringLiteral("读取字库设备信息"));
    });

    auto wireActivateBootSlot = [this](bool bootA) {
        const QString opLabel = bootA ? QStringLiteral("激活 Boot A") : QStringLiteral("激活 Boot B");
        runAsync([this, bootA, opLabel]() {
            if (!ensureConnected()) {
                QMetaObject::invokeMethod(this, [this]() {
                    if (isCancelRequested())
                        appendLog(QStringLiteral("操作已取消"), QStringLiteral("#7D6B3A"));
                    else
                        appendLog(QStringLiteral("激活启动分区错误：未连接设备或握手失败"), QStringLiteral("#C0392B"));
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), QStringLiteral("#7D6B3A"));
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(opLabel))
                return;

            int comboIdx = 0;
            QMetaObject::invokeMethod(this, [this, &comboIdx]() {
                comboIdx = ui->storageCombo->currentIndex();
            }, Qt::BlockingQueuedConnection);

            const QString kind = resolveStorageKindForBoot(edl_service_storage_type(m_edlService), comboIdx);
            const int lun = lunForManualBootSlot(kind, bootA);
            edl_service_try_set_bootable_storage_drive(m_edlService, lun);

            QMetaObject::invokeMethod(this, [this, opLabel, lun, kind, bootA]() {
                const QString slot = bootA ? QStringLiteral("A") : QStringLiteral("B");
                appendLog(QStringLiteral("──────── %1 ────────").arg(opLabel), QStringLiteral("#5B6B8C"));
                appendLog(QStringLiteral("存储类型: %1 | 发送 setbootablestoragedrive(value=%2)（Boot %3）")
                              .arg(kind.isEmpty() ? QStringLiteral("(未知)") : kind)
                              .arg(lun)
                              .arg(slot),
                          QStringLiteral("#6B7280"));
                if (kind == QLatin1String("emmc"))
                    appendLog(QStringLiteral("提示：eMMC 通常为单 LUN，Boot A/B 均使用 value=0；A/B 槽位切换还依赖 GPT 属性或刷写对应分区。"),
                              QStringLiteral("#7D6B3A"));
                appendLog(QStringLiteral("应答细节见「详细日志」（部分机型仅忽略/无 ACK）。"), QStringLiteral("#6B7280"));
                appendLog(QStringLiteral("SAKURAEDL %1 完成").arg(opLabel), QStringLiteral("#2E8B3E"));
            }, Qt::QueuedConnection);
        }, opLabel);
    };
    connect(ui->actionActivateBootA, &QAction::triggered, this, [wireActivateBootSlot]() {
        wireActivateBootSlot(true);
    });
    connect(ui->actionActivateBootB, &QAction::triggered, this, [wireActivateBootSlot]() {
        wireActivateBootSlot(false);
    });

#if 0
    #if 0
    connect(ui->readGptBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog("操作已取消", "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog("操作已取消", "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            edl_partition_info_t parts[256];
            int count = 256;

            QString storageStr;
            QMetaObject::invokeMethod(this, [this, &storageStr]() {
                int idx = ui->storageCombo->currentIndex();
                if (idx == 1) storageStr = "emmc";
                else if (idx == 2) storageStr = "ufs";
            }, Qt::BlockingQueuedConnection);

            if (!preflightFirehose("读取分区表"))
                return;

            /* UFS：与 core svc_ensure_gpt 一致扫至 LUN15（部分 OPPO/Realme GPT 在更高 LUN） */
            int maxLun = storageStr == "emmc" ? 1 : 24;
            edl_error_t err = edl_service_read_gpt_ex(m_edlService, parts, &count, maxLun, 0u);

            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog("操作已取消（读取分区表可能已部分完成）", "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            if (err == EDL_OK) {
                QVector<edl_partition_info_t> partVec(parts, parts + count);
                QMetaObject::invokeMethod(this, [this, partVec]() {
                    populatePartitionTable(partVec.data(), partVec.size()); /*
                    appendLog(QString("读取分区表 OK（共 %1 个分区）").arg(partVec.size()), "#2E8B3E");
                */    appendLog(QString("读取分区表成功（共 %1 个分区）").arg(partVec.size()), "#2E8B3E");
                }, Qt::QueuedConnection);
            } else if (err == EDL_ERR_CANCELLED) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog("读取分区表已取消", "#7D6B3A");
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this, err]() { /*
                    logEdlResult("读取分区表", err);
                */    logEdlResult("Read GPT", err);
                }, Qt::QueuedConnection);
            }
        }, QStringLiteral("读取分区表"));
    });

    });
#endif
    #if 0
    connect(ui->readGptBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            edl_partition_info_t parts[256];
            int count = 256;

            QString storageStr;
            QMetaObject::invokeMethod(this, [this, &storageStr]() {
                int idx = ui->storageCombo->currentIndex();
                if (idx == 1)
                    storageStr = QStringLiteral("emmc");
                else if (idx == 2)
                    storageStr = QStringLiteral("ufs");
            }, Qt::BlockingQueuedConnection);

            if (!preflightFirehose("Read GPT"))
                return;

            const int maxLun = storageStr == QStringLiteral("emmc") ? 1 : 24;
            edl_error_t err = edl_service_read_gpt_ex(m_edlService, parts, &count, maxLun, 0u);

            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消（读取分区表过程中）"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            if (err == EDL_OK) {
                QVector<edl_partition_info_t> partVec(parts, parts + count);
                QMetaObject::invokeMethod(this, [this, partVec]() {
                    populatePartitionTable(partVec.data(), partVec.size());
                    appendLog(QStringLiteral("读取分区表完成（%1 个分区）").arg(partVec.size()),
                              "#2E8B3E");
                }, Qt::QueuedConnection);
            } else if (err == EDL_ERR_CANCELLED) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("读取分区表已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this, err]() {
                    logEdlResult(QStringLiteral("Read GPT"), err);
                }, Qt::QueuedConnection);
            }
        }, QStringLiteral("Read GPT"));
    });
    #endif

    connect(ui->writeGptXmlBtn, &QPushButton::clicked, this, [this]() {
        const QStringList xmlPaths = QFileDialog::getOpenFileNames(
            this,
            QStringLiteral("选择 Rawprogram XML（可多选，含 PrimaryGPT / BackupGPT）"),
            QString(),
            QStringLiteral("Rawprogram XML (*.xml);;所有文件 (*.*)"));
        if (xmlPaths.isEmpty())
            return;

        const bool doFixGpt = ui->fixGptAfterWriteCheckBox->isChecked();

        runAsync([this, xmlPaths, doFixGpt]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), QStringLiteral("#7D6B3A"));
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), QStringLiteral("#7D6B3A"));
                }, Qt::QueuedConnection);
                return;
            }

            if (!preflightFirehose(QStringLiteral("写 GPT")))
                return;

            bool fileLenOnlyGpt = false;
            QMetaObject::invokeMethod(
                this,
                [&fileLenOnlyGpt, this]() {
                    fileLenOnlyGpt = ui->writeFileLengthOnlyCheckBox->isChecked();
                },
                Qt::BlockingQueuedConnection);
            edl_service_set_write_options(m_edlService, !fileLenOnlyGpt, false);

            const int total = xmlPaths.size();
            for (int idx = 0; idx < total; ++idx) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消（写 GPT 可能已部分完成）"),
                                  QStringLiteral("#7D6B3A"));
                    }, Qt::QueuedConnection);
                    return;
                }

                const QString &xmlPath = xmlPaths.at(idx);
                const QString baseDir = QFileInfo(xmlPath).absolutePath();

                QMetaObject::invokeMethod(this, [this, idx, total, xmlPath]() {
                    appendLog(QStringLiteral("写 GPT [%1/%2] %3")
                                  .arg(idx + 1)
                                  .arg(total)
                                  .arg(xmlPath),
                              QStringLiteral("#6B7280"));
                }, Qt::QueuedConnection);

                const QByteArray xmlUtf8 = xmlPath.toUtf8();
                const QByteArray baseUtf8 = baseDir.toUtf8();
                /* 多文件时 fixgpt 只在全部写入成功后执行一次，避免重复 */
                edl_error_t err = edl_service_write_gpt_from_rawprogram_xml(
                    m_edlService, xmlUtf8.constData(), baseUtf8.constData(), 0u);

                if (err != EDL_OK) {
                    QMetaObject::invokeMethod(this, [this, xmlPath, err]() {
                        appendLog(QStringLiteral("失败: %1").arg(xmlPath), QStringLiteral("#C0392B"));
                        logEdlResult(QStringLiteral("写 GPT"), err);
                    }, Qt::QueuedConnection);
                    return;
                }
            }

            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消（写 GPT 可能已部分完成）"),
                              QStringLiteral("#7D6B3A"));
                }, Qt::QueuedConnection);
                return;
            }

            edl_error_t fixErr = EDL_OK;
            if (doFixGpt && !isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("写 GPT：执行 fixgpt（主备/CRC）…"),
                              QStringLiteral("#6B7280"));
                }, Qt::QueuedConnection);
                fixErr = edl_service_fix_gpt(m_edlService);
                if (fixErr == EDL_ERR_CANCELLED) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("fixgpt 已取消"), QStringLiteral("#7D6B3A"));
                    }, Qt::QueuedConnection);
                    return;
                }
                if (fixErr != EDL_OK) {
                    QMetaObject::invokeMethod(this, [this, fixErr]() {
                        logEdlResult(QStringLiteral("fixgpt"), fixErr);
                    }, Qt::QueuedConnection);
                }
            }

            if (isCancelRequested())
                return;

            QMetaObject::invokeMethod(this, [this, doFixGpt, fixErr]() {
                if (doFixGpt && fixErr != EDL_OK) {
                    appendLog(QStringLiteral("GPT 镜像已按所选 XML 写入，但 fixgpt 失败（见上）。"),
                              QStringLiteral("#C0392B"));
                    return;
                }
                appendLog(QStringLiteral("写 GPT 完成。如需查看新分区布局，请点击「读取分区表」。"),
                          QStringLiteral("#2E8B3E"));
            }, Qt::QueuedConnection);
        }, QStringLiteral("写 GPT"));
    });

    connect(ui->removeFrpBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog("操作已取消", "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog("操作已取消", "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose("移除 FRP"))
                return;
            if (!ensureGptCacheReady())
                return;
            if (isCancelRequested())
                return;
            edl_error_t err = edl_service_erase_frp(m_edlService);
            if (err == EDL_ERR_CANCELLED)
                return;
            QMetaObject::invokeMethod(this, [this, err]() {
                logEdlResult("移除 FRP", err);
            }, Qt::QueuedConnection);
        }, QStringLiteral("移除 FRP"));
    });

    auto *rebootMenu = ui->rebootBtn->menu();
    if (rebootMenu) {
        auto actions = rebootMenu->actions();
        for (auto *action : actions) {
            if (action->isSeparator()) continue;
            connect(action, &QAction::triggered, this, [this, action]() {
                const QString key = action->data().toString();
                QString rebootLabel = QStringLiteral("重启");
                if (key == QLatin1String("reset"))
                    rebootLabel = QStringLiteral("重启到系统");
                else if (key == QLatin1String("off"))
                    rebootLabel = QStringLiteral("关机");
                else if (key == QLatin1String("misc_fastbootd"))
                    rebootLabel = QStringLiteral("重启到 Fastbootd（MISC）");
                else if (key == QLatin1String("misc_recovery"))
                    rebootLabel = QStringLiteral("重启到 Recovery（MISC）");
                else if (key == QLatin1String("edl_download"))
                    rebootLabel = QStringLiteral("重启到 EDL（9008）");

                if (key == QLatin1String("misc_fastbootd") || key == QLatin1String("misc_recovery")) {
                    const bool toFastbootd = (key == QLatin1String("misc_fastbootd"));
                    runAsync([this, toFastbootd, rebootLabel]() {
                        if (!ensureConnected()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                if (isCancelRequested())
                                    appendLog("操作已取消", "#7D6B3A");
                                else
                                    appendLog(QStringLiteral("MISC 重启错误：未连接设备"), "#C0392B");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (isCancelRequested()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                appendLog("操作已取消", "#7D6B3A");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (!preflightFirehose(toFastbootd ? QStringLiteral("Fastbootd(MISC)")
                                                           : QStringLiteral("Recovery(MISC)")))
                            return;
                        const QString path = resolveMiscBootImage(toFastbootd);
                        if (path.isEmpty()) {
                            QMetaObject::invokeMethod(this, [this, toFastbootd]() {
                                appendLog(toFastbootd
                                              ? QStringLiteral("未找到 Fastbootd 用 MISC 镜像：请在「设置」填写路径，"
                                                               "或将 misc_tofastbootd.img 放到程序目录，"
                                                               "或编译时嵌入 bundled/（见 bundled/README.md）。")
                                              : QStringLiteral("未找到 Recovery 用 MISC 镜像：请在「设置」填写路径，"
                                                               "或将 misc_torecovery.img 放到程序目录，"
                                                               "或编译时嵌入 bundled/（见 bundled/README.md）。"),
                                          "#C0392B");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (!ensureGptCacheReady())
                            return;
                        if (isCancelRequested())
                            return;
                        QMetaObject::invokeMethod(this, [this, path]() {
                            const QString fileName = QFileInfo(path).fileName();
                            appendLog(QStringLiteral("MISC 镜像: %1")
                                          .arg(fileName.isEmpty() ? QStringLiteral("(未知文件)") : fileName),
                                      "#6B7280");
                        }, Qt::QueuedConnection);
                        const QByteArray pb = path.toUtf8();
                        edl_error_t err = edl_service_misc_write_image_and_reset(
                            m_edlService, pb.constData(), nullptr);
                        if (err == EDL_ERR_CANCELLED)
                            return;
                        QMetaObject::invokeMethod(this, [this, err]() {
                            if (err == EDL_OK)
                                appendLog(QStringLiteral("已写入 MISC 并发送重启（reset）"), "#2E8B3E");
                            else
                                appendLog(QStringLiteral("MISC 重启失败: %1")
                                              .arg(QString::fromUtf8(edl_error_str(err))),
                                          "#C0392B");
                        }, Qt::QueuedConnection);
                    }, rebootLabel);
                    return;
                }

                if (key == QLatin1String("edl_download")) {
                    runAsync([this, rebootLabel]() {
                        if (!ensureConnected()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                if (isCancelRequested())
                                    appendLog("操作已取消", "#7D6B3A");
                                else
                                    appendLog(QStringLiteral("EDL 重启错误：未连接设备"), "#C0392B");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (isCancelRequested()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                appendLog("操作已取消", "#7D6B3A");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (!preflightFirehose(QStringLiteral("power download(EDL)")))
                            return;
                        edl_service_reboot(m_edlService, "download");
                        QMetaObject::invokeMethod(this, [this]() {
                            appendLog(QStringLiteral("已发送 power download（若未进 9008 属固件差异，请用音量键或 adb reboot edl）"),
                                      "#2E8B3E");
                        }, Qt::QueuedConnection);
                    }, rebootLabel);
                    return;
                }

                QString mode = QStringLiteral("reset");
                if (key == QLatin1String("off"))
                    mode = QStringLiteral("off");
                else if (key != QLatin1String("reset")) {
                    /* 兼容无 data 的旧菜单项 */
                    const QString text = action->text();
                    if (text.contains(QStringLiteral("关机")))
                        mode = QStringLiteral("off");
                }

                runAsync([this, mode, rebootLabel]() {
                    if (!ensureConnected()) {
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isCancelRequested())
                                appendLog("操作已取消", "#7D6B3A");
                            else
                                appendLog(QStringLiteral("电源指令错误：未连接设备"), "#C0392B");
                        }, Qt::QueuedConnection);
                        return;
                    }
                    if (isCancelRequested()) {
                        QMetaObject::invokeMethod(this, [this]() {
                            appendLog("操作已取消", "#7D6B3A");
                        }, Qt::QueuedConnection);
                        return;
                    }
                    if (!preflightFirehose(QStringLiteral("电源指令")))
                        return;
                    edl_service_reboot(m_edlService, mode.toUtf8().constData());
                    QMetaObject::invokeMethod(this, [this, mode]() {
                        if (mode == QLatin1String("off"))
                            appendLog(QStringLiteral("关机指令成功"), "#2E8B3E");
                        else
                            appendLog(QStringLiteral("重启到系统成功"), "#2E8B3E");
                    }, Qt::QueuedConnection);
                }, rebootLabel);
            });
        }
    }
#endif

    #endif

    #if 0
    connect(ui->readStorageDeviceInfoBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected(true)) {
                QMetaObject::invokeMethod(this, [this]() {
                    if (isCancelRequested())
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    else
                        appendLog(QStringLiteral("读取字库设备信息错误：设备未连接或握手失败"),
                                  "#C0392B");
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(QStringLiteral("读取字库设备信息")))
                return;

            QMetaObject::invokeMethod(this, [this]() {
                appendLog(QStringLiteral("【读取字库设备信息】正在读取字库设备信息…"), "#6B7280");
            }, Qt::QueuedConnection);
            char buf[16384] = {};
            edl_error_t err = edl_service_get_storage_device_report(m_edlService, buf, sizeof(buf));
            const QString report = QString::fromUtf8(buf);
            QMetaObject::invokeMethod(this, [this, err, report]() {
                if (err == EDL_OK) {
                    appendSectionLog(QStringLiteral("字库设备信息"));
                    for (const QString &line : report.split(QLatin1Char('\n'))) {
                        if (!line.trimmed().isEmpty())
                            appendInfoLog(line);
                    }
                }
            }, Qt::QueuedConnection);

            if (err != EDL_OK || isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this, err]() {
                    logEdlResult(QStringLiteral("读取字库设备信息"), err);
                }, Qt::QueuedConnection);
                return;
            }

            QMetaObject::invokeMethod(this, [this]() {
                appendLog(QStringLiteral("【读取字库设备信息】正在探测设备系统信息…"), "#6B7280");
            }, Qt::QueuedConnection);

            if (!ensureGptCacheReady()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("设备系统信息：读取分区表失败，未能继续解析"), "#6B7280");
                    logEdlResult(QStringLiteral("读取字库设备信息"), EDL_OK);
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested())
                return;

            edl_android_props_t adbProps{};
            edl_error_t propErr = edl_service_probe_android_build_props(m_edlService, &adbProps);
            QMetaObject::invokeMethod(this, [this, err, propErr, adbProps]() {
                if (propErr == EDL_OK && adbProps.source_partition[0]) {
                    appendAndroidBuildPropsLog(adbProps);
                } else if (propErr != EDL_OK) {
                    appendLog(QStringLiteral("设备系统信息解析：%1")
                                  .arg(QString::fromUtf8(edl_error_str(propErr))),
                              "#6B7280");
                }
                logEdlResult(QStringLiteral("读取字库设备信息"), err);
            }, Qt::QueuedConnection);
        }, QStringLiteral("读取字库设备信息"));
    });

    #endif

    connect(ui->readStorageDeviceInfoBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected(true)) {
                QMetaObject::invokeMethod(this, [this]() {
                    if (isCancelRequested())
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    else
                        appendLog(QStringLiteral("读取字库设备信息错误：设备未连接或握手失败"), "#C0392B");
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(QStringLiteral("读取字库设备信息")))
                return;

            char buf[16384] = {};
            edl_device_query_options_t queryOptions;
            edl_device_query_options_init(&queryOptions);
            queryOptions.read_storage_report = true;
            queryOptions.ensure_gpt_cache = true;
            queryOptions.read_android_props = true;

            edl_device_query_result_t queryResult = {};
            queryResult.storage_report = buf;
            queryResult.storage_report_size = sizeof(buf);
            edl_android_props_t adbProps{};
            queryResult.android_props = &adbProps;

            const edl_error_t queryErr =
                edl_service_collect_device_query(m_edlService, &queryOptions, &queryResult);
            if (isCancelRequested())
                return;

            const edl_error_t infoErr = buf[0] ? EDL_OK : queryErr;
            const edl_error_t propErr = adbProps.source_partition[0]
                ? EDL_OK
                : (queryErr != EDL_OK && infoErr == EDL_OK ? queryErr : EDL_OK);
            const QString report = QString::fromUtf8(buf);

            QMetaObject::invokeMethod(this, [this, infoErr, propErr, adbProps, report]() {
                if (infoErr == EDL_OK) {
                    appendSectionLog(QStringLiteral("字库设备信息"));
                    for (const QString &line : report.split(QLatin1Char('\n'))) {
                        if (!line.trimmed().isEmpty())
                            appendInfoLog(line);
                    }
                    applyDeviceStorageToUi(false);
                }
                if (propErr == EDL_OK && adbProps.source_partition[0]) {
                    appendAndroidBuildPropsLog(adbProps);
                } else if (infoErr == EDL_OK && propErr != EDL_OK) {
                    appendLog(QStringLiteral("设备系统信息解析：%1")
                                  .arg(QString::fromUtf8(edl_error_str(propErr))),
                              "#6B7280");
                }
                logEdlResult(QStringLiteral("读取字库设备信息"), infoErr);
            }, Qt::QueuedConnection);
        }, QStringLiteral("读取字库设备信息"));
    });

    auto wireActivateBootSlot = [this](bool bootA) {
        const QString opLabel = bootA ? QStringLiteral("激活 Boot A")
                                      : QStringLiteral("激活 Boot B");
        runAsync([this, bootA, opLabel]() {
            if (!ensureConnected()) {
                QMetaObject::invokeMethod(this, [this]() {
                    if (isCancelRequested())
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    else
                        appendLog(QStringLiteral("激活启动分区错误：设备未连接或握手失败"),
                                  "#C0392B");
                }, Qt::QueuedConnection);
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(opLabel))
                return;

            int comboIdx = 0;
            QMetaObject::invokeMethod(this, [this, &comboIdx]() {
                comboIdx = ui->storageCombo->currentIndex();
            }, Qt::BlockingQueuedConnection);

            const QString kind = resolveStorageKindForBoot(edl_service_storage_type(m_edlService), comboIdx);
            const int lun = lunForManualBootSlot(kind, bootA);
            edl_service_try_set_bootable_storage_drive(m_edlService, lun);

            QMetaObject::invokeMethod(this, [this, opLabel, lun, kind, bootA]() {
                const QString slot = bootA ? QStringLiteral("A") : QStringLiteral("B");
                appendLog(QStringLiteral("======== %1 ========").arg(opLabel), "#5B6B8C");
                appendLog(QStringLiteral("存储类型: %1 | 发送 setbootablestoragedrive(value=%2)（Boot %3）")
                              .arg(kind.isEmpty() ? QStringLiteral("(未知)") : kind)
                              .arg(lun)
                              .arg(slot),
                          "#6B7280");
                if (kind == QLatin1String("emmc")) {
                    appendLog(QStringLiteral("提示：eMMC 通常只有单个可启动 LUN；槽位切换仍可能依赖 GPT 属性或本次刷写的分区集合。"),
                              "#7D6B3A");
                }
                appendLog(QStringLiteral("设备原始响应请查看详细日志。"), "#6B7280");
                appendLog(QStringLiteral("SAKURAEDL %1 完成").arg(opLabel), "#2E8B3E");
            }, Qt::QueuedConnection);
        }, opLabel);
    };
    connect(ui->actionActivateBootA, &QAction::triggered, this, [wireActivateBootSlot]() {
        wireActivateBootSlot(true);
    });
    connect(ui->actionActivateBootB, &QAction::triggered, this, [wireActivateBootSlot]() {
        wireActivateBootSlot(false);
    });

    connect(ui->readGptBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            edl_partition_info_t parts[256];
            int count = 256;
            QString storageStr;
            QMetaObject::invokeMethod(this, [this, &storageStr]() {
                const int idx = ui->storageCombo->currentIndex();
                if (idx == 1)
                    storageStr = QStringLiteral("emmc");
                else if (idx == 2)
                    storageStr = QStringLiteral("ufs");
            }, Qt::BlockingQueuedConnection);

            if (!preflightFirehose(QStringLiteral("读取分区表")))
                return;
            {
                edl_device_query_options_t queryOptions;
                edl_device_query_options_init(&queryOptions);
                queryOptions.read_storage_report = false;
                queryOptions.ensure_gpt_cache = true;
                queryOptions.read_android_props = false;
                queryOptions.gpt_max_lun = storageStr == QStringLiteral("emmc") ? 1 : 0;

                edl_device_query_result_t queryResult = {};
                queryResult.gpt_parts = parts;
                queryResult.gpt_count = &count;

                const edl_error_t err =
                    edl_service_collect_device_query(m_edlService, &queryOptions, &queryResult);
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消（读取分区表过程中）"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                    return;
                }

                if (err == EDL_OK) {
                    QVector<edl_partition_info_t> partVec(parts, parts + count);
                    QMetaObject::invokeMethod(this, [this, partVec]() {
                        populatePartitionTable(partVec.data(), partVec.size());
                        appendLog(QStringLiteral("读取分区表完成（%1 个分区）").arg(partVec.size()), "#2E8B3E");
                    }, Qt::QueuedConnection);
                } else if (err == EDL_ERR_CANCELLED) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("读取分区表已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                } else {
                    QMetaObject::invokeMethod(this, [this, err]() {
                        logEdlResult(QStringLiteral("读取分区表"), err);
                    }, Qt::QueuedConnection);
                }
                return;
            }

            const int maxLun = storageStr == QStringLiteral("emmc") ? 1 : 24;
            edl_error_t err = edl_service_read_gpt_ex(m_edlService, parts, &count, maxLun, 0u);

            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消（读取分区表过程中）"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            if (err == EDL_OK) {
                QVector<edl_partition_info_t> partVec(parts, parts + count);
                QMetaObject::invokeMethod(this, [this, partVec]() {
                    populatePartitionTable(partVec.data(), partVec.size());
                    appendLog(QStringLiteral("读取分区表完成（%1 个分区）").arg(partVec.size()), "#2E8B3E");
                }, Qt::QueuedConnection);
            } else if (err == EDL_ERR_CANCELLED) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("读取分区表已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this, err]() {
                    logEdlResult(QStringLiteral("Read GPT"), err);
                }, Qt::QueuedConnection);
            }
        }, QStringLiteral("读取分区表"));
    });

    connect(ui->writeGptXmlBtn, &QPushButton::clicked, this, [this]() {
        const QStringList xmlPaths = QFileDialog::getOpenFileNames(
            this,
            QStringLiteral("选择包含 PrimaryGPT/BackupGPT 的 rawprogram XML"),
            QString(),
            QStringLiteral("Rawprogram XML (*.xml);;所有文件 (*.*)"));
        if (xmlPaths.isEmpty())
            return;

        const bool doFixGpt = ui->fixGptAfterWriteCheckBox->isChecked();
        runAsync([this, xmlPaths, doFixGpt]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(QStringLiteral("Write GPT")))
                return;

            bool fileLenOnlyGpt = false;
            QMetaObject::invokeMethod(
                this,
                [&fileLenOnlyGpt, this]() {
                    fileLenOnlyGpt = ui->writeFileLengthOnlyCheckBox->isChecked();
                },
                Qt::BlockingQueuedConnection);
            edl_service_set_write_options(m_edlService, !fileLenOnlyGpt, false);

            const int total = xmlPaths.size();
            for (int idx = 0; idx < total; ++idx) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消（写入 GPT 过程中）"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                    return;
                }

                const QString &xmlPath = xmlPaths.at(idx);
                const QString baseDir = QFileInfo(xmlPath).absolutePath();
                QMetaObject::invokeMethod(this, [this, idx, total, xmlPath]() {
                    appendLog(QStringLiteral("写入 GPT [%1/%2] %3")
                                  .arg(idx + 1)
                                  .arg(total)
                                  .arg(xmlPath),
                              "#6B7280");
                }, Qt::QueuedConnection);

                const QByteArray xmlUtf8 = xmlPath.toUtf8();
                const QByteArray baseUtf8 = baseDir.toUtf8();
                edl_error_t err = edl_service_write_gpt_from_rawprogram_xml(
                    m_edlService, xmlUtf8.constData(), baseUtf8.constData(), 0u);
                if (err != EDL_OK) {
                    QMetaObject::invokeMethod(this, [this, xmlPath, err]() {
                        appendLog(QStringLiteral("写入 GPT 失败：%1").arg(xmlPath), "#C0392B");
                        logEdlResult(QStringLiteral("Write GPT"), err);
                    }, Qt::QueuedConnection);
                    return;
                }
            }

            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消（写入 GPT 过程中）"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }

            edl_error_t fixErr = EDL_OK;
            if (doFixGpt && !isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("写入 GPT：正在执行 fixgpt"), "#6B7280");
                }, Qt::QueuedConnection);
                fixErr = edl_service_fix_gpt(m_edlService);
                if (fixErr == EDL_ERR_CANCELLED) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("fixgpt 已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                    return;
                }
                if (fixErr != EDL_OK) {
                    QMetaObject::invokeMethod(this, [this, fixErr]() {
                        logEdlResult(QStringLiteral("fixgpt"), fixErr);
                    }, Qt::QueuedConnection);
                }
            }

            if (isCancelRequested())
                return;

            QMetaObject::invokeMethod(this, [this, doFixGpt, fixErr]() {
                if (doFixGpt && fixErr != EDL_OK) {
                    appendLog(QStringLiteral("GPT 镜像已写入，但 fixgpt 执行失败"), "#C0392B");
                    return;
                }
                appendLog(QStringLiteral("写入 GPT 完成，可重新读取分区表刷新显示"),
                          "#2E8B3E");
            }, Qt::QueuedConnection);
        }, QStringLiteral("Write GPT"));
    });

    connect(ui->removeFrpBtn, &QPushButton::clicked, this, [this]() {
        runAsync([this]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (isCancelRequested()) {
                QMetaObject::invokeMethod(this, [this]() {
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                }, Qt::QueuedConnection);
                return;
            }
            if (!preflightFirehose(QStringLiteral("Erase FRP")))
                return;
            if (!ensureGptCacheReady())
                return;
            if (isCancelRequested())
                return;

            edl_error_t err = edl_service_erase_frp(m_edlService);
            if (err == EDL_ERR_CANCELLED)
                return;
            QMetaObject::invokeMethod(this, [this, err]() {
                logEdlResult(QStringLiteral("Erase FRP"), err);
            }, Qt::QueuedConnection);
        }, QStringLiteral("Erase FRP"));
    });

    auto *rebootMenu = ui->rebootBtn->menu();
    if (rebootMenu) {
        const auto actions = rebootMenu->actions();
        for (QAction *action : actions) {
            if (action->isSeparator())
                continue;
            connect(action, &QAction::triggered, this, [this, action]() {
                const QString key = action->data().toString();
                QString rebootLabel = QStringLiteral("重启");
                if (key == QLatin1String("reset"))
                    rebootLabel = QStringLiteral("重启到系统");
                else if (key == QLatin1String("off"))
                    rebootLabel = QStringLiteral("关机");
                else if (key == QLatin1String("misc_fastbootd"))
                    rebootLabel = QStringLiteral("重启到 Fastbootd (MISC)");
                else if (key == QLatin1String("misc_recovery"))
                    rebootLabel = QStringLiteral("重启到 Recovery (MISC)");
                else if (key == QLatin1String("edl_download"))
                    rebootLabel = QStringLiteral("重启到 EDL (9008)");

                if (key == QLatin1String("misc_fastbootd") || key == QLatin1String("misc_recovery")) {
                    const bool toFastbootd = (key == QLatin1String("misc_fastbootd"));
                    runAsync([this, toFastbootd, rebootLabel]() {
                        if (!ensureConnected()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                if (isCancelRequested())
                                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                                else
                                    appendLog(QStringLiteral("MISC 重启错误：设备未连接或握手失败"),
                                              "#C0392B");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (isCancelRequested()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (!preflightFirehose(toFastbootd ? QStringLiteral("Fastbootd (MISC)")
                                                           : QStringLiteral("Recovery (MISC)"))) {
                            return;
                        }

                        const QString path = resolveMiscBootImage(toFastbootd);
                        if (path.isEmpty()) {
                            QMetaObject::invokeMethod(this, [this, toFastbootd]() {
                                appendLog(
                                    toFastbootd
                                        ? QStringLiteral("未找到 Fastbootd 的 MISC 镜像。请在设置中指定路径，"
                                                         "或将 misc_tofastbootd.img 放在程序旁，"
                                                         "或打包进 bundled。")
                                        : QStringLiteral("未找到 Recovery 的 MISC 镜像。请在设置中指定路径，"
                                                         "或将 misc_torecovery.img 放在程序旁，"
                                                         "或打包进 bundled。"),
                                    "#C0392B");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (!ensureGptCacheReady())
                            return;
                        if (isCancelRequested())
                            return;

                        QMetaObject::invokeMethod(this, [this, path]() {
                            const QString fileName = QFileInfo(path).fileName();
                            appendLog(QStringLiteral("MISC 镜像: %1")
                                          .arg(fileName.isEmpty() ? QStringLiteral("(未知文件)") : fileName),
                                      "#6B7280");
                        }, Qt::QueuedConnection);

                        const QByteArray pb = path.toUtf8();
                        edl_error_t err =
                            edl_service_misc_write_image_and_reset(m_edlService, pb.constData(), nullptr);
                        if (err == EDL_ERR_CANCELLED)
                            return;
                        QMetaObject::invokeMethod(this, [this, err]() {
                            if (err == EDL_OK) {
                                appendLog(QStringLiteral("MISC 镜像写入完成，已发送重启"), "#2E8B3E");
                            } else {
                                appendLog(QStringLiteral("MISC 重启失败：%1")
                                              .arg(QString::fromUtf8(edl_error_str(err))),
                                          "#C0392B");
                            }
                        }, Qt::QueuedConnection);
                    }, rebootLabel);
                    return;
                }

                if (key == QLatin1String("edl_download")) {
                    runAsync([this, rebootLabel]() {
                        if (!ensureConnected()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                if (isCancelRequested())
                                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                                else
                                    appendLog(QStringLiteral("EDL 重启错误：设备未连接或握手失败"),
                                              "#C0392B");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (isCancelRequested()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                            }, Qt::QueuedConnection);
                            return;
                        }
                        if (!preflightFirehose(QStringLiteral("power download (EDL)")))
                            return;

                        edl_service_reboot(m_edlService, "download");
                        QMetaObject::invokeMethod(this, [this]() {
                            appendLog(QStringLiteral("已发送 power download；若设备未进入 9008，请改用按键组合或 adb reboot edl"),
                                      "#2E8B3E");
                        }, Qt::QueuedConnection);
                    }, rebootLabel);
                    return;
                }

                QString mode = QStringLiteral("reset");
                if (key == QLatin1String("off")) {
                    mode = QStringLiteral("off");
                } else if (key != QLatin1String("reset")) {
                    const QString text = action->text();
                    if (text.contains(QStringLiteral("off"), Qt::CaseInsensitive))
                        mode = QStringLiteral("off");
                }

                runAsync([this, mode, rebootLabel]() {
                    if (!ensureConnected()) {
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isCancelRequested())
                                appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                            else
                                appendLog(QStringLiteral("电源命令错误：设备未连接或握手失败"),
                                          "#C0392B");
                        }, Qt::QueuedConnection);
                        return;
                    }
                    if (isCancelRequested()) {
                        QMetaObject::invokeMethod(this, [this]() {
                            appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                        }, Qt::QueuedConnection);
                        return;
                    }
                    if (!preflightFirehose(QStringLiteral("电源命令")))
                        return;

                    const QByteArray modeUtf8 = mode.toUtf8();
                    edl_service_reboot(m_edlService, modeUtf8.constData());
                    QMetaObject::invokeMethod(this, [this, mode]() {
                        if (mode == QLatin1String("off"))
                            appendLog(QStringLiteral("关机命令已发送"), "#2E8B3E");
                        else
                            appendLog(QStringLiteral("重启到系统命令已发送"), "#2E8B3E");
                    }, Qt::QueuedConnection);
                }, rebootLabel);
            });
        }
    }
}

static QList<int> selectedRows(QTableWidget *table)
{
    QSet<int> rows;
    for (int r = 0; r < table->rowCount(); ++r) {
        if (table->isRowHidden(r))
            continue;
        auto *nameItem = table->item(r, ColName);
        if (nameItem && nameItem->checkState() == Qt::Checked)
            rows.insert(r);
    }
    QList<int> sorted = rows.values();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

void MainWindow::enqueueMiscVendorWipe(const QString &vendorSlug)
{
    const QString usedFile = QStringLiteral("misc_wipedata_%1.img").arg(vendorSlug);
    const QString path = resolveMiscVendorPackagedPath(usedFile);
    const QString opLabel = QStringLiteral("恢复出厂设置（%1）").arg(vendorSlug);

    runAsync([this, path, vendorSlug, opLabel]() {
        if (!ensureConnected()) {
            QMetaObject::invokeMethod(this, [this]() {
                if (isCancelRequested())
                    appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                else
                    appendLog(QStringLiteral("恢复出厂设置错误：设备未连接或握手失败"), "#C0392B");
            }, Qt::QueuedConnection);
            return;
        }
        if (isCancelRequested()) {
            QMetaObject::invokeMethod(this, [this]() {
                appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
            }, Qt::QueuedConnection);
            return;
        }
        if (!preflightFirehose(opLabel))
            return;

        if (path.isEmpty()) {
            QMetaObject::invokeMethod(this, [this, vendorSlug]() {
                appendLog(QStringLiteral("缺少 misc_wipedata_%1.img。请将它放在程序目录旁，或打包到 bundled/misc_vendor。")
                              .arg(vendorSlug),
                          "#C0392B");
            }, Qt::QueuedConnection);
            return;
        }
        if (!ensureGptCacheReady())
            return;
        if (isCancelRequested())
            return;

        QMetaObject::invokeMethod(this, [this, path]() {
            const QString fileName = QFileInfo(path).fileName();
            appendLog(QStringLiteral("MISC 镜像: %1")
                          .arg(fileName.isEmpty() ? QStringLiteral("(未知文件)") : fileName),
                      "#6B7280");
        }, Qt::QueuedConnection);

        const QByteArray pathUtf8 = path.toUtf8();
        edl_error_t err = edl_service_misc_write_image_and_reset(m_edlService, pathUtf8.constData(), nullptr);
        if (err == EDL_ERR_CANCELLED)
            return;
        QMetaObject::invokeMethod(this, [this, err, opLabel]() {
            if (err == EDL_OK) {
                appendLog(QStringLiteral("%1：MISC 镜像写入完成，已发送重启").arg(opLabel), "#2E8B3E");
            } else {
                appendLog(QStringLiteral("%1失败：%2")
                              .arg(opLabel, QString::fromUtf8(edl_error_str(err))),
                          "#C0392B");
            }
        }, Qt::QueuedConnection);
    }, opLabel);
}

#if 0
void MainWindow::wirePartitionButtons()
{
    /* ---- READ: batch-capable, no confirmation ---- */
    connect(ui->readPartBtn, &QPushButton::clicked, this, [this]() {
        auto rows = selectedRows(ui->partitionTable);
        if (rows.isEmpty()) {
            appendLog("请先在分区表中选择要读取的分区", "#C0392B");
            return;
        }

        QVector<ReadPartitionTask> tasks;
        if (rows.size() == 1) {
            const int r = rows.first();
            QString name = ui->partitionTable->item(r, ColName)->text();
            name.remove(QStringLiteral(" [擦除]")).remove(QStringLiteral(" [清零]"));
            const QString defPath = QDir::homePath() + QLatin1Char('/') + name + QStringLiteral(".img");
            const QString path = QFileDialog::getSaveFileName(
                this,
                QStringLiteral("保存为 IMG 镜像"),
                defPath,
                QStringLiteral("IMG 镜像 (*.img);;原始镜像 (*.bin);;所有文件 (*.*)"));
            if (path.isEmpty())
                return;
            ReadPartitionTask t;
            t.name = name;
            t.path = path;
            fillReadPartitionTaskFromRow(ui->partitionTable, r, t);
            tasks.append(t);
            appendLog(QStringLiteral("开始读取分区 → %1").arg(path), "#6B7280");
        } else {
            QString saveDir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择保存目录（每分区保存为 分区名.img）"));
            if (saveDir.isEmpty())
                return;
            for (int r : rows) {
                QString name = ui->partitionTable->item(r, ColName)->text();
                name.remove(QStringLiteral(" [擦除]")).remove(QStringLiteral(" [清零]"));
                ReadPartitionTask t;
                t.name = name;
                t.path = saveDir + QLatin1Char('/') + name + QStringLiteral(".img");
                fillReadPartitionTaskFromRow(ui->partitionTable, r, t);
                tasks.append(t);
            }
            appendLog(QStringLiteral("开始读取 %1 个分区 → %2（*.img）").arg(tasks.size()).arg(saveDir), "#6B7280");
        }

        runAsync([this, tasks]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog("操作已取消", "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (!preflightFirehose("读取分区前"))
                return;
            if (!ensureGptCacheReady())
                return;

            bool anyFail = false;
            auto logNow = [this, &anyFail](const QString &text, bool ok) {
                if (!ok) anyFail = true;
                QMetaObject::invokeMethod(this, [this, text, ok]() {
                    appendLog(text, ok ? "#2E8B3E" : "#C0392B");
                }, Qt::QueuedConnection);
            };
            for (int i = 0; i < tasks.size(); ++i) {
                if (i > 0 && tasks.size() >= 6 && (i % 5) == 0) {
                    if (!preflightFirehose(QString("批量读取 第 %1 项").arg(i + 1))) {
                        logNow("链路检测失败，已停止后续分区", false);
                        break;
                    }
                }
                const ReadPartitionTask &task = tasks[i];
                const QString &name = task.name;
                const QString &path = task.path;
                if (isCancelRequested()) {
                    logNow("操作已取消（后续分区未执行）", false);
                    break;
                }
                const edl_partition_info_t *partPtr =
                    edl_service_find_partition(m_edlService, name.toUtf8().constData());
                edl_partition_info_t partFromTable;
                bool usedTableFallback = false;
                if (!partPtr && task.hasTableFallback) {
                    std::memset(&partFromTable, 0, sizeof(partFromTable));
                    partFromTable.lun = task.tableLun;
                    partFromTable.start_sector = task.tableStartSector;
                    partFromTable.num_sectors = task.tableNumSectors;
                    partFromTable.sector_size = task.tableSectorSize > 0 ? task.tableSectorSize : 4096;
                    const QByteArray nmu = name.toUtf8();
                    std::strncpy(partFromTable.name, nmu.constData(), sizeof(partFromTable.name) - 1);
                    partFromTable.name[sizeof(partFromTable.name) - 1] = '\0';
                    if (!task.tableStartSectorExpr.isEmpty()) {
                        const QByteArray expr = task.tableStartSectorExpr.toUtf8();
                        std::strncpy(partFromTable.start_sector_expr, expr.constData(),
                                     sizeof(partFromTable.start_sector_expr) - 1);
                        partFromTable.start_sector_expr[sizeof(partFromTable.start_sector_expr) - 1] = '\0';
                    }
                    partPtr = &partFromTable;
                    usedTableFallback = true;
                }
                if (!partPtr) {
                    logNow(QString("读取 %1 分区失败: %2（请点「读取分区表」同步 GPT，"
                                          "或确认该行含 LUN/起始扇区/大小）")
                                      .arg(name, QString::fromUtf8(edl_error_str(EDL_ERR_FH_PARTITION_NOT_FOUND))),
                                  false);
                    break;
                }
                logNow(QString("正在读取 %1…").arg(name), true);
                edl_error_t err = edl_service_read_partition(m_edlService, partPtr, path.toUtf8().constData());
                const QString detail = QString::fromUtf8(edl_error_str(err));
                if (err == EDL_OK) {
                    if (usedTableFallback) {
                        if (edl_service_is_gpt_cache_loaded(m_edlService)) {
                            logNow(QStringLiteral(
                                              "读取 %1 分区 OK（GPT 中无此分区名，已用分区表行 LUN/扇区）")
                                              .arg(name),
                                          true);
                        } else {
                            logNow(QStringLiteral(
                                              "读取 %1 分区 OK（设备未解析 GPT，已用分区表行；"
                                              "列表若来自 XML 可能与全盘扫描结果不一致）")
                                              .arg(name),
                                          true);
                        }
                    } else {
                        logNow(QString("读取 %1 分区 OK").arg(name), true);
                    }
                    continue;
                }
                if (err == EDL_ERR_CANCELLED) {
                    logNow(QString("读取 %1 已取消").arg(name), false);
                    break;
                }
                if (edl_error_is_fail(err)) {
                    logNow(QString("读取 %1 分区失败: %2%3")
                                      .arg(name, detail,
                                           usedTableFallback ? QStringLiteral("（已用分区表回退）")
                                                             : QString()),
                                  false);
                    break;
                }
                logNow(QString("读取 %1 分区错误: %2%3")
                                  .arg(name, detail,
                                       usedTableFallback ? QStringLiteral("（已用分区表回退）")
                                                         : QString()),
                              false);
                if (edl_error_error_stops_batch(err))
                    break;
            }
            if (anyFail && !isCancelRequested())
                markAsyncTaskFailed();
            endBatchProgress();
        }, QStringLiteral("读取分区"));
    });

    /* ---- WRITE: batch via double-click assigned files, no confirmation ---- */
    connect(ui->writePartBtn, &QPushButton::clicked, this, [this]() {
        QVector<WritePartitionTask> tasks;

        auto rows = selectedRows(ui->partitionTable);
        auto fillFallbackFromRow = [this](int r, WritePartitionTask &wt) {
            wt.hasTableFallback = false;
            auto *nameItem = ui->partitionTable->item(r, ColName);
            auto *s0 = ui->partitionTable->item(r, ColStartSector);
            auto *s3 = ui->partitionTable->item(r, ColLun);
            if (!nameItem || !s0 || !s3)
                return;
            bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
            const qint64 startSectorRole = nameItem->data(RoleStartSector).toLongLong(&ok1);
            const qint64 numSectorsRole = nameItem->data(RoleNumSectors).toLongLong(&ok2);
            const int sectorSizeRole = nameItem->data(RoleSectorSize).toInt(&ok3);
            const int lunRole = nameItem->data(RoleLun).toInt(&ok4);

            wt.tableStartSector = ok1 ? startSectorRole : s0->text().trimmed().toLongLong(&ok1);
            wt.tableNumSectors = ok2 ? numSectorsRole : 0;
            wt.tableSectorSize = ok3 && sectorSizeRole > 0 ? sectorSizeRole : 4096;
            int lunParsed = 0;
            const bool lunFromText = parseTableLunCell(s3->text(), &lunParsed);
            wt.tableLun = ok4 ? lunRole : (lunFromText ? lunParsed : 0);
            wt.tableStartSectorExpr = nameItem->data(RoleStartSectorExpr).toString().trimmed();
            wt.hasTableFallback = ok1 && (ok4 || lunFromText);
        };

        for (int r : rows) {
            auto *fileItem = ui->partitionTable->item(r, ColFile);
            QString filePath;
            if (fileItem)
                filePath = fileItem->data(Qt::UserRole).toString();
            if (!filePath.isEmpty()) {
                QString pn = ui->partitionTable->item(r, ColName)->text();
                pn.remove(QStringLiteral(" [擦除]")).remove(QStringLiteral(" [清零]"));
                WritePartitionTask wt;
                wt.name = pn;
                wt.path = filePath;
                fillFallbackFromRow(r, wt);
                tasks.append(wt);
            }
        }

        if (tasks.isEmpty()) {
            if (rows.size() == 1) {
                const int r = rows.first();
                QString partName = ui->partitionTable->item(r, ColName)->text();
                partName.remove(QStringLiteral(" [擦除]")).remove(QStringLiteral(" [清零]"));
                const QString imgPath = QFileDialog::getOpenFileName(
                    this, QString("选择 %1 的镜像").arg(partName), "",
                    "镜像文件 (*.bin *.img *.mbn *.elf);;所有文件 (*.*)");
                if (imgPath.isEmpty())
                    return;
                WritePartitionTask wt;
                wt.name = partName;
                wt.path = imgPath;
                fillFallbackFromRow(r, wt);
                tasks.append(wt);
            } else if (rows.size() > 1) {
                appendLog("批量写入: 请先双击分区行分配镜像文件", "#C0392B");
                return;
            } else {
                appendLog("请先在分区表中选择要写入的分区", "#C0392B");
                return;
            }
        }

        const int total = tasks.size();
        appendLog(QString("开始写入 %1 个分区...").arg(total), "#6B7280");

        {
            QStringList assignedButUnchecked;
            auto *table = ui->partitionTable;
            for (int r = 0; r < table->rowCount(); ++r) {
                if (table->isRowHidden(r))
                    continue;
                auto *ni = table->item(r, ColName);
                auto *fi = table->item(r, ColFile);
                if (!ni || !fi)
                    continue;
                const QString path = fi->data(Qt::UserRole).toString().trimmed();
                if (path.isEmpty())
                    continue;
                if (ni->checkState() != Qt::Checked) {
                    QString pn = ni->text();
                    pn.remove(QStringLiteral(" [擦除]")).remove(QStringLiteral(" [清零]"));
                    assignedButUnchecked.append(pn);
                }
            }
            if (!assignedButUnchecked.isEmpty()) {
                appendLog(QStringLiteral("【检查】下列分区已分配镜像文件但未勾选，本次不会写入：%1")
                              .arg(assignedButUnchecked.join(QStringLiteral("、"))),
                          QStringLiteral("#C0392B"));
            }
        }

        runAsync([this, tasks]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog("操作已取消", "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (!preflightFirehose("写入分区前"))
                return;
            if (!ensureGptCacheReady())
                return;
            bool fileLenOnly = false;
            QMetaObject::invokeMethod(
                this,
                [&fileLenOnly, this]() {
                    fileLenOnly = ui->writeFileLengthOnlyCheckBox->isChecked();
                },
                Qt::BlockingQueuedConnection);
            edl_service_set_write_options(m_edlService,
                                          !fileLenOnly,
                                          false);
            bool anyFail = false;
            auto logNow = [this, &anyFail](const QString &text, bool ok) {
                if (!ok) anyFail = true;
                QMetaObject::invokeMethod(this, [this, text, ok]() {
                    appendLog(text, ok ? "#2E8B3E" : "#C0392B");
                }, Qt::QueuedConnection);
            };
            bool finishedAllTasks = true;
            bool wroteRealData = false;
            bool wroteGptLayout = false;
            for (int i = 0; i < tasks.size(); ++i) {
                if (i > 0 && tasks.size() >= 6 && (i % 5) == 0) {
                    if (!preflightFirehose(QString("批量写入 第 %1 项").arg(i + 1))) {
                        logNow("链路检测失败，已停止后续分区", false);
                        finishedAllTasks = false;
                        break;
                    }
                }
                const WritePartitionTask &wt = tasks[i];
                const QString &name = wt.name;
                const QString &path = wt.path;
                if (isCancelRequested()) {
                    logNow("操作已取消（后续分区未执行）", false);
                    finishedAllTasks = false;
                    break;
                }
                const qint64 logicalBytes = inferImageLogicalBytes(path);
                if (logicalBytes <= 0) {
                    logNow(QString("写入 %1 跳过：镜像为 0 字节（不写入、不填充）").arg(name),
                                  true);
                    continue;
                }

                edl_partition_info_t partFromTable;
                const edl_partition_info_t *partPtr = nullptr;
                if (wt.hasTableFallback) {
                    std::memset(&partFromTable, 0, sizeof(partFromTable));
                    partFromTable.lun = wt.tableLun;
                    partFromTable.start_sector = wt.tableStartSector;
                    partFromTable.num_sectors = wt.tableNumSectors;
                    partFromTable.sector_size = wt.tableSectorSize > 0 ? wt.tableSectorSize : 4096;
                    const QByteArray nmu = name.toUtf8();
                    strncpy(partFromTable.name, nmu.constData(), sizeof(partFromTable.name) - 1);
                    partFromTable.name[sizeof(partFromTable.name) - 1] = '\0';
                    if (!wt.tableStartSectorExpr.isEmpty()) {
                        const QByteArray expr = wt.tableStartSectorExpr.toUtf8();
                        strncpy(partFromTable.start_sector_expr, expr.constData(),
                                sizeof(partFromTable.start_sector_expr) - 1);
                        partFromTable.start_sector_expr[sizeof(partFromTable.start_sector_expr) - 1] = '\0';
                    }
                    partPtr = &partFromTable;
                } else {
                    partPtr = edl_service_find_partition(m_edlService, name.toUtf8().constData());
                }
                if (!partPtr) {
                    logNow(QString("写入 %1 失败: GPT 中无此分区，且未从分区表读取到 LUN/起始扇区"
                                          "（rawprogram 任务如 PrimaryGPT 请从 XML 加载表格）")
                                      .arg(name),
                                  false);
                    finishedAllTasks = false;
                    break;
                }

                logNow(QString("正在写入 %1…").arg(name), true);
                const edl_error_t err =
                    edl_service_write_partition(m_edlService, partPtr, path.toUtf8().constData());
                const QString detail = QString::fromUtf8(edl_error_str(err));
                if (err == EDL_OK) {
                    wroteRealData = true;
                    if (name.compare(QStringLiteral("PrimaryGPT"), Qt::CaseInsensitive) == 0
                        || name.compare(QStringLiteral("BackupGPT"), Qt::CaseInsensitive) == 0) {
                        wroteGptLayout = true;
                    }
                    logNow(QString("写入 %1 分区 OK").arg(name), true);
                    continue;
                }
                if (err == EDL_ERR_CANCELLED) {
                    logNow(QString("写入 %1 已取消").arg(name), false);
                    finishedAllTasks = false;
                    break;
                }
                if (edl_error_is_fail(err)) {
                    logNow(QString("写入 %1 分区失败: %2").arg(name, detail), false);
                    finishedAllTasks = false;
                    break;
                }
                logNow(QString("写入 %1 分区错误: %2").arg(name, detail), false);
                if (edl_error_error_stops_batch(err)) {
                    finishedAllTasks = false;
                    break;
                }
            }
            /* 写入 →（可选）patch →（可选）fixgpt → 回读分区表 →（可选）激活启动分区 */
            if (finishedAllTasks && wroteRealData) {
                QString patchLine;
                QMetaObject::invokeMethod(this, [&patchLine, this]() {
                    patchLine = ui->patchLineEdit->text();
                }, Qt::BlockingQueuedConnection);
                const QStringList patchRaw =
                    patchLine.split(QLatin1Char(';'), Qt::SkipEmptyParts);
                QStringList patchPaths;
                QSet<QString> seenPatchKeys;
                for (const QString &pfp : patchRaw) {
                    const QString p = pfp.trimmed();
                    if (p.isEmpty())
                        continue;
                    QFileInfo fi(p);
                    const QString key = fi.exists() ? fi.canonicalFilePath() : p;
                    if (seenPatchKeys.contains(key))
                        continue;
                    seenPatchKeys.insert(key);
                    patchPaths.append(p);
                }
                if (!patchPaths.isEmpty() && !isCancelRequested()) {
                    if (!preflightFirehose(QStringLiteral("应用补丁前"))) {
                        logNow(QStringLiteral("补丁跳过：链路检测失败"), false);
                    } else {
                        for (const QString &pfp : patchPaths) {
                            if (isCancelRequested()) {
                                logNow(QStringLiteral("补丁应用已取消（后续补丁跳过）"), false);
                                break;
                            }
                            const QString p = pfp.trimmed();
                            if (p.isEmpty())
                                continue;
                            if (!QFileInfo::exists(p)) {
                                logNow(QStringLiteral("补丁文件不存在（跳过）: %1").arg(p), false);
                                continue;
                            }
                            edl_error_t perr =
                                edl_service_apply_patch_file(m_edlService, p.toUtf8().constData());
                            if (perr != EDL_OK) {
                                logNow(QStringLiteral("应用补丁失败：%1")
                                           .arg(QString::fromUtf8(edl_error_str(perr))),
                                       false);
                                finishedAllTasks = false;
                                break;
                            }
                            logNow(QStringLiteral("应用补丁成功：%1").arg(p), true);
                        }
                    }
                }

                if (finishedAllTasks && wroteRealData && !isCancelRequested()) {
                    bool doFixGpt = true;
                    QMetaObject::invokeMethod(
                        this,
                        [&doFixGpt, this]() {
                            doFixGpt = ui->fixGptAfterWriteCheckBox->isChecked();
                        },
                        Qt::BlockingQueuedConnection);

                    bool runFixGpt = doFixGpt;
                    if (tasks.size() == 1) {
                        const QString n = tasks[0].name;
                        const bool isGptNamed =
                            (n.compare(QStringLiteral("PrimaryGPT"), Qt::CaseInsensitive) == 0)
                            || (n.compare(QStringLiteral("BackupGPT"), Qt::CaseInsensitive) == 0);
                        if (!isGptNamed)
                            runFixGpt = false;
                    }

                    if (runFixGpt) {
                        if (!preflightFirehose(QStringLiteral("修复 GPT 前"))) {
                            logNow(QStringLiteral("GPT 修复跳过：链路检测失败"), false);
                        } else {
                            edl_error_t fixErr = edl_service_fix_gpt(m_edlService);
                            if (fixErr == EDL_OK) {
                                logNow(QStringLiteral("修复 GPT 成功（主备同步 + CRC）"), true);
                            } else {
                                logNow(QStringLiteral("修复 GPT FAIL（可能影响启动）: %1")
                                                  .arg(QString::fromUtf8(edl_error_str(fixErr))),
                                              false);
                            }
                        }
                    } else {
                        if (doFixGpt && tasks.size() == 1) {
                            logNow(QStringLiteral("已跳过 fixgpt（单分区写入非 PrimaryGPT/BackupGPT）"),
                                          true);
                        } else if (!doFixGpt) {
                            logNow(QStringLiteral("已跳过 fixgpt（未勾选「写入后执行 fixgpt」）"), true);
                        } else {
                            logNow(QStringLiteral("已跳过 fixgpt"), true);
                        }
                    }

#if 0
                    const bool shouldRefreshGptView = wroteGptLayout || runFixGpt;
                    if (isCancelRequested()) { /* skip reread */ }
                    else if (!shouldRefreshGptView) {
                        logNow(QStringLiteral("已跳过分区表刷新：布局未变化"), true); /*
                        logNow(QStringLiteral("宸茶烦杩囧垎鍖鸿〃鍥炶锛堟湰娆℃湭鏀瑰姩 GPT 甯冨眬锛?), true);
                        */
                    } else {
                    edl_partition_info_t parts[256];
                    int gptCount = 256;
                    QString storageStr2;
                    QMetaObject::invokeMethod(
                        this,
                        [&storageStr2, this]() {
                            int idx = ui->storageCombo->currentIndex();
                            if (idx == 1)
                                storageStr2 = QStringLiteral("emmc");
                            else if (idx == 2)
                                storageStr2 = QStringLiteral("ufs");
                        },
                        Qt::BlockingQueuedConnection);
                    const int maxLunR = (storageStr2 == QStringLiteral("emmc")) ? 1 : 24;
                    edl_error_t rerr = edl_service_read_gpt_ex(m_edlService, parts, &gptCount, maxLunR, 0u);
                    if (rerr == EDL_OK) {
                        QVector<edl_partition_info_t> partVec(parts, parts + gptCount);
                        QMetaObject::invokeMethod(
                            this,
                            [this, partVec]() {
                                populatePartitionTable(partVec.data(), partVec.size());
                            },
                            Qt::QueuedConnection); /*
                        logNow(QStringLiteral("回读分区表成功（已刷新列表）"), true);
                    */  logNow(QStringLiteral("分区表刷新完成"), true); } else { /*
                        logNow(QStringLiteral("回读分区表失败（请手动点「读取分区表」）: %1")
                                          .arg(QString::fromUtf8(edl_error_str(rerr))),
                                      false);
                    */  logNow(QStringLiteral("安全模式刷新分区表失败：%1")
                                   .arg(QString::fromUtf8(edl_error_str(rerr))),
                               false); }
                    } /* else: reread */
#endif
                    const bool shouldRefreshGptView = wroteGptLayout || runFixGpt;
                    if (isCancelRequested()) {
                        /* skip reread */
                    } else if (!shouldRefreshGptView) {
                        logNow(QStringLiteral("已跳过分区表刷新：布局未变化"), true);
                    } else {
                        edl_partition_info_t parts[256];
                        int gptCount = 256;
                        QString storageStr2;
                        QMetaObject::invokeMethod(
                            this,
                            [&storageStr2, this]() {
                                int idx = ui->storageCombo->currentIndex();
                                if (idx == 1)
                                    storageStr2 = QStringLiteral("emmc");
                                else if (idx == 2)
                                    storageStr2 = QStringLiteral("ufs");
                            },
                            Qt::BlockingQueuedConnection);
                        const int maxLunR = (storageStr2 == QStringLiteral("emmc")) ? 1 : 24;
                        edl_error_t rerr =
                            edl_service_read_gpt_ex(m_edlService, parts, &gptCount, maxLunR, 0u);
                        if (rerr == EDL_OK) {
                            QVector<edl_partition_info_t> partVec(parts, parts + gptCount);
                            QMetaObject::invokeMethod(
                                this,
                                [this, partVec]() {
                                    populatePartitionTable(partVec.data(), partVec.size());
                                },
                                Qt::QueuedConnection);
                            logNow(QStringLiteral("分区表刷新完成"), true);
                        } else {
                            logNow(QStringLiteral("安全模式刷新分区表失败：%1")
                                       .arg(QString::fromUtf8(edl_error_str(rerr))),
                                   false);
                        }
                    }
                }
            }

            if (anyFail && !isCancelRequested())
                markAsyncTaskFailed();
            endBatchProgress();
        }, QStringLiteral("写入分区"));
    });

    /* ---- ERASE: batch-capable, requires confirmation with strong warning ---- */
    connect(ui->erasePartBtn, &QPushButton::clicked, this, [this]() {
        auto rows = selectedRows(ui->partitionTable);
        if (rows.isEmpty()) {
            appendLog("请先在分区表中选择要擦除的分区", "#C0392B");
            return;
        }

        QStringList names;
        for (int r : rows) {
            QString n = ui->partitionTable->item(r, ColName)->text();
            n.remove(QStringLiteral(" [擦除]")).remove(QStringLiteral(" [清零]"));
            names << n;
        }

        auto ret = QMessageBox::warning(this, "擦除确认",
            QString("即将擦除以下 %1 个分区:\n\n%2\n\n"
                    "此操作不可逆！擦除后数据将永久丢失。\n"
                    "如果您不清楚后果，请勿继续。\n\n"
                    "确认擦除？")
                .arg(names.size()).arg(names.join(", ")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;

        const QList<int> rowList = rows;
        runAsync([this, rowList]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog("操作已取消", "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (!preflightFirehose("擦除分区前"))
                return;
            if (!ensureGptCacheReady())
                return;
            bool anyFail = false;
            auto logNow = [this, &anyFail](const QString &text, bool ok) {
                if (!ok) anyFail = true;
                QMetaObject::invokeMethod(this, [this, text, ok]() {
                    appendLog(text, ok ? "#2E8B3E" : "#C0392B");
                }, Qt::QueuedConnection);
            };
            for (int i = 0; i < rowList.size(); ++i) {
                if (i > 0 && rowList.size() >= 6 && (i % 5) == 0) {
                    if (!preflightFirehose(QString("批量擦除 第 %1 项").arg(i + 1))) {
                        logNow("链路检测失败，已停止后续分区", false);
                        break;
                    }
                }
                const int r = rowList[i];
                if (isCancelRequested()) {
                    logNow("操作已取消（后续分区未执行）", false);
                    break;
                }
                QString name = ui->partitionTable->item(r, ColName)->text();
                name.remove(QStringLiteral(" [擦除]")).remove(QStringLiteral(" [清零]"));

                ReadPartitionTask wt;
                fillReadPartitionTaskFromRow(ui->partitionTable, r, wt);

                const edl_partition_info_t *partPtr =
                    edl_service_find_partition(m_edlService, name.toUtf8().constData());
                edl_partition_info_t partFromTable;
                bool usedTableFallback = false;
                if (!partPtr && wt.hasTableFallback) {
                    std::memset(&partFromTable, 0, sizeof(partFromTable));
                    partFromTable.lun = wt.tableLun;
                    partFromTable.start_sector = wt.tableStartSector;
                    partFromTable.num_sectors = wt.tableNumSectors;
                    partFromTable.sector_size = wt.tableSectorSize > 0 ? wt.tableSectorSize : 4096;
                    const QByteArray nmu = name.toUtf8();
                    strncpy(partFromTable.name, nmu.constData(), sizeof(partFromTable.name) - 1);
                    partFromTable.name[sizeof(partFromTable.name) - 1] = '\0';
                    if (!wt.tableStartSectorExpr.isEmpty()) {
                        const QByteArray expr = wt.tableStartSectorExpr.toUtf8();
                        strncpy(partFromTable.start_sector_expr, expr.constData(),
                                sizeof(partFromTable.start_sector_expr) - 1);
                        partFromTable.start_sector_expr[sizeof(partFromTable.start_sector_expr) - 1] = '\0';
                    }
                    partPtr = &partFromTable;
                    usedTableFallback = true;
                }
                if (!partPtr) {
                    logNow(QStringLiteral("擦除 %1 分区失败: GPT 中无此分区且该行无 LUN/扇区回退"
                                               "（请点「读取分区表」或从 XML 加载表格）")
                                      .arg(name),
                                  false);
                    break;
                }

                logNow(QString("正在擦除 %1…").arg(name), true);
                const qint64 partBytes = taskBytes.value(i);
                if (batchTotalBytes > 0 && partBytes > 0) {
                    qint64 currentBaseBytes = 0;
                    for (int j = 0; j < i; ++j)
                        currentBaseBytes += taskBytes.value(j);
                    setBatchProgressWindow(currentBaseBytes, partBytes);
                }
                const qint64 partBytes = taskBytes.value(i);
                if (batchTotalBytes > 0 && partBytes > 0)
                    setBatchProgressWindow(batchBaseBytes, partBytes);
                const qint64 partBytes = taskBytes.value(i);
                if (batchTotalBytes > 0 && partBytes > 0)
                    setBatchProgressWindow(batchBaseBytes, partBytes);
                const qint64 partBytes = taskBytes.value(i);
                if (batchTotalBytes > 0 && partBytes > 0)
                    setBatchProgressWindow(batchBaseBytes, partBytes);
                edl_error_t err = edl_service_erase_partition(m_edlService, partPtr);
                const QString detail = QString::fromUtf8(edl_error_str(err));
                if (err == EDL_OK && partBytes > 0)
                    batchBaseBytes += partBytes;
                if (err == EDL_OK && partBytes > 0)
                    batchBaseBytes += partBytes;
                if (err == EDL_OK && partBytes > 0)
                    batchBaseBytes += partBytes;
                if (err == EDL_OK && partBytes > 0)
                    batchBaseBytes += partBytes;
                if (err == EDL_OK) {
                    if (usedTableFallback) {
                        if (edl_service_is_gpt_cache_loaded(m_edlService)) {
                            logNow(QStringLiteral("擦除 %1 分区 OK（GPT 中无此名，已用分区表行）").arg(name),
                                          true);
                        } else {
                            logNow(QStringLiteral(
                                     "擦除 %1 分区 OK（设备未解析 GPT，已用分区表行；与读取/写入一致）")
                                     .arg(name),
                                 true);
                        }
                    } else {
                        logNow(QString("擦除 %1 分区 OK").arg(name), true);
                    }
                    continue;
                }
                if (err == EDL_ERR_CANCELLED) {
                    logNow(QString("擦除 %1 已取消").arg(name), false);
                    break;
                }
                if (edl_error_is_fail(err)) {
                    logNow(QString("擦除 %1 分区失败: %2").arg(name, detail), false);
                    break;
                }
                logNow(QString("擦除 %1 分区错误: %2").arg(name, detail), false);
                if (edl_error_error_stops_batch(err))
                    break;
            }
            if (anyFail && !isCancelRequested())
                markAsyncTaskFailed();
        }, QStringLiteral("擦除分区"));
    });
}

#endif
void MainWindow::wirePartitionButtons()
{
    connect(ui->readPartBtn, &QPushButton::clicked, this, [this]() {
        const QList<int> rows = selectedRows(ui->partitionTable);
        if (rows.isEmpty()) {
            appendLog(QStringLiteral("请先选择要读取的分区行"), "#C0392B");
            return;
        }

        QVector<ReadPartitionTask> tasks;
        if (rows.size() == 1) {
            const int row = rows.first();
            QString name = ui->partitionTable->item(row, ColName)->text();
            name.remove(QStringLiteral(" [鎿﹂櫎]")).remove(QStringLiteral(" [娓呴浂]"));
            const QString defaultPath = QDir::homePath() + QLatin1Char('/') + name + QStringLiteral(".img");
            const QString path = QFileDialog::getSaveFileName(
                this,
                QStringLiteral("保存分区镜像"),
                defaultPath,
                QStringLiteral("IMG 镜像 (*.img);;原始镜像 (*.bin);;所有文件 (*.*)"));
            if (path.isEmpty())
                return;

            ReadPartitionTask task;
            task.name = name;
            task.path = path;
            fillReadPartitionTaskFromRow(ui->partitionTable, row, task);
            tasks.append(task);
            appendLog(QStringLiteral("开始读取分区 -> %1").arg(path), "#6B7280");
        } else {
            const QString saveDir = QFileDialog::getExistingDirectory(
                this,
                QStringLiteral("选择输出目录（每个分区保存为 <name>.img）"));
            if (saveDir.isEmpty())
                return;

            for (int row : rows) {
                QString name = ui->partitionTable->item(row, ColName)->text();
                name.remove(QStringLiteral(" [鎿﹂櫎]")).remove(QStringLiteral(" [娓呴浂]"));
                ReadPartitionTask task;
                task.name = name;
                task.path = saveDir + QLatin1Char('/') + name + QStringLiteral(".img");
                fillReadPartitionTaskFromRow(ui->partitionTable, row, task);
                tasks.append(task);
            }
            appendLog(QStringLiteral("开始读取 %1 个分区 -> %2")
                          .arg(tasks.size())
                          .arg(saveDir),
                      "#6B7280");
        }

        runAsync([this, tasks]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (!preflightFirehose(QStringLiteral("Read partition")))
                return;
            if (!ensureGptCacheReady())
                return;

            bool anyFail = false;
            auto logNow = [this, &anyFail](const QString &text, bool ok) {
                if (!ok)
                    anyFail = true;
                QMetaObject::invokeMethod(this, [this, text, ok]() {
                    appendLog(text, ok ? "#2E8B3E" : "#C0392B");
                }, Qt::QueuedConnection);
            };

            QVector<qint64> taskBytes(tasks.size(), 0);
            qint64 batchTotalBytes = 0;
            for (int i = 0; i < tasks.size(); ++i) {
                edl_partition_info_t previewPart{};
                bool previewFallback = false;
                const edl_partition_info_t *previewPtr =
                    resolveReadPartitionTask(m_edlService, tasks[i], &previewPart, &previewFallback);
                const qint64 bytes = safePartitionLogicalBytes(
                    previewPtr,
                    tasks[i].tableSectorSize > 0 ? tasks[i].tableSectorSize : 4096);
                taskBytes[i] = bytes;
                if (bytes > 0) {
                    const qint64 limit = (std::numeric_limits<qint64>::max)() - batchTotalBytes;
                    batchTotalBytes += bytes > limit ? limit : bytes;
                }
            }
            beginBatchProgress(batchTotalBytes);
            qint64 batchBaseBytes = 0;

            for (int i = 0; i < tasks.size(); ++i) {
                if (i > 0 && tasks.size() >= 6 && (i % 5) == 0) {
                    if (!preflightFirehose(QStringLiteral("Batch read item %1").arg(i + 1))) {
                        logNow(QStringLiteral("链路检查失败：已停止后续分区操作"), false);
                        break;
                    }
                }

                const ReadPartitionTask &task = tasks[i];
                const QString &name = task.name;
                const QString &path = task.path;
                if (isCancelRequested()) {
                    logNow(QStringLiteral("操作已取消；其余分区已跳过"), false);
                    break;
                }

                edl_partition_info_t partFromTable{};
                bool usedTableFallback = false;
                const edl_partition_info_t *partPtr =
                    resolveReadPartitionTask(m_edlService, task, &partFromTable, &usedTableFallback);
                if (!partPtr) {
                    logNow(QStringLiteral("读取 %1 分区失败：GPT 中无此分区，且没有可用的表格回退数据")
                               .arg(name),
                           false);
                    break;
                }

                logNow(QStringLiteral("正在读取 %1").arg(name), true);
                const qint64 partBytes = taskBytes.value(i);
                if (batchTotalBytes > 0 && partBytes > 0)
                    setBatchProgressWindow(batchBaseBytes, partBytes);
                const QByteArray pathUtf8 = path.toUtf8();
                edl_error_t err = edl_service_read_partition(m_edlService, partPtr, pathUtf8.constData());
                const QString detail = QString::fromUtf8(edl_error_str(err));
                if (err == EDL_OK && partBytes > 0)
                    batchBaseBytes += partBytes;
                if (err == EDL_OK) {
                    if (usedTableFallback) {
                        if (edl_service_is_gpt_cache_loaded(m_edlService)) {
                            logNow(QStringLiteral("读取 %1 成功（GPT 名称查找未命中，已使用表格回退行）")
                                       .arg(name),
                                   true);
                        } else {
                            logNow(QStringLiteral("读取 %1 成功（设备 GPT 未加载，已使用表格回退行）")
                                       .arg(name),
                                   true);
                        }
                    } else {
                        logNow(QStringLiteral("读取 %1 成功").arg(name), true);
                    }
                    continue;
                }
                if (err == EDL_ERR_CANCELLED) {
                    logNow(QStringLiteral("读取 %1 已取消").arg(name), false);
                    break;
                }
                const QString suffix =
                    usedTableFallback ? QStringLiteral("（已使用表格回退）") : QString();
                if (edl_error_is_fail(err)) {
                    logNow(QStringLiteral("读取 %1 失败：%2%3").arg(name, detail, suffix), false);
                    break;
                }
                logNow(QStringLiteral("读取 %1 错误：%2%3").arg(name, detail, suffix), false);
                if (edl_error_error_stops_batch(err))
                    break;
            }

            if (anyFail && !isCancelRequested())
                markAsyncTaskFailed();
            endBatchProgress();
        }, QStringLiteral("Read partition"));
    });

    connect(ui->writePartBtn, &QPushButton::clicked, this, [this]() {
        QVector<WritePartitionTask> tasks;
        const QList<int> rows = selectedRows(ui->partitionTable);
        auto fillFallbackFromRow = [this](int row, WritePartitionTask &task) {
            task.hasTableFallback = false;
            auto *nameItem = ui->partitionTable->item(row, ColName);
            auto *startItem = ui->partitionTable->item(row, ColStartSector);
            auto *lunItem = ui->partitionTable->item(row, ColLun);
            if (!nameItem || !startItem || !lunItem)
                return;

            bool ok1 = false;
            bool ok2 = false;
            bool ok3 = false;
            bool ok4 = false;
            const qint64 startSectorRole = nameItem->data(RoleStartSector).toLongLong(&ok1);
            const qint64 numSectorsRole = nameItem->data(RoleNumSectors).toLongLong(&ok2);
            const int sectorSizeRole = nameItem->data(RoleSectorSize).toInt(&ok3);
            const int lunRole = nameItem->data(RoleLun).toInt(&ok4);

            task.tableStartSector = ok1 ? startSectorRole : startItem->text().trimmed().toLongLong(&ok1);
            task.tableNumSectors = ok2 ? numSectorsRole : 0;
            task.tableSectorSize = ok3 && sectorSizeRole > 0 ? sectorSizeRole : 4096;
            int lunParsed = 0;
            const bool lunFromText = parseTableLunCell(lunItem->text(), &lunParsed);
            task.tableLun = ok4 ? lunRole : (lunFromText ? lunParsed : 0);
            task.tableStartSectorExpr = nameItem->data(RoleStartSectorExpr).toString().trimmed();
            task.hasTableFallback = ok1 && (ok4 || lunFromText);
        };

        for (int row : rows) {
            auto *fileItem = ui->partitionTable->item(row, ColFile);
            QString filePath;
            if (fileItem)
                filePath = fileItem->data(Qt::UserRole).toString();
            if (!filePath.isEmpty()) {
                QString name = ui->partitionTable->item(row, ColName)->text();
                name.remove(QStringLiteral(" [鎿﹂櫎]")).remove(QStringLiteral(" [娓呴浂]"));
                WritePartitionTask task;
                task.name = name;
                task.path = filePath;
                fillFallbackFromRow(row, task);
                tasks.append(task);
            }
        }

        if (tasks.isEmpty()) {
            if (rows.size() == 1) {
                const int row = rows.first();
                QString partName = ui->partitionTable->item(row, ColName)->text();
                partName.remove(QStringLiteral(" [鎿﹂櫎]")).remove(QStringLiteral(" [娓呴浂]"));
                const QString imgPath = QFileDialog::getOpenFileName(
                    this,
                    QStringLiteral("选择 %1 的镜像").arg(partName),
                    QString(),
                    QStringLiteral("镜像文件 (*.bin *.img *.mbn *.elf);;所有文件 (*.*)"));
                if (imgPath.isEmpty())
                    return;

                WritePartitionTask task;
                task.name = partName;
                task.path = imgPath;
                fillFallbackFromRow(row, task);
                tasks.append(task);
            } else if (rows.size() > 1) {
                appendLog(QStringLiteral("批量写入：请先双击分区行分配镜像文件"), "#C0392B");
                return;
            } else {
                appendLog(QStringLiteral("请先选择要写入的分区行"), "#C0392B");
                return;
            }
        }

        appendLog(QStringLiteral("开始写入 %1 个分区...").arg(tasks.size()), "#6B7280");
        {
            QStringList assignedButUnchecked;
            auto *table = ui->partitionTable;
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->isRowHidden(row))
                    continue;
                auto *nameItem = table->item(row, ColName);
                auto *fileItem = table->item(row, ColFile);
                if (!nameItem || !fileItem)
                    continue;

                const QString path = fileItem->data(Qt::UserRole).toString().trimmed();
                if (path.isEmpty())
                    continue;
                if (nameItem->checkState() != Qt::Checked) {
                    QString name = nameItem->text();
                    name.remove(QStringLiteral(" [鎿﹂櫎]")).remove(QStringLiteral(" [娓呴浂]"));
                    assignedButUnchecked.append(name);
                }
            }
            if (!assignedButUnchecked.isEmpty()) {
                appendLog(QStringLiteral("以下分区已分配镜像但未勾选，本次已跳过: %1")
                              .arg(assignedButUnchecked.join(QStringLiteral(", "))),
                          "#C0392B");
            }
        }

        runAsync([this, tasks]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (!preflightFirehose(QStringLiteral("Write partition")))
                return;
            if (!ensureGptCacheReady())
                return;

            bool fileLenOnly = false;
            QMetaObject::invokeMethod(this, [&fileLenOnly, this]() {
                fileLenOnly = ui->writeFileLengthOnlyCheckBox->isChecked();
            }, Qt::BlockingQueuedConnection);
            edl_service_set_write_options(m_edlService, !fileLenOnly, false);

            bool anyFail = false;
            auto logNow = [this, &anyFail](const QString &text, bool ok) {
                if (!ok)
                    anyFail = true;
                QMetaObject::invokeMethod(this, [this, text, ok]() {
                    appendLog(text, ok ? "#2E8B3E" : "#C0392B");
                }, Qt::QueuedConnection);
            };

            QVector<qint64> taskBytes(tasks.size(), 0);
            qint64 batchTotalBytes = 0;
            for (int i = 0; i < tasks.size(); ++i) {
                const qint64 logicalBytes = inferImageLogicalBytes(tasks[i].path);
                edl_partition_info_t previewPart{};
                const edl_partition_info_t *previewPtr =
                    resolveWritePartitionTask(m_edlService, tasks[i], &previewPart);
                const qint64 bytes = estimateWriteProgressBytes(
                    logicalBytes,
                    previewPtr,
                    fileLenOnly,
                    tasks[i].tableSectorSize > 0 ? tasks[i].tableSectorSize : 4096);
                taskBytes[i] = bytes;
                if (bytes > 0) {
                    const qint64 limit = (std::numeric_limits<qint64>::max)() - batchTotalBytes;
                    batchTotalBytes += bytes > limit ? limit : bytes;
                }
            }
            beginBatchProgress(batchTotalBytes);
            qint64 batchBaseBytes = 0;

            bool finishedAllTasks = true;
            bool wroteRealData = false;
            bool wroteGptLayout = false;
            for (int i = 0; i < tasks.size(); ++i) {
                if (i > 0 && tasks.size() >= 6 && (i % 5) == 0) {
                    if (!preflightFirehose(QStringLiteral("Batch write item %1").arg(i + 1))) {
                        logNow(QStringLiteral("链路检查失败：已停止后续分区操作"), false);
                        finishedAllTasks = false;
                        break;
                    }
                }

                const WritePartitionTask &task = tasks[i];
                const QString &name = task.name;
                const QString &path = task.path;
                if (isCancelRequested()) {
                    logNow(QStringLiteral("操作已取消；其余分区已跳过"), false);
                    finishedAllTasks = false;
                    break;
                }

                const qint64 logicalBytes = inferImageLogicalBytes(path);
                if (logicalBytes <= 0) {
                    logNow(QStringLiteral("写入 %1 已跳过：镜像大小为 0 字节").arg(name), true);
                    continue;
                }

                edl_partition_info_t partFromTable{};
                const edl_partition_info_t *partPtr =
                    resolveWritePartitionTask(m_edlService, task, &partFromTable);
                if (!partPtr) {
                    logNow(QStringLiteral("写入 %1 失败：GPT 中无此分区，且没有可用的表格回退数据")
                               .arg(name),
                           false);
                    finishedAllTasks = false;
                    break;
                }

                logNow(QStringLiteral("正在写入 %1").arg(name), true);
                const QByteArray pathUtf8 = path.toUtf8();
                const qint64 partBytes = taskBytes.value(i);
                if (batchTotalBytes > 0 && partBytes > 0)
                    setBatchProgressWindow(batchBaseBytes, partBytes);
                const edl_error_t err =
                    edl_service_write_partition(m_edlService, partPtr, pathUtf8.constData());
                const QString detail = QString::fromUtf8(edl_error_str(err));
                if (err == EDL_OK && partBytes > 0)
                    batchBaseBytes += partBytes;
                if (err == EDL_OK) {
                    wroteRealData = true;
                    if (name.compare(QStringLiteral("PrimaryGPT"), Qt::CaseInsensitive) == 0
                        || name.compare(QStringLiteral("BackupGPT"), Qt::CaseInsensitive) == 0) {
                        wroteGptLayout = true;
                    }
                    logNow(QStringLiteral("写入 %1 成功").arg(name), true);
                    continue;
                }
                if (err == EDL_ERR_CANCELLED) {
                    logNow(QStringLiteral("写入 %1 已取消").arg(name), false);
                    finishedAllTasks = false;
                    break;
                }
                if (edl_error_is_fail(err)) {
                    logNow(QStringLiteral("写入 %1 失败：%2").arg(name, detail), false);
                    finishedAllTasks = false;
                    break;
                }
                logNow(QStringLiteral("写入 %1 错误：%2").arg(name, detail), false);
                if (edl_error_error_stops_batch(err)) {
                    finishedAllTasks = false;
                    break;
                }
            }

            if (batchTotalBytes > 0 && batchBaseBytes >= batchTotalBytes) {
                setBatchProgressWindow(batchTotalBytes, 1);
                relayEdlProgressFromCore(1, 1);
            }

            if (finishedAllTasks && wroteRealData) {
                QString patchLine;
                QMetaObject::invokeMethod(this, [&patchLine, this]() {
                    patchLine = ui->patchLineEdit->text();
                }, Qt::BlockingQueuedConnection);

                const QStringList patchRaw = patchLine.split(QLatin1Char(';'), Qt::SkipEmptyParts);
                QStringList patchPaths;
                QSet<QString> seenPatchKeys;
                for (const QString &entry : patchRaw) {
                    const QString patchPath = entry.trimmed();
                    if (patchPath.isEmpty())
                        continue;
                    QFileInfo fi(patchPath);
                    const QString key = fi.exists() ? fi.canonicalFilePath() : patchPath;
                    if (seenPatchKeys.contains(key))
                        continue;
                    seenPatchKeys.insert(key);
                    patchPaths.append(patchPath);
                }

                if (!patchPaths.isEmpty() && !isCancelRequested()) {
                    if (!preflightFirehose(QStringLiteral("Apply patch"))) {
                        logNow(QStringLiteral("链路检查失败：已跳过补丁阶段"), false);
                    } else {
                        for (const QString &patchPath : patchPaths) {
                            if (isCancelRequested()) {
                                logNow(QStringLiteral("补丁阶段已取消：后续补丁已跳过"),
                                       false);
                                break;
                            }
                            if (!QFileInfo::exists(patchPath)) {
                                logNow(QStringLiteral("补丁文件不存在，已跳过：%1")
                                           .arg(patchPath),
                                       false);
                                continue;
                            }
                            const QByteArray patchUtf8 = patchPath.toUtf8();
                            edl_error_t patchErr =
                                edl_service_apply_patch_file(m_edlService, patchUtf8.constData());
                            if (patchErr != EDL_OK) {
                                logNow(QStringLiteral("应用补丁失败：%1")
                                           .arg(QString::fromUtf8(edl_error_str(patchErr))),
                                       false);
                                finishedAllTasks = false;
                                break;
                            }
                            logNow(QStringLiteral("应用补丁成功：%1").arg(patchPath), true);
                        }
                    }
                }

                if (finishedAllTasks && wroteRealData && !isCancelRequested()) {
                    bool doFixGpt = true;
                    QMetaObject::invokeMethod(this, [&doFixGpt, this]() {
                        doFixGpt = ui->fixGptAfterWriteCheckBox->isChecked();
                    }, Qt::BlockingQueuedConnection);

                    bool runFixGpt = doFixGpt;
                    if (tasks.size() == 1) {
                        const QString name = tasks[0].name;
                        const bool isGptNamed =
                            name.compare(QStringLiteral("PrimaryGPT"), Qt::CaseInsensitive) == 0
                            || name.compare(QStringLiteral("BackupGPT"), Qt::CaseInsensitive) == 0;
                        if (!isGptNamed)
                            runFixGpt = false;
                    }

                    if (runFixGpt) {
                        if (!preflightFirehose(QStringLiteral("Fix GPT"))) {
                            logNow(QStringLiteral("链路检查失败：已跳过 GPT 修复"), false);
                        } else {
                            edl_error_t fixErr = edl_service_fix_gpt(m_edlService);
                            if (fixErr == EDL_OK) {
                                logNow(QStringLiteral("修复 GPT 完成"), true);
                            } else {
                                logNow(QStringLiteral("修复 GPT 失败：%1")
                                           .arg(QString::fromUtf8(edl_error_str(fixErr))),
                                       false);
                            }
                        }
                    } else {
                        if (doFixGpt && tasks.size() == 1) {
                            logNow(QStringLiteral("已跳过 fixgpt：写入目标不是 PrimaryGPT 或 BackupGPT"),
                                   true);
                        } else if (!doFixGpt) {
                            logNow(QStringLiteral("已跳过 fixgpt：未勾选该选项"), true);
                        } else {
                            logNow(QStringLiteral("已跳过 fixgpt"), true);
                        }
                    }

                    const bool shouldRefreshGptView = wroteGptLayout || runFixGpt;
                    if (isCancelRequested()) {
                        /* Skip reread on cancellation. */
                    } else if (!shouldRefreshGptView) {
                        logNow(QStringLiteral("已跳过分区表刷新：布局未变化"), true);
                    } else {
                        edl_partition_info_t parts[256];
                        int gptCount = 256;
                        QString storageStr;
                        QMetaObject::invokeMethod(this, [&storageStr, this]() {
                            const int idx = ui->storageCombo->currentIndex();
                            if (idx == 1)
                                storageStr = QStringLiteral("emmc");
                            else if (idx == 2)
                                storageStr = QStringLiteral("ufs");
                        }, Qt::BlockingQueuedConnection);
                        const int maxLun = storageStr == QStringLiteral("emmc") ? 1 : 24;
                        edl_error_t refreshErr =
                            edl_service_read_gpt_ex(m_edlService, parts, &gptCount, maxLun, 0u);
                        if (refreshErr == EDL_OK) {
                            QVector<edl_partition_info_t> partVec(parts, parts + gptCount);
                            QMetaObject::invokeMethod(this, [this, partVec]() {
                                populatePartitionTable(partVec.data(), partVec.size());
                            }, Qt::QueuedConnection);
                            logNow(QStringLiteral("分区表刷新完成"), true);
                        } else {
                            logNow(QStringLiteral("安全模式刷新分区表失败：%1")
                                       .arg(QString::fromUtf8(edl_error_str(refreshErr))),
                                   false);
                        }
                    }
                }
            }

            if (anyFail && !isCancelRequested())
                markAsyncTaskFailed();
            endBatchProgress();
        }, QStringLiteral("Write partition"));
    });

    connect(ui->erasePartBtn, &QPushButton::clicked, this, [this]() {
        const QList<int> rows = selectedRows(ui->partitionTable);
        if (rows.isEmpty()) {
            appendLog(QStringLiteral("请先选择要擦除的分区行"), "#C0392B");
            return;
        }

        QStringList names;
        for (int row : rows) {
            QString name = ui->partitionTable->item(row, ColName)->text();
            name.remove(QStringLiteral(" [鎿﹂櫎]")).remove(QStringLiteral(" [娓呴浂]"));
            names << name;
        }

        const auto ret = QMessageBox::warning(
            this,
            QStringLiteral("确认擦除"),
            QStringLiteral("即将擦除以下 %1 个分区：\n\n%2\n\n此操作不可撤销，是否继续？")
                .arg(names.size())
                .arg(names.join(QStringLiteral(", "))),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;

        const QList<int> rowList = rows;
        runAsync([this, rowList]() {
            if (!ensureConnected()) {
                if (isCancelRequested()) {
                    QMetaObject::invokeMethod(this, [this]() {
                        appendLog(QStringLiteral("操作已取消"), "#7D6B3A");
                    }, Qt::QueuedConnection);
                }
                return;
            }
            if (!preflightFirehose(QStringLiteral("Erase partition")))
                return;
            if (!ensureGptCacheReady())
                return;

            bool anyFail = false;
            auto logNow = [this, &anyFail](const QString &text, bool ok) {
                if (!ok)
                    anyFail = true;
                QMetaObject::invokeMethod(this, [this, text, ok]() {
                    appendLog(text, ok ? "#2E8B3E" : "#C0392B");
                }, Qt::QueuedConnection);
            };

            QVector<qint64> taskBytes(rowList.size(), 0);
            qint64 batchTotalBytes = 0;
            for (int i = 0; i < rowList.size(); ++i) {
                ReadPartitionTask previewTask;
                fillReadPartitionTaskFromRow(ui->partitionTable, rowList[i], previewTask);
                previewTask.name = ui->partitionTable->item(rowList[i], ColName)->text();
                previewTask.name.remove(QStringLiteral(" [閹匡箓娅嶿")).remove(QStringLiteral(" [濞撳懘娴俔"));
                edl_partition_info_t previewPart{};
                bool previewFallback = false;
                const edl_partition_info_t *previewPtr =
                    resolveReadPartitionTask(m_edlService, previewTask, &previewPart, &previewFallback);
                const qint64 bytes = safePartitionLogicalBytes(
                    previewPtr,
                    previewTask.tableSectorSize > 0 ? previewTask.tableSectorSize : 4096);
                taskBytes[i] = bytes;
                if (bytes > 0) {
                    const qint64 limit = (std::numeric_limits<qint64>::max)() - batchTotalBytes;
                    batchTotalBytes += bytes > limit ? limit : bytes;
                }
            }
            beginBatchProgress(batchTotalBytes);
            for (int i = 0; i < rowList.size(); ++i) {
                if (i > 0 && rowList.size() >= 6 && (i % 5) == 0) {
                    if (!preflightFirehose(QStringLiteral("Batch erase item %1").arg(i + 1))) {
                        logNow(QStringLiteral("链路检查失败：已停止后续分区操作"), false);
                        break;
                    }
                }

                const int row = rowList[i];
                const qint64 partBytes = taskBytes.value(i);
                if (batchTotalBytes > 0 && partBytes > 0) {
                    qint64 currentBaseBytes = 0;
                    for (int j = 0; j < i; ++j)
                        currentBaseBytes += taskBytes.value(j);
                    setBatchProgressWindow(currentBaseBytes, partBytes);
                }
                if (isCancelRequested()) {
                    logNow(QStringLiteral("操作已取消；其余分区已跳过"), false);
                    break;
                }

                QString name = ui->partitionTable->item(row, ColName)->text();
                name.remove(QStringLiteral(" [鎿﹂櫎]")).remove(QStringLiteral(" [娓呴浂]"));

                ReadPartitionTask fallback;
                fillReadPartitionTaskFromRow(ui->partitionTable, row, fallback);
                edl_partition_info_t partFromTable{};
                bool usedTableFallback = false;
                fallback.name = name;
                const edl_partition_info_t *partPtr =
                    resolveReadPartitionTask(m_edlService, fallback, &partFromTable, &usedTableFallback);
                if (!partPtr) {
                    logNow(QStringLiteral("擦除 %1 失败：GPT 中无此分区，且没有可用的表格回退数据")
                               .arg(name),
                           false);
                    break;
                }

                logNow(QStringLiteral("正在擦除 %1").arg(name), true);
                edl_error_t err = edl_service_erase_partition(m_edlService, partPtr);
                const QString detail = QString::fromUtf8(edl_error_str(err));
                if (err == EDL_OK) {
                    if (usedTableFallback) {
                        if (edl_service_is_gpt_cache_loaded(m_edlService)) {
                            logNow(QStringLiteral("擦除 %1 成功（GPT 名称查找未命中，已使用表格回退行）")
                                       .arg(name),
                                   true);
                        } else {
                            logNow(QStringLiteral("擦除 %1 成功（设备 GPT 未加载，已使用表格回退行）")
                                       .arg(name),
                                   true);
                        }
                    } else {
                        logNow(QStringLiteral("擦除 %1 成功").arg(name), true);
                    }
                    continue;
                }
                if (err == EDL_ERR_CANCELLED) {
                    logNow(QStringLiteral("擦除 %1 已取消").arg(name), false);
                    break;
                }
                if (edl_error_is_fail(err)) {
                    logNow(QStringLiteral("擦除 %1 失败：%2").arg(name, detail), false);
                    break;
                }
                logNow(QStringLiteral("擦除 %1 错误：%2").arg(name, detail), false);
                if (edl_error_error_stops_batch(err))
                    break;
            }

            if (anyFail && !isCancelRequested())
                markAsyncTaskFailed();
        }, QStringLiteral("Erase partition"));
    });
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(eventType)
    MSG *msg = static_cast<MSG *>(message);

    if (msg->message == WM_NCLBUTTONDBLCLK) {
        if (m_maximized) {
            setGeometry(m_normalGeometry);
            m_maximized = false;
        } else {
            m_normalGeometry = geometry();
            QScreen *scr = screen();
            if (scr)
                setGeometry(scr->availableGeometry());
            m_maximized = true;
        }
        *result = 0;
        return true;
    }

    if (msg->message == WM_NCHITTEST) {
        constexpr int border = 5;
        RECT rc;
        GetWindowRect(msg->hwnd, &rc);
        long x = GET_X_LPARAM(msg->lParam);
        long y = GET_Y_LPARAM(msg->lParam);

        bool l = x < rc.left   + border;
        bool r = x > rc.right  - border;
        bool t = y < rc.top    + border;
        bool b = y > rc.bottom - border;

        if (t && l) { *result = HTTOPLEFT;     return true; }
        if (t && r) { *result = HTTOPRIGHT;    return true; }
        if (b && l) { *result = HTBOTTOMLEFT;  return true; }
        if (b && r) { *result = HTBOTTOMRIGHT; return true; }
        if (l) { *result = HTLEFT;   return true; }
        if (r) { *result = HTRIGHT;  return true; }
        if (t) { *result = HTTOP;    return true; }
        if (b) { *result = HTBOTTOM; return true; }

        QWidget *titleHost = ui->menuButton ? ui->menuButton->parentWidget() : ui->centralwidget;
        if (titleHost) {
            QPoint local = titleHost->mapFromGlobal(QPoint(x, y));
            int titleHeight = 0;
            const QWidget *titleWidgets[] = {
                ui->menuButton,
                ui->titleLabel,
                ui->settingsButton,
                ui->wallpaperButton,
                ui->logButton,
                ui->minimizeButton,
                ui->maximizeButton,
                ui->closeButton
            };
            for (const QWidget *w : titleWidgets) {
                if (w && w->isVisible())
                    titleHeight = qMax(titleHeight, w->geometry().bottom() + 1);
            }
            const QRect titleRect(0, 0, titleHost->width(), titleHeight);
            if (!titleRect.contains(local))
                return QMainWindow::nativeEvent(eventType, message, result);

            for (auto *child : titleHost->findChildren<QPushButton *>()) {
                if (!child->isVisible())
                    continue;
                /* pos()+size() 与 mapFromGlobal 同属顶部宿主坐标系，避免无边框窗体下点击不进槽 */
                const QRect hit(child->geometry());
                if (hit.contains(local)) {
                    *result = HTCLIENT;
                    return true;
                }
            }
            *result = HTCAPTION;
            return true;
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

namespace {

static QString cacheDirForCloudDevice(const QString &id)
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + QStringLiteral("/cloud_cache");
    if (!id.isEmpty())
        root += QLatin1Char('/') + id;
    QDir().mkpath(root);
    return root;
}

static QString localFileNameFromRel(const QString &rel)
{
    const int slash = rel.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? rel.mid(slash + 1) : rel;
}

} // namespace

void MainWindow::applyCloudDeviceEntry(const DeviceEntry &dev)
{
    if (dev.cloudBaseUrl.isEmpty())
        return;

    QProgressDialog progress(tr("正在从云端缓存资源…"), QString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    QString err;
    bool progressShown = false;
    const QString base = EdlApi::normalizeBaseUrl(dev.cloudBaseUrl);
    const QString dir = cacheDirForCloudDevice(dev.id);

    auto runDl = [&](const QString &rel, const QString &localPath) -> bool {
        if (rel.isEmpty() || !rel.startsWith(QStringLiteral("uploads/")))
            return true;
        const QFileInfo fi(localPath);
        if (fi.exists() && fi.isFile() && fi.size() > 0)
            return true;
        err.clear();
        if (!progressShown) {
            progress.show();
            QApplication::processEvents();
            progressShown = true;
        }
        QEventLoop loop;
        EdlApi::downloadToFile(m_netManager, base, rel, localPath, this, [&](QString e) {
            err = e;
            loop.quit();
        });
        loop.exec();
        return err.isEmpty();
    };

    QString fhLocal;
    if (!dev.firehose.isEmpty() && dev.firehose.startsWith(QStringLiteral("uploads/"))) {
        fhLocal = dir + QLatin1Char('/') + localFileNameFromRel(dev.firehose);
        if (!runDl(dev.firehose, fhLocal)) {
            progress.close();
            QMessageBox::warning(this, tr("云端缓存"), tr("无法下载 Firehose：%1").arg(err));
            return;
        }
    } else if (!dev.firehose.isEmpty()) {
        fhLocal = dev.firehose;
    }

    QString localDig;
    const QString digPath = dev.authParams.value(QStringLiteral("digest"));
    if (!digPath.isEmpty() && digPath.startsWith(QStringLiteral("uploads/"))) {
        localDig = dir + QLatin1Char('/') + localFileNameFromRel(digPath);
        if (!runDl(digPath, localDig)) {
            progress.close();
            QMessageBox::warning(this, tr("云端缓存"), tr("无法下载 Digest：%1").arg(err));
            return;
        }
    } else if (!digPath.isEmpty()) {
        localDig = digPath;
    }

    QString localSig;
    const QString sigPath = dev.authParams.value(QStringLiteral("sign"));
    if (!sigPath.isEmpty() && sigPath.startsWith(QStringLiteral("uploads/"))) {
        localSig = dir + QLatin1Char('/') + localFileNameFromRel(sigPath);
        if (!runDl(sigPath, localSig)) {
            progress.close();
            QMessageBox::warning(this, tr("云端缓存"), tr("无法下载 Signature：%1").arg(err));
            return;
        }
    } else if (!sigPath.isEmpty()) {
        localSig = sigPath;
    }

    ui->realmeAuthCheckBox->setChecked(false);
    ui->oplusAuthCheckBox->setChecked(false);
    ui->oneplusAuthCheckBox->setChecked(false);
    ui->xiaomiAuthCheckBox->setChecked(false);
    const QString k = dev.authKind;
    if (k == QStringLiteral("realme"))
        ui->realmeAuthCheckBox->setChecked(true);
    else if (k == QStringLiteral("vip"))
        ui->oplusAuthCheckBox->setChecked(true);
    else if (k == QStringLiteral("oneplus"))
        ui->oneplusAuthCheckBox->setChecked(true);
    else if (k == QStringLiteral("xiaomi"))
        ui->xiaomiAuthCheckBox->setChecked(true);

    const QString proj = dev.authParams.value(QStringLiteral("project_number"));
    if (!proj.isEmpty())
        ui->realmeProjectNumberLineEdit->setText(proj);

    if (!fhLocal.isEmpty())
        ui->firehoseLineEdit->setText(fhLocal);
    if (!localDig.isEmpty())
        ui->digestLineEdit->setText(localDig);
    if (!localSig.isEmpty())
        ui->signatureLineEdit->setText(localSig);

    if (!progressShown) {
        appendLog(QStringLiteral("已应用机型配置（命中本地缓存，无需重新下载）。"),
                  QStringLiteral("#529b2e"));
        return;
    }

    progress.close();
    appendLog(QStringLiteral("已从云端应用机型配置（本机缓存目录）。"), QStringLiteral("#529b2e"));
}

void MainWindow::warmupCloudApiConnection()
{
    const QString base = EdlApi::normalizeBaseUrl(resolvedCloudEdlBaseUrl());
    if (base.isEmpty())
        return;
    /* 轻量 GET /health，建立连接与 TLS 会话，后续机型列表等请求首包更快 */
    EdlApi::fetchHealth(m_netManager, base, this, [](bool, QString) {});
}

void MainWindow::checkCloudUpdateIfConfigured()
{
    const QString base = EdlApi::normalizeBaseUrl(resolvedCloudEdlBaseUrl());
    if (base.isEmpty())
        return;

    EdlApi::fetchUpdateInfo(m_netManager, base, this, [this](QJsonObject o, QString err) {
        if (!err.isEmpty())
            return;
        const bool force = o.value(QStringLiteral("force_update")).toBool();
        const bool optional = o.value(QStringLiteral("optional_update")).toBool();
        const QString latest = o.value(QStringLiteral("latest_version")).toString();
        const QString dl = o.value(QStringLiteral("download_url")).toString();
        const QString notes = o.value(QStringLiteral("release_notes")).toString();
        const QString mode = o.value(QStringLiteral("mode")).toString();
        const QString appVer = QCoreApplication::applicationVersion();
        if (mode == QStringLiteral("none") || latest.isEmpty())
            return;
        if (latest == appVer)
            return;
        QString msg = tr("发现新版本 %1（当前 %2）。").arg(latest, appVer);
        if (!notes.isEmpty())
            msg += QLatin1Char('\n') + notes;
        if (!dl.isEmpty())
            msg += QLatin1Char('\n') + tr("下载：%1").arg(dl);
        if (force) {
            QMessageBox::warning(this, tr("强制更新"), msg);
        } else if (optional) {
            QMessageBox::information(this, tr("更新提示"), msg);
        }
    });
}
