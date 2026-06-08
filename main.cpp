#include "mainwindow.h"
#include "databasemanager.h"
#include "loggingmanager.h"
#include "configmanager.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QSurfaceFormat>

#ifdef TENCO_ENABLE_ABOUT_3D
#include <QtQuick3D/qquick3d.h>
#endif

Q_LOGGING_CATEGORY(lcMain, "tenco.main")

static LoggingManager::Settings loggingSettingsFromConfig()
{
    const auto logging = ConfigManager::instance().logging();
    LoggingManager::Settings settings;
    settings.level = logging.level;
    settings.consoleEnabled = logging.consoleEnabled;
    settings.fileEnabled = logging.fileEnabled;
    settings.includeSourceLocation = logging.includeSourceLocation;
    settings.includeThreadId = logging.includeThreadId;
    settings.includeCategory = logging.includeCategory;
    settings.maxFileBytes = logging.maxFileBytes;
    settings.maxBackupFiles = logging.maxBackupFiles;
    settings.perSessionFile = logging.perSessionFile;
    settings.auditEnabled = logging.auditEnabled;
    settings.auditMaxFileBytes = logging.auditMaxFileBytes;
    settings.auditMaxBackupFiles = logging.auditMaxBackupFiles;
    settings.redactSensitiveData = logging.redactSensitiveData;
    settings.categoryRules = logging.categoryRules;
    return settings;
}

int main(int argc, char *argv[])
{
#ifdef TENCO_ENABLE_ABOUT_3D
    QSurfaceFormat::setDefaultFormat(QQuick3D::idealSurfaceFormat());
#endif

    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Tenco"));
    QCoreApplication::setApplicationName(QStringLiteral("Tenco"));

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

    LoggingManager::initialize(loggingSettingsFromConfig());
    QObject::connect(&ConfigManager::instance(), &ConfigManager::configChanged, []() {
        LoggingManager::reconfigure(loggingSettingsFromConfig());
        LoggingManager::audit(QStringLiteral("config.reload"), QStringLiteral("success"));
        QString dbError;
        if (!DatabaseManager::instance().initialize(ConfigManager::instance().database(), &dbError)) {
            qCWarning(lcMain) << "Database reinitialize failed:" << dbError;
        }
    });

    QString backupError;
    if (!ConfigManager::instance().createStartupBackup(&backupError)) {
        qCWarning(lcMain) << "Create startup config backup failed:" << backupError;
    }

    QString dbError;
    if (!DatabaseManager::instance().initialize(ConfigManager::instance().database(), &dbError)) {
        qCWarning(lcMain) << "Database initialization failed:" << dbError;
    }

    QApplication::setWindowIcon(QIcon(":/icon.ico"));
    MainWindow w;
    w.setWindowFlags(Qt::FramelessWindowHint); // 移除系统默认标题栏

    w.show();
    const int code = a.exec();
    DatabaseManager::instance().shutdown();
    LoggingManager::shutdown();
    return code;
}
