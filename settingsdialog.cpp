#include "settingsdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScrollArea>
#include <QFrame>
#include <QShortcut>
#include <QKeySequence>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(500, 520);
    resize(540, 700);
    buildUi();
}

void SettingsDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 12, 12);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(252, 248, 242, 252));
    p.drawPath(path);

    p.setClipPath(path);
    p.setBrush(QColor(192, 133, 53));
    p.drawRect(QRectF(r.left(), r.top(), r.width(), 3.0));
    p.setClipping(false);

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(110, 98, 88, 70), 1));
    p.drawPath(path);
}

void SettingsDialog::buildUi()
{
    auto *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    /* ── 顶栏 ── */
    auto *header = new QWidget(this);
    header->setObjectName("settingsHeader");
    auto *headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(22, 18, 16, 8);
    headerLay->setSpacing(8);

    auto *titleCol = new QVBoxLayout;
    titleCol->setSpacing(4);
    auto *titleLbl = new QLabel(tr("设置"));
    titleLbl->setObjectName("settingsTitleLabel");
    auto *subLbl = new QLabel(tr("主题、日志、重启镜像与 Realme 签名"));
    subLbl->setObjectName("settingsSubtitleLabel");
    titleCol->addWidget(titleLbl);
    titleCol->addWidget(subLbl);
    headerLay->addLayout(titleCol, 1);

    auto *closeBtn = new QPushButton(QStringLiteral("\u2715"));
    closeBtn->setFixedSize(32, 32);
    closeBtn->setObjectName("dialogCloseBtn");
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    headerLay->addWidget(closeBtn, 0, Qt::AlignTop);

    mainLay->addWidget(header);

    auto *sepTop = new QFrame(this);
    sepTop->setObjectName("settingsHeaderSeparator");
    sepTop->setFrameShape(QFrame::HLine);
    sepTop->setFixedHeight(1);
    mainLay->addWidget(sepTop);

    /* ── 可滚动内容 ── */
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("settingsScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto *content = new QWidget;
    content->setObjectName("settingsScrollContent");
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(20, 12, 20, 16);
    root->setSpacing(14);

    auto lineStyle = [](QLineEdit *e) {
        e->setObjectName("settingsLineEdit");
        e->setMinimumHeight(28);
    };

    // ── Theme Group ──
    auto *themeGroup = new QGroupBox(tr("主题"));
    themeGroup->setObjectName("settingsGroup");
    auto *tgl = new QVBoxLayout(themeGroup);
    tgl->setSpacing(8);

    m_apiRadio = new QRadioButton(tr("API 主题（远程 JSON）"));
    m_localRadio = new QRadioButton(tr("本地壁纸（图片文件）"));
    m_localRadio->setChecked(true);

    auto *apiRow = new QHBoxLayout;
    auto *apiLbl = new QLabel(tr("URL"));
    apiLbl->setFixedWidth(40);
    apiLbl->setObjectName("settingsFieldLabel");
    m_apiUrlEdit = new QLineEdit;
    lineStyle(m_apiUrlEdit);
    m_apiUrlEdit->setPlaceholderText(QStringLiteral("https://example.com/theme.json"));
    m_apiUrlEdit->setEnabled(false);
    apiRow->addWidget(apiLbl);
    apiRow->addWidget(m_apiUrlEdit, 1);

    auto *localRow = new QHBoxLayout;
    auto *localLbl = new QLabel(tr("路径"));
    localLbl->setFixedWidth(40);
    localLbl->setObjectName("settingsFieldLabel");
    m_localPathEdit = new QLineEdit;
    lineStyle(m_localPathEdit);
    m_localPathEdit->setPlaceholderText(tr("选择壁纸图片…"));
    m_localBrowseBtn = new QPushButton(QStringLiteral("…"));
    m_localBrowseBtn->setFixedSize(40, 30);
    m_localBrowseBtn->setObjectName("settingsBrowseBtn");
    m_localBrowseBtn->setCursor(Qt::PointingHandCursor);
    localRow->addWidget(localLbl);
    localRow->addWidget(m_localPathEdit, 1);
    localRow->addWidget(m_localBrowseBtn);

    tgl->addWidget(m_apiRadio);
    tgl->addLayout(apiRow);
    tgl->addWidget(m_localRadio);
    tgl->addLayout(localRow);
    root->addWidget(themeGroup);

    connect(m_apiRadio, &QRadioButton::toggled, this, [this](bool on) {
        m_apiUrlEdit->setEnabled(on);
        m_localPathEdit->setEnabled(!on);
        m_localBrowseBtn->setEnabled(!on);
    });
    connect(m_localBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString p = QFileDialog::getOpenFileName(this, tr("选择壁纸"), QString(),
                        tr("图片文件 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*.*)"));
        if (!p.isEmpty())
            m_localPathEdit->setText(p);
    });

    // ── Log Group ──
    auto *logGroup = new QGroupBox(tr("日志"));
    logGroup->setObjectName("settingsGroup");
    auto *lgl = new QVBoxLayout(logGroup);
    lgl->setSpacing(8);

    m_autoSaveLogCheck = new QCheckBox(tr("自动保存日志"));
    m_autoSaveLogCheck->setChecked(true);
    m_timestampCheck = new QCheckBox(tr("显示时间戳"));
    m_timestampCheck->setChecked(true);

    auto *logPathRow = new QHBoxLayout;
    auto *logPathLbl = new QLabel(tr("目录"));
    logPathLbl->setFixedWidth(40);
    logPathLbl->setObjectName("settingsFieldLabel");
    m_logPathEdit = new QLineEdit;
    lineStyle(m_logPathEdit);
    m_logPathEdit->setPlaceholderText(tr("日志保存目录…"));
    m_logBrowseBtn = new QPushButton(QStringLiteral("…"));
    m_logBrowseBtn->setFixedSize(40, 30);
    m_logBrowseBtn->setObjectName("settingsBrowseBtn");
    m_logBrowseBtn->setCursor(Qt::PointingHandCursor);
    logPathRow->addWidget(logPathLbl);
    logPathRow->addWidget(m_logPathEdit, 1);
    logPathRow->addWidget(m_logBrowseBtn);

    m_detailLogCheck = new QCheckBox(tr("在界面显示详细日志（NOP/Firehose，调试用）"));
    m_detailLogCheck->setChecked(false);

    lgl->addWidget(m_autoSaveLogCheck);
    lgl->addLayout(logPathRow);
    lgl->addWidget(m_timestampCheck);
    lgl->addWidget(m_detailLogCheck);
    root->addWidget(logGroup);

    connect(m_logBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, tr("选择日志保存目录"));
        if (!dir.isEmpty())
            m_logPathEdit->setText(dir);
    });

    // ── General Group ──
    auto *genGroup = new QGroupBox(tr("通用"));
    genGroup->setObjectName("settingsGroup");
    auto *ggl = new QVBoxLayout(genGroup);
    ggl->setSpacing(8);

    m_autoDetectCheck = new QCheckBox(tr("自动检测设备"));
    m_autoDetectCheck->setChecked(true);
    m_confirmCheck = new QCheckBox(tr("执行操作前确认"));
    m_confirmCheck->setChecked(true);

    ggl->addWidget(m_autoDetectCheck);
    ggl->addWidget(m_confirmCheck);
    root->addWidget(genGroup);

    // ── EDL Admin 云端（机型 / 更新 / 文件） ──
    auto *cloudGroup = new QGroupBox(tr("SAKURAEDL 云端（EDL Admin）"));
    cloudGroup->setObjectName("settingsGroup");
    auto *cgl = new QVBoxLayout(cloudGroup);
    cgl->setSpacing(8);
    auto *cloudHint = new QLabel(
        tr("填写管理端 HTTP(S) 根地址（与 edl-admin 后端一致），用于：拉取机型列表、检查更新、"
           "缓存 Firehose/认证文件到本机。\n"
           "未保存过此项时，默认使用 https://api.xiriacg.top；本地联调可改为 http://127.0.0.1:8088。"
           "若留空并保存，将不使用云端地址。"));
    cloudHint->setWordWrap(true);
    cloudHint->setObjectName("settingsHintLabel");
    cgl->addWidget(cloudHint);
    auto *cloudRow = new QHBoxLayout;
    auto *cloudLbl = new QLabel(QStringLiteral("Base URL"));
    cloudLbl->setFixedWidth(72);
    cloudLbl->setObjectName("settingsFieldLabel");
    m_cloudEdlBaseUrlEdit = new QLineEdit;
    lineStyle(m_cloudEdlBaseUrlEdit);
    m_cloudEdlBaseUrlEdit->setPlaceholderText(QStringLiteral("https://api.xiriacg.top（本地联调可填 http://127.0.0.1:8088）"));
    cloudRow->addWidget(cloudLbl);
    cloudRow->addWidget(m_cloudEdlBaseUrlEdit, 1);
    cgl->addLayout(cloudRow);
    root->addWidget(cloudGroup);

    // ── MISC reboot images ──
    auto *miscGroup = new QGroupBox(tr("MISC 重启镜像"));
    miscGroup->setObjectName("settingsGroup");
    auto *mgl = new QVBoxLayout(miscGroup);
    mgl->setSpacing(8);
    auto *miscHint = new QLabel(
        tr("将镜像写入 misc 分区后执行正常重启。\n"
           "可留空：优先程序目录下 misc_tofastbootd.img / misc_torecovery.img；"
           "兼容旧名 misc_fastboot.bin / misc_recovery.bin；若编译时嵌入了 bundled/ 会自动释放使用。"));
    miscHint->setWordWrap(true);
    miscHint->setObjectName("settingsHintLabel");
    mgl->addWidget(miscHint);

    auto *fbRow = new QHBoxLayout;
    auto *fbLbl = new QLabel(QStringLiteral("Fastbootd"));
    fbLbl->setFixedWidth(72);
    fbLbl->setObjectName("settingsFieldLabel");
    m_miscFastbootPathEdit = new QLineEdit;
    lineStyle(m_miscFastbootPathEdit);
    m_miscFastbootPathEdit->setPlaceholderText(tr("misc_tofastbootd.img 或自定义路径…"));
    m_miscFastbootBrowseBtn = new QPushButton(QStringLiteral("…"));
    m_miscFastbootBrowseBtn->setFixedSize(40, 30);
    m_miscFastbootBrowseBtn->setObjectName("settingsBrowseBtn");
    m_miscFastbootBrowseBtn->setCursor(Qt::PointingHandCursor);
    fbRow->addWidget(fbLbl);
    fbRow->addWidget(m_miscFastbootPathEdit, 1);
    fbRow->addWidget(m_miscFastbootBrowseBtn);
    mgl->addLayout(fbRow);

    auto *recRow = new QHBoxLayout;
    auto *recLbl = new QLabel(QStringLiteral("Recovery"));
    recLbl->setFixedWidth(72);
    recLbl->setObjectName("settingsFieldLabel");
    m_miscRecoveryPathEdit = new QLineEdit;
    lineStyle(m_miscRecoveryPathEdit);
    m_miscRecoveryPathEdit->setPlaceholderText(tr("misc_torecovery.img 或自定义路径…"));
    m_miscRecoveryBrowseBtn = new QPushButton(QStringLiteral("…"));
    m_miscRecoveryBrowseBtn->setFixedSize(40, 30);
    m_miscRecoveryBrowseBtn->setObjectName("settingsBrowseBtn");
    m_miscRecoveryBrowseBtn->setCursor(Qt::PointingHandCursor);
    recRow->addWidget(recLbl);
    recRow->addWidget(m_miscRecoveryPathEdit, 1);
    recRow->addWidget(m_miscRecoveryBrowseBtn);
    mgl->addLayout(recRow);

    root->addWidget(miscGroup);

    connect(m_miscFastbootBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString p = QFileDialog::getOpenFileName(this, tr("选择 Fastbootd 用 MISC 镜像"), QString(),
                        tr("镜像 (*.bin *.img);;所有文件 (*.*)"));
        if (!p.isEmpty())
            m_miscFastbootPathEdit->setText(p);
    });
    connect(m_miscRecoveryBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString p = QFileDialog::getOpenFileName(this, tr("选择 Recovery 用 MISC 镜像"), QString(),
                        tr("镜像 (*.bin *.img);;所有文件 (*.*)"));
        if (!p.isEmpty())
            m_miscRecoveryPathEdit->setText(p);
    });

    // ── Realme Group ──
    auto *realmeGroup = new QGroupBox(tr("Realme 云端签名"));
    realmeGroup->setObjectName("settingsGroup");
    auto *rgl = new QVBoxLayout(realmeGroup);
    rgl->setSpacing(8);

    auto *realmeApiRow = new QHBoxLayout;
    auto *realmeApiLbl = new QLabel(QStringLiteral("API"));
    realmeApiLbl->setFixedWidth(50);
    realmeApiLbl->setObjectName("settingsFieldLabel");
    m_realmeApiUrlEdit = new QLineEdit;
    lineStyle(m_realmeApiUrlEdit);
    m_realmeApiUrlEdit->setPlaceholderText(tr("https://…/sign（留空用默认）"));
    realmeApiRow->addWidget(realmeApiLbl);
    realmeApiRow->addWidget(m_realmeApiUrlEdit, 1);

    auto *rcsmAccRow = new QHBoxLayout;
    auto *rcsmAccLbl = new QLabel(tr("账号"));
    rcsmAccLbl->setFixedWidth(50);
    rcsmAccLbl->setObjectName("settingsFieldLabel");
    m_realmeRcsmAccountEdit = new QLineEdit;
    lineStyle(m_realmeRcsmAccountEdit);
    m_realmeRcsmAccountEdit->setPlaceholderText(tr("RCSMAUTH（留空用内置默认）"));
    rcsmAccRow->addWidget(rcsmAccLbl);
    rcsmAccRow->addWidget(m_realmeRcsmAccountEdit, 1);

    auto *rcsmKeyRow = new QHBoxLayout;
    auto *rcsmKeyLbl = new QLabel(QStringLiteral("Key"));
    rcsmKeyLbl->setFixedWidth(50);
    rcsmKeyLbl->setObjectName("settingsFieldLabel");
    m_realmeRcsmKeyEdit = new QLineEdit;
    lineStyle(m_realmeRcsmKeyEdit);
    m_realmeRcsmKeyEdit->setPlaceholderText(tr("Auth Key UUID（留空用内置默认）"));
    m_realmeRcsmKeyEdit->setEchoMode(QLineEdit::Password);
    rcsmKeyRow->addWidget(rcsmKeyLbl);
    rcsmKeyRow->addWidget(m_realmeRcsmKeyEdit, 1);

    rgl->addLayout(realmeApiRow);
    rgl->addLayout(rcsmAccRow);
    rgl->addLayout(rcsmKeyRow);
    root->addWidget(realmeGroup);

    root->addStretch(0);

    scroll->setWidget(content);
    mainLay->addWidget(scroll, 1);

    /* ── 底栏按钮 ── */
    auto *sepBot = new QFrame(this);
    sepBot->setObjectName("settingsFooterSeparator");
    sepBot->setFrameShape(QFrame::HLine);
    sepBot->setFixedHeight(1);
    mainLay->addWidget(sepBot);

    auto *footer = new QWidget(this);
    footer->setObjectName("settingsFooter");
    auto *btnRow = new QHBoxLayout(footer);
    btnRow->setContentsMargins(20, 12, 20, 18);
    btnRow->setSpacing(10);
    btnRow->addStretch();
    m_saveBtn = new QPushButton(tr("保存"));
    m_saveBtn->setObjectName("settingsSaveBtn");
    m_saveBtn->setMinimumSize(108, 36);
    m_saveBtn->setCursor(Qt::PointingHandCursor);
    m_cancelBtn = new QPushButton(tr("取消"));
    m_cancelBtn->setObjectName("settingsCancelBtn");
    m_cancelBtn->setMinimumSize(96, 36);
    m_cancelBtn->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_saveBtn);
    mainLay->addWidget(footer);

    connect(m_saveBtn, &QPushButton::clicked, this, [this]() {
        emit themeChanged();
        accept();
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &QDialog::reject);
}

// ── Getters ──
bool SettingsDialog::isApiTheme() const { return m_apiRadio->isChecked(); }
QString SettingsDialog::apiUrl() const { return m_apiUrlEdit->text(); }
QString SettingsDialog::localWallpaperPath() const { return m_localPathEdit->text(); }
bool SettingsDialog::autoSaveLog() const { return m_autoSaveLogCheck->isChecked(); }
QString SettingsDialog::logSavePath() const { return m_logPathEdit->text(); }
bool SettingsDialog::showTimestamp() const { return m_timestampCheck->isChecked(); }
bool SettingsDialog::showDetailLog() const { return m_detailLogCheck->isChecked(); }
bool SettingsDialog::autoDetectDevice() const { return m_autoDetectCheck->isChecked(); }
bool SettingsDialog::confirmBeforeAction() const { return m_confirmCheck->isChecked(); }
QString SettingsDialog::realmeApiUrl() const { return m_realmeApiUrlEdit->text().trimmed(); }
QString SettingsDialog::realmeRcsmAccount() const { return m_realmeRcsmAccountEdit->text().trimmed(); }
QString SettingsDialog::realmeRcsmKey() const { return m_realmeRcsmKeyEdit->text().trimmed(); }
QString SettingsDialog::miscFastbootImagePath() const { return m_miscFastbootPathEdit->text().trimmed(); }
QString SettingsDialog::miscRecoveryImagePath() const { return m_miscRecoveryPathEdit->text().trimmed(); }
QString SettingsDialog::cloudEdlBaseUrl() const { return m_cloudEdlBaseUrlEdit->text().trimmed(); }

// ── Setters ──
void SettingsDialog::setApiUrl(const QString &url) { m_apiUrlEdit->setText(url); }
void SettingsDialog::setLocalWallpaperPath(const QString &path) { m_localPathEdit->setText(path); }
void SettingsDialog::setApiThemeMode(bool api)
{
    if (api)
        m_apiRadio->setChecked(true);
    else
        m_localRadio->setChecked(true);
}
void SettingsDialog::setAutoSaveLog(bool on) { m_autoSaveLogCheck->setChecked(on); }
void SettingsDialog::setLogSavePath(const QString &path) { m_logPathEdit->setText(path); }
void SettingsDialog::setShowTimestamp(bool on) { m_timestampCheck->setChecked(on); }
void SettingsDialog::setShowDetailLog(bool on) { m_detailLogCheck->setChecked(on); }
void SettingsDialog::setAutoDetectDevice(bool on) { m_autoDetectCheck->setChecked(on); }
void SettingsDialog::setConfirmBeforeAction(bool on) { m_confirmCheck->setChecked(on); }
void SettingsDialog::setRealmeApiUrl(const QString &url) { m_realmeApiUrlEdit->setText(url); }
void SettingsDialog::setRealmeRcsmAccount(const QString &acc) { m_realmeRcsmAccountEdit->setText(acc); }
void SettingsDialog::setRealmeRcsmKey(const QString &key) { m_realmeRcsmKeyEdit->setText(key); }
void SettingsDialog::setMiscFastbootImagePath(const QString &path) { m_miscFastbootPathEdit->setText(path); }
void SettingsDialog::setMiscRecoveryImagePath(const QString &path) { m_miscRecoveryPathEdit->setText(path); }
void SettingsDialog::setCloudEdlBaseUrl(const QString &url) { m_cloudEdlBaseUrlEdit->setText(url); }
