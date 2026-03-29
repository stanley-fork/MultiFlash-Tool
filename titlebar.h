#ifndef TITLEBAR_H
#define TITLEBAR_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QHBoxLayout>
#include <QMouseEvent>

class TitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit TitleBar(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setVersion(const QString &version);
    void updateMaximizeIcon();

signals:
    void menuClicked();
    void settingsClicked();
    void minimizeClicked();
    void maximizeClicked();
    void closeClicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QToolButton *m_menuBtn;
    QLabel *m_titleLabel;
    QToolButton *m_settingsBtn;
    QToolButton *m_minimizeBtn;
    QToolButton *m_maximizeBtn;
    QToolButton *m_closeBtn;

    QPoint m_dragPos;
    bool m_dragging = false;
};

#endif // TITLEBAR_H
