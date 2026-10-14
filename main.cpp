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
#ifdef TENCO_TEST_HOOKS
#include "tests/gui_smoke.h"
#endif

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
#ifdef TENCO_TEST_HOOKS
    const QCommandLineOption smokeOption("smoke-output", "Run GUI smoke with loopback-only test config.", "path");
    parser.addOption(smokeOption);
#endif
    parser.process(a);

    if (parser.isSet(configOption)) {
        const QString path = parser.value(configOption).trimmed();
        if (!path.isEmpty()) {
            ConfigManager::instance().setConfigFilePath(path);
        }
    }

#ifdef TENCO_TEST_HOOKS
    if (parser.isSet(smokeOption)) {
        const auto& config = ConfigManager::instance();
        if (!parser.isSet(configOption) || QUrl(config.network().websocketUrl).host() != "127.0.0.1" ||
            QUrl(config.network().statusReadUrl).host() != "127.0.0.1" || config.gimbal().enabled ||
            config.video().autoStart || config.tracking().enabled || config.poseSource().enabled) return 2;
    }
#endif
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
#ifdef TENCO_TEST_HOOKS
    if (parser.isSet(smokeOption)) installGuiSmoke(a, w, parser.value(smokeOption));
#endif
    const int code = a.exec();
    DatabaseManager::instance().shutdown();
    LoggingManager::shutdown();
    return code;
}
