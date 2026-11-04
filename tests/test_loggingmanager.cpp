#include "loggingmanager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

class LoggingManagerTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setApplicationName("TencoLoggingTest");
    }
    void defaultDirectoryFollowsExecutableNotWorkingDirectory() {
        const QString expected = QDir(QCoreApplication::applicationDirPath()).filePath("logs");
        const QString originalCwd = QDir::currentPath();
        QTemporaryDir otherWorkingDirectory;
        QVERIFY(otherWorkingDirectory.isValid());
        QVERIFY(QDir::setCurrent(otherWorkingDirectory.path()));
        const QString beforeInitialization = LoggingManager::logDirectoryPath();
        LoggingManager::Settings settings; settings.consoleEnabled = false;
        LoggingManager::initialize(settings);
        const QString initialized = LoggingManager::logDirectoryPath();
        const QString file = LoggingManager::currentLogFilePath();
        LoggingManager::shutdown();
        QVERIFY(QDir::setCurrent(originalCwd));
        QCOMPARE(beforeInitialization, expected);
        QCOMPARE(initialized, expected);
        QCOMPARE(QFileInfo(file).absolutePath(), expected);
        QVERIFY(QFileInfo(file).isFile());
        QVERIFY(!QDir(otherWorkingDirectory.filePath("logs")).exists());
    }
    void singleRunFileAndRedaction() {
        LoggingManager::Settings settings; settings.consoleEnabled = false;
        settings.auditEnabled = false; settings.perSessionFile = false;
        LoggingManager::initialize(settings);
        const QString path = LoggingManager::currentLogFilePath();
        const QString directory = LoggingManager::logDirectoryPath();
        QCOMPARE(LoggingManager::sessionLogFilePath(), path);
        QVERIFY(LoggingManager::auditLogFilePath().isEmpty());
        QVERIFY(QFileInfo(path).fileName().startsWith("tenco_"));
        qInfo("连接状态已改变");
        LoggingManager::audit("config.save", "saved", {{"token", "confidential-test-value"}});
        LoggingManager::audit("chassis.manual_move", "start", {{"source", "motion-sample-sentinel"}});
        qDebug("debug-sample-sentinel");
        LoggingManager::reconfigure(settings);
        QCOMPARE(LoggingManager::currentLogFilePath(), path);
        LoggingManager::shutdown();
        QFile log(path); QVERIFY(log.open(QIODevice::ReadOnly)); const auto bytes = log.readAll();
        QVERIFY(bytes.contains(QString("连接状态已改变").toUtf8()));
        QVERIFY(bytes.contains(QString("保存配置").toUtf8()));
        QVERIFY(!bytes.contains("confidential-test-value"));
        QVERIFY(!bytes.contains("motion-sample-sentinel"));
        QVERIFY(!bytes.contains("debug-sample-sentinel"));
        LoggingManager::initialize(settings);
        const QString next = LoggingManager::currentLogFilePath();
        QVERIFY(next != path); QCOMPARE(QFileInfo(next).absolutePath(), directory);
        LoggingManager::shutdown();
    }
    void boundedFileKeepsLatestRecords() {
        LoggingManager::Settings settings; settings.consoleEnabled = false; settings.maxFileBytes = 256 * 1024;
        LoggingManager::initialize(settings);
        const QString path = LoggingManager::currentLogFilePath();
        for (int i = 0; i < 600; ++i) qWarning().noquote() << QString::number(i) + QString(1600, 'X');
        LoggingManager::reconfigure(settings); // Drain the bounded asynchronous queue.
        qCritical("latest-critical-sentinel");
        LoggingManager::shutdown();
        QFile log(path); QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.size() <= settings.maxFileBytes);
        const auto bytes = log.readAll();
        QVERIFY(bytes.contains("latest-critical-sentinel"));
        QVERIFY(bytes.contains(QString("容量上限").toUtf8()));
        QVERIFY(!QFile::exists(path + ".1"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (qEnvironmentVariableIsSet("TENCO_LOGGING_TEST_ISOLATED")) {
        LoggingManagerTest test;
        return QTest::qExec(&test, argc, argv);
    }
    // Exercise the real default path, while keeping all generated run logs
    // inside a temporary executable directory rather than the application's
    // deployment directory or the build tree.
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const QString isolatedExecutable = temporary.filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
    if (!QFile::copy(QCoreApplication::applicationFilePath(), isolatedExecutable)) return 2;
    const auto permissions = QFile::permissions(QCoreApplication::applicationFilePath());
    if (!QFile::setPermissions(isolatedExecutable, permissions)) return 2;
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("TENCO_LOGGING_TEST_ISOLATED", "1");
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.setWorkingDirectory(temporary.path());
    child.start(isolatedExecutable, QCoreApplication::arguments().mid(1));
    if (!child.waitForStarted(10000)) return 2;
    if (!child.waitForFinished(30000)) { child.kill(); child.waitForFinished(1000); return 2; }
    return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 2;
}
#include "test_loggingmanager.moc"
