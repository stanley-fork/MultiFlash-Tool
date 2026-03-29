#include "mainwindow.h"

#include <QCoreApplication>
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QNetworkProxyFactory>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    /* 与系统浏览器一致：使用 Windows「代理 / PAC」等配置。默认 Qt 常直连，易在需代理网络下超时。 */
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    QCoreApplication::setOrganizationName(QStringLiteral("SAKURAEDL"));
    QApplication::setApplicationName(QStringLiteral("SAKURAEDL"));
    QApplication::setApplicationVersion(QStringLiteral("4.0.0"));
    /* 关闭主窗口即退出事件循环，避免无可见窗口时进程仍驻留 */
    QApplication::setQuitOnLastWindowClosed(true);

    QFont defaultFont("Segoe UI", 10);
    defaultFont.setStyleStrategy(QFont::PreferAntialias);
    QApplication::setFont(defaultFont);

    QFile styleFile(":/style/style/dark_theme.qss");
    if (styleFile.open(QFile::ReadOnly | QFile::Text)) {
        QString styleSheet = styleFile.readAll();
        a.setStyleSheet(styleSheet);
        styleFile.close();
    }

    MainWindow w;
    w.show();
    return QApplication::exec();
}
