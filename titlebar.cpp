#include "titlebar.h"
#include <QApplication>
#include <QCoreApplication>
#include <QStyle>
#include <QIcon>

TitleBar::TitleBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(40);
    setObjectName("TitleBar");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(4);

    m_menuBtn = new QToolButton(this);
    m_menuBtn->setObjectName("MenuButton");
    m_menuBtn->setIcon(QIcon(":/icons/icons/menu.svg"));
    m_menuBtn->setIconSize(QSize(20, 20));
    m_menuBtn->setFixedSize(36, 32);
    m_menuBtn->setCursor(Qt::PointingHandCursor);
    connect(m_menuBtn, &QToolButton::clicked, this, &TitleBar::menuClicked);

    m_titleLabel = new QLabel(
        QStringLiteral("%1 | SAKURAEDL").arg(QCoreApplication::applicationVersion()), this);
    m_titleLabel->setObjectName("TitleLabel");
    m_titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_settingsBtn = new QToolButton(this);
    m_settingsBtn->setObjectName("SettingsButton");
    m_settingsBtn->setIcon(QIcon(":/icons/icons/settings.svg"));
    m_settingsBtn->setIconSize(QSize(18, 18));
    m_settingsBtn->setFixedSize(30, 30);
    m_settingsBtn->setCursor(Qt::PointingHandCursor);
    connect(m_settingsBtn, &QToolButton::clicked, this, &TitleBar::settingsClicked);

    m_minimizeBtn = new QToolButton(this);
    m_minimizeBtn->setObjectName("MinimizeButton");
    m_minimizeBtn->setIcon(QIcon(":/icons/icons/minimize.svg"));
    m_minimizeBtn->setIconSize(QSize(16, 16));
    m_minimizeBtn->setFixedSize(30, 30);
    m_minimizeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_minimizeBtn, &QToolButton::clicked, this, &TitleBar::minimizeClicked);

    m_maximizeBtn = new QToolButton(this);
    m_maximizeBtn->setObjectName("MaximizeButton");
    m_maximizeBtn->setIcon(QIcon(":/icons/icons/maximize.svg"));
    m_maximizeBtn->setIconSize(QSize(16, 16));
    m_maximizeBtn->setFixedSize(30, 30);
    m_maximizeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_maximizeBtn, &QToolButton::clicked, this, &TitleBar::maximizeClicked);

    m_closeBtn = new QToolButton(this);
    m_closeBtn->setObjectName("CloseButton");
    m_closeBtn->setIcon(QIcon(":/icons/icons/close.svg"));
    m_closeBtn->setIconSize(QSize(16, 16));
    m_closeBtn->setFixedSize(30, 30);
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_closeBtn, &QToolButton::clicked, this, &TitleBar::closeClicked);

    layout->addWidget(m_menuBtn);
    layout->addSpacing(8);
    layout->addWidget(m_titleLabel, 1);
    layout->addWidget(m_settingsBtn);
    layout->addSpacing(6);
    layout->addWidget(m_minimizeBtn);
    layout->addWidget(m_maximizeBtn);
    layout->addWidget(m_closeBtn);
}

void TitleBar::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
}

void TitleBar::setVersion(const QString &version)
{
    m_titleLabel->setText(version);
}

void TitleBar::updateMaximizeIcon()
{
    if (window()->isMaximized()) {
        m_maximizeBtn->setIcon(QIcon(":/icons/icons/maximize.svg"));
    } else {
        m_maximizeBtn->setIcon(QIcon(":/icons/icons/maximize.svg"));
    }
}

void TitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragPos = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
        event->accept();
    }
}

void TitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        if (window()->isMaximized()) {
            window()->showNormal();
            m_dragPos = QPoint(width() / 2, height() / 2);
        }
        window()->move(event->globalPosition().toPoint() - m_dragPos);
        event->accept();
    }
}

void TitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    event->accept();
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (window()->isMaximized()) {
            window()->showNormal();
        } else {
            window()->showMaximized();
        }
        updateMaximizeIcon();
        event->accept();
    }
}
