#include "mainwindow.h"
#include "loggingmanager.h"
#include "configmanager.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcMain, "tenco.main")

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Tenco"));
    QCoreApplication::setApplicationName(QStringLiteral("Tenco"));
    LoggingManager::initialize();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Tenco Qt desktop client"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption configOption(
        QStringList{QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Override config file path."),
        QStringLiteral("path"));
    parser.addOption(configOption);
    parser.process(a);

    if (parser.isSet(configOption)) {
        const QString path = parser.value(configOption).trimmed();
        if (!path.isEmpty()) {
            ConfigManager::instance().setConfigFilePath(path);
        }
    }
    QString backupError;
    if (!ConfigManager::instance().createStartupBackup(&backupError)) {
        qCWarning(lcMain) << "Create startup config backup failed:" << backupError;
    }

    QApplication::setWindowIcon(QIcon(":/icon.ico"));
    MainWindow w;
    w.setWindowFlags(Qt::FramelessWindowHint); // 移除系统默认标题栏

    w.show();
    const int code = a.exec();
    LoggingManager::shutdown();
    return code;
}
