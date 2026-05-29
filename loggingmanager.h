#ifndef LOGGINGMANAGER_H
#define LOGGINGMANAGER_H

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class LoggingManager
{
public:
    struct Settings {
        QString level = QStringLiteral("info");
        bool consoleEnabled = true;
        bool fileEnabled = true;
        bool includeSourceLocation = false;
        bool includeThreadId = true;
        bool includeCategory = true;
        qint64 maxFileBytes = 5 * 1024 * 1024;
        int maxBackupFiles = 10;
        bool perSessionFile = true;
        bool auditEnabled = true;
        qint64 auditMaxFileBytes = 5 * 1024 * 1024;
        int auditMaxBackupFiles = 10;
        bool redactSensitiveData = true;
        QStringList categoryRules;
    };

    static void initialize();
    static void initialize(const Settings &settings);
    static void reconfigure(const Settings &settings);
    static void shutdown();
    static Settings currentSettings();
    static QString logDirectoryPath();
    static QString currentLogFilePath();
    static QString sessionLogFilePath();
    static QString auditLogFilePath();
    static QString diagnosticsDirectoryPath();
    static void audit(const QString &action, const QString &result, const QMap<QString, QString> &fields = {});
    static QString exportDiagnostics(const QString &targetDirectory = QString(), QString *errorMessage = nullptr);
    static QString redact(const QString &text);

private:
    static void rotateLogs(const QString &basePath, int maxFiles, qint64 maxBytes);
};

#endif // LOGGINGMANAGER_H
