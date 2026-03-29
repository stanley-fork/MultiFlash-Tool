#include "devicedialog.h"
#include "edl_api_client.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QShortcut>
#include <QAbstractItemView>
#include <QPainter>
#include <QPaintEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QFrame>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QSizePolicy>
#include <QTableWidgetItem>
#include <algorithm>

void DeviceDialog::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 12, 12);

    /* 暖色磨砂底，避免深灰/纯黑外框 */
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(252, 248, 242, 252));
    p.drawPath(path);

    p.setClipPath(path);
    p.setBrush(QColor(192, 133, 53));
    p.drawRect(QRectF(r.left(), r.top(), r.width(), 3.0));
    p.setClipping(false);

    /* 描边用浅褐灰，不用纯黑 */
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(110, 98, 88, 70), 1));
    p.drawPath(path);
}

void DeviceDialog::done(int r)
{
    if (r == QDialog::Accepted && m_table->rowCount() > 0)
        emit deviceSelected(selectedDevice());
    QDialog::done(r);
}

DeviceDialog::DeviceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    /* 禁止 WA_DeleteOnClose：主窗口用栈上 DeviceDialog dlg; dlg.exec()，若开启会导致关闭时对栈对象 delete，闪退 */
    setMinimumSize(700, 540);
    resize(820, 600);

    buildUi();
    populateDevices();
    rebuildBrandCombo();
    applyFilter();
}

void DeviceDialog::buildUi()
{
    auto *root = new QWidget(this);
    root->setObjectName("deviceDialogRoot");
    root->setAttribute(Qt::WA_TranslucentBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(root);

    auto *inner = new QVBoxLayout(root);
    inner->setContentsMargins(24, 22, 24, 20);
    inner->setSpacing(0);

    auto *title = new QLabel(tr("选择机型"), root);
    title->setObjectName("deviceDialogTitle");

    auto *subtitle = new QLabel(
        tr("列表来自设置中配置的 EDL Admin 服务。可按芯片、设备名、品牌或 Firehose 搜索；"
           "双击行或 Enter 确认，Esc 取消。"),
        root);
    subtitle->setObjectName("deviceDialogSubtitle");
    subtitle->setWordWrap(true);

    auto *headerRow = new QHBoxLayout();
    headerRow->setSpacing(12);
    auto *titleCol = new QVBoxLayout();
    titleCol->setSpacing(6);
    titleCol->addWidget(title);
    titleCol->addWidget(subtitle);
    auto *closeBtn = new QPushButton(QStringLiteral("\u2715"), root);
    closeBtn->setObjectName("dialogCloseBtn");
    closeBtn->setFixedSize(32, 32);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setToolTip(tr("关闭"));
    headerRow->addLayout(titleCol, 1);
    headerRow->addWidget(closeBtn, 0, Qt::AlignTop);
    inner->addLayout(headerRow);
    inner->addSpacing(18);

    connect(closeBtn, &QPushButton::clicked, this, &DeviceDialog::reject);

    auto *filterBar = new QFrame(root);
    filterBar->setObjectName("deviceFilterBar");
    auto *filterLay = new QHBoxLayout(filterBar);
    filterLay->setContentsMargins(12, 10, 12, 10);
    filterLay->setSpacing(12);

    m_searchEdit = new QLineEdit(filterBar);
    m_searchEdit->setObjectName("deviceSearchEdit");
    m_searchEdit->setPlaceholderText(tr("搜索芯片、设备、品牌、Firehose…"));
    m_searchEdit->setClearButtonEnabled(true);
    filterLay->addWidget(m_searchEdit, 1);

    auto *brandLbl = new QLabel(tr("品牌"), filterBar);
    brandLbl->setObjectName("deviceBrandLabel");

    m_brandCombo = new QComboBox(filterBar);
    m_brandCombo->setObjectName("deviceBrandCombo");
    m_brandCombo->setMinimumWidth(168);
    m_brandCombo->setMaxVisibleItems(20);

    m_countLabel = new QLabel(filterBar);
    m_countLabel->setObjectName("deviceCountLabel");
    m_countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_countLabel->setMinimumWidth(100);

    filterLay->addWidget(brandLbl);
    filterLay->addWidget(m_brandCombo);
    filterLay->addWidget(m_countLabel);

    inner->addWidget(filterBar);
    inner->addSpacing(12);

    auto *tableCard = new QFrame(root);
    tableCard->setObjectName("deviceTableCard");
    tableCard->setMinimumHeight(300);
    auto *tableCardLay = new QVBoxLayout(tableCard);
    tableCardLay->setContentsMargins(1, 1, 1, 1);
    tableCardLay->setSpacing(0);

    m_deviceStack = new QStackedWidget(tableCard);
    m_deviceStack->setObjectName("deviceDialogStack");
    m_deviceStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_table = new QTableWidget(tableCard);
    m_table->setObjectName("deviceTable");
    m_table->setAttribute(Qt::WA_StyledBackground, true);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({ tr("芯片"), tr("设备"), tr("品牌"), tr("Firehose") });
    /* 芯片/品牌按内容；型号可拖拽宽度；最后一列 Firehose 占满剩余宽度（长路径扩展显示） */
    m_table->horizontalHeader()->setMinimumSectionSize(72);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setColumnWidth(1, 220);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(34);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->setFocusPolicy(Qt::StrongFocus);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->setWordWrap(false);
    m_table->setMinimumHeight(280);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    m_emptyHint = new QLabel(
        tr("<p style='margin:0'><span style='font-size:15px;font-weight:600;color:#6E6A62;'>"
           "未找到机型</span></p>"
           "<p style='margin:12px 0 0 0;color:#9A958C;'>"
           "试试缩短关键词，或将品牌改为「全部品牌」。</p>"),
        tableCard);
    m_emptyHint->setObjectName("deviceEmptyHint");
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setTextFormat(Qt::RichText);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setVisible(false);

    m_deviceStack->addWidget(m_table);
    m_deviceStack->addWidget(m_emptyHint);
    tableCardLay->addWidget(m_deviceStack);

    inner->addWidget(tableCard, 1);

    auto *sep = new QFrame(root);
    sep->setObjectName("deviceDialogSeparator");
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Plain);
    sep->setFixedHeight(1);
    inner->addSpacing(14);
    inner->addWidget(sep);
    inner->addSpacing(14);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    btnRow->addStretch();

    m_cancelBtn = new QPushButton(tr("取消"), root);
    m_cancelBtn->setObjectName("dialogCancelBtn");
    m_cancelBtn->setFixedHeight(34);
    m_cancelBtn->setMinimumWidth(108);
    btnRow->addWidget(m_cancelBtn);

    m_selectBtn = new QPushButton(tr("选择机型"), root);
    m_selectBtn->setObjectName("dialogSelectBtn");
    m_selectBtn->setFixedHeight(34);
    m_selectBtn->setMinimumWidth(120);
    btnRow->addWidget(m_selectBtn);

    inner->addLayout(btnRow);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &DeviceDialog::applyFilter);
    connect(m_brandCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeviceDialog::applyFilter);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
        if (m_table->rowCount() > 0)
            accept();
    });
    connect(m_selectBtn, &QPushButton::clicked, this, &DeviceDialog::accept);
    connect(m_cancelBtn, &QPushButton::clicked, this, &DeviceDialog::reject);

    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &DeviceDialog::reject);
    auto acceptIfRow = [this] {
        if (m_table->rowCount() > 0 && m_table->currentRow() >= 0)
            accept();
    };
    auto *enterRet = new QShortcut(QKeySequence(Qt::Key_Return), this);
    connect(enterRet, &QShortcut::activated, this, acceptIfRow);
    auto *enterKp = new QShortcut(QKeySequence(Qt::Key_Enter), this);
    connect(enterKp, &QShortcut::activated, this, acceptIfRow);
    auto *focusSearch = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(focusSearch, &QShortcut::activated, m_searchEdit, qOverload<>(&QWidget::setFocus));
}

void DeviceDialog::populateDevices()
{
    // 机型数据仅来自云端 GET /api/v1/device-models（见 loadCloudDeviceList）
}

void DeviceDialog::setNetworkAccessManager(QNetworkAccessManager *nam)
{
    m_netManager = nam;
}

void DeviceDialog::setCloudBaseUrl(const QString &baseUrl)
{
    m_cloudBaseUrl = EdlApi::normalizeBaseUrl(baseUrl);
    if (!m_cloudBaseUrl.isEmpty()) {
        m_allDevices.clear();
        m_cloudFetchDone = false;
        m_cloudFetchError = false;
        m_cloudFetchStarted = false;
        rebuildBrandCombo();
        applyFilter();
    }
}

void DeviceDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_cloudBaseUrl.isEmpty())
        return;
    if (!m_netManager) {
        m_cloudFetchDone = true;
        m_cloudFetchError = true;
        m_cloudFetchStarted = true;
        rebuildBrandCombo();
        applyFilter();
        return;
    }
    if (m_cloudFetchStarted)
        return;
    m_cloudFetchStarted = true;
    loadCloudDeviceList();
}

void DeviceDialog::loadCloudDeviceList()
{
    EdlApi::fetchDeviceModels(m_netManager, m_cloudBaseUrl, this,
        [this](QJsonArray arr, QString err) {
            if (!err.isEmpty()) {
                m_allDevices.clear();
                m_cloudFetchDone = true;
                m_cloudFetchError = true;
                QMessageBox::warning(this, tr("云端机型"),
                    tr("无法加载机型列表：%1\n请检查 Base URL、网络与管理端是否已启动。").arg(err));
                rebuildBrandCombo();
                applyFilter();
                return;
            }
            QVector<DeviceEntry> out;
            out.reserve(arr.size());
            for (const QJsonValue &v : arr) {
                if (!v.isObject())
                    continue;
                const QJsonObject o = v.toObject();
                DeviceEntry e;
                e.id = o.value(QStringLiteral("id")).toString();
                e.chipset = o.value(QStringLiteral("chipset")).toString();
                e.device = o.value(QStringLiteral("device_name")).toString();
                e.brand = o.value(QStringLiteral("brand")).toString();
                e.firehose = o.value(QStringLiteral("firehose")).toString();
                e.authKind = o.value(QStringLiteral("auth_kind")).toString();
                const QJsonObject ap = o.value(QStringLiteral("auth_params")).toObject();
                for (auto it = ap.begin(); it != ap.end(); ++it)
                    e.authParams.insert(it.key(), it.value().toString());
                e.cloudBaseUrl = m_cloudBaseUrl;
                out.append(e);
            }
            m_cloudFetchDone = true;
            m_cloudFetchError = false;
            m_allDevices = out;
            rebuildBrandCombo();
            applyFilter();
        });
}

void DeviceDialog::rebuildBrandCombo()
{
    const QSignalBlocker b(m_brandCombo);
    m_brandCombo->clear();
    m_brandCombo->addItem(tr("全部品牌"), QString());

    QStringList brands;
    brands.reserve(m_allDevices.size());
    for (const auto &d : m_allDevices) {
        if (!d.brand.isEmpty())
            brands.append(d.brand);
    }
    std::sort(brands.begin(), brands.end());
    brands.erase(std::unique(brands.begin(), brands.end()), brands.end());

    for (const QString &bname : brands)
        m_brandCombo->addItem(bname, bname);
}

void DeviceDialog::applyFilter()
{
    const QString kw = m_searchEdit->text().trimmed().toLower();
    const QString brandSel = m_brandCombo->currentData().toString();

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    auto matchesKeyword = [&](const DeviceEntry &d) -> bool {
        if (kw.isEmpty())
            return true;
        return d.chipset.toLower().contains(kw) || d.device.toLower().contains(kw)
            || d.brand.toLower().contains(kw) || d.firehose.toLower().contains(kw);
    };

    for (int i = 0; i < m_allDevices.size(); ++i) {
        const auto &d = m_allDevices[i];
        if (!brandSel.isEmpty() && d.brand != brandSel)
            continue;
        if (!matchesKeyword(d))
            continue;

        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *chipItem = new QTableWidgetItem(d.chipset);
        chipItem->setData(Qt::UserRole, i);
        if (!d.chipset.isEmpty())
            chipItem->setToolTip(d.chipset);
        m_table->setItem(row, 0, chipItem);

        auto *devItem = new QTableWidgetItem(d.device);
        if (!d.device.isEmpty())
            devItem->setToolTip(d.device);
        m_table->setItem(row, 1, devItem);

        auto *brandItem = new QTableWidgetItem(d.brand);
        if (!d.brand.isEmpty())
            brandItem->setToolTip(d.brand);
        m_table->setItem(row, 2, brandItem);

        auto *fhItem = new QTableWidgetItem(d.firehose);
        if (!d.firehose.isEmpty())
            fhItem->setToolTip(d.firehose);
        m_table->setItem(row, 3, fhItem);
    }

    if (m_table->rowCount() > 0) {
        m_table->resizeColumnToContents(0);
        m_table->resizeColumnToContents(2);
    }

    m_table->setSortingEnabled(true);

    const int n = m_table->rowCount();
    m_countLabel->setText(tr("共 %1 条").arg(n));

    const bool empty = (n == 0);
    m_deviceStack->setCurrentWidget(empty ? static_cast<QWidget *>(m_emptyHint)
                                           : static_cast<QWidget *>(m_table));
    m_selectBtn->setEnabled(!empty);

    if (empty)
        updateEmptyHint();

    if (!empty) {
        m_table->selectRow(0);
        m_table->setFocus();
    }
}

void DeviceDialog::updateEmptyHint()
{
    if (m_cloudBaseUrl.isEmpty()) {
        m_emptyHint->setText(
            tr("<p style='margin:0'><span style='font-size:15px;font-weight:600;color:#6E6A62;'>"
               "未配置云端</span></p>"
               "<p style='margin:12px 0 0 0;color:#9A958C;'>"
               "请在「设置」→「SAKURAEDL 云端（EDL Admin）」中填写管理端 Base URL（如 http://127.0.0.1:8088），"
               "保存后再打开本窗口。</p>"));
        return;
    }
    if (!m_cloudFetchDone) {
        m_emptyHint->setText(
            tr("<p style='margin:0'><span style='font-size:15px;font-weight:600;color:#6E6A62;'>"
               "正在加载</span></p>"
               "<p style='margin:12px 0 0 0;color:#9A958C;'>"
               "正在从云端拉取机型列表…</p>"));
        return;
    }
    if (m_cloudFetchError) {
        m_emptyHint->setText(
            tr("<p style='margin:0'><span style='font-size:15px;font-weight:600;color:#6E6A62;'>"
               "加载失败</span></p>"
               "<p style='margin:12px 0 0 0;color:#9A958C;'>"
               "无法从服务端获取机型，请检查网络与 Base URL 后重试。</p>"));
        return;
    }
    if (m_allDevices.isEmpty()) {
        m_emptyHint->setText(
            tr("<p style='margin:0'><span style='font-size:15px;font-weight:600;color:#6E6A62;'>"
               "暂无机型</span></p>"
               "<p style='margin:12px 0 0 0;color:#9A958C;'>"
               "服务端未返回任何机型，请在 edl-admin 管理后台添加。</p>"));
        return;
    }
    m_emptyHint->setText(
        tr("<p style='margin:0'><span style='font-size:15px;font-weight:600;color:#6E6A62;'>"
           "未找到机型</span></p>"
           "<p style='margin:12px 0 0 0;color:#9A958C;'>"
           "试试缩短搜索关键词，或将品牌改为「全部品牌」。</p>"));
}

DeviceEntry DeviceDialog::selectedDevice() const
{
    const QList<QTableWidgetItem *> items = m_table->selectedItems();
    if (items.isEmpty())
        return {};

    const int row = items.first()->row();
    QTableWidgetItem *chipItem = m_table->item(row, 0);
    if (!chipItem)
        return {};

    const QVariant v = chipItem->data(Qt::UserRole);
    if (v.isValid() && v.canConvert<int>()) {
        const int idx = v.toInt();
        if (idx >= 0 && idx < m_allDevices.size())
            return m_allDevices[idx];
    }

    return DeviceEntry{
        chipItem->text(),
        m_table->item(row, 1) ? m_table->item(row, 1)->text() : QString(),
        m_table->item(row, 2) ? m_table->item(row, 2)->text() : QString(),
        m_table->item(row, 3) ? m_table->item(row, 3)->text() : QString(),
    };
}
