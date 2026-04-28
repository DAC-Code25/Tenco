#include "loggingmanager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdlib>
#include <cstdio>

namespace {
QMutex g_logMutex;
QFile g_logFile;
QtMessageHandler g_previousHandler = nullptr;
bool g_initialized = false;
}

void LoggingManager::rotateLogs(const char *basePath, int maxFiles, qint64 maxBytes)
{
    QFile baseFile(QString::fromUtf8(basePath));
    if (!baseFile.exists() || baseFile.size() < maxBytes) {
        return;
    }

    for (int i = maxFiles - 1; i >= 1; --i) {
        const QString src = QStringLiteral("%1.%2").arg(QString::fromUtf8(basePath)).arg(i);
        const QString dst = QStringLiteral("%1.%2").arg(QString::fromUtf8(basePath)).arg(i + 1);
        if (QFile::exists(dst)) {
            QFile::remove(dst);
        }
        if (QFile::exists(src)) {
            QFile::rename(src, dst);
        }
    }

    const QString firstBackup = QStringLiteral("%1.1").arg(QString::fromUtf8(basePath));
    if (QFile::exists(firstBackup)) {
        QFile::remove(firstBackup);
    }
    QFile::rename(QString::fromUtf8(basePath), firstBackup);
}

static void tencoMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context);

    const QString level = [&]() -> QString {
        switch (type) {
        case QtDebugMsg:
            return QStringLiteral("DEBUG");
        case QtInfoMsg:
            return QStringLiteral("INFO");
        case QtWarningMsg:
            return QStringLiteral("WARN");
        case QtCriticalMsg:
            return QStringLiteral("ERROR");
        case QtFatalMsg:
            return QStringLiteral("FATAL");
        }
        return QStringLiteral("UNK");
    }();

    const QString line =
        QStringLiteral("%1 [%2] %3").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")), level, message);

#ifdef Q_OS_WIN
    const QByteArray consoleLine = line.toLocal8Bit();
#else
    const QByteArray consoleLine = line.toUtf8();
#endif
    FILE *consoleStream = (type == QtDebugMsg || type == QtInfoMsg) ? stdout : stderr;
    fprintf(consoleStream, "%s\n", consoleLine.constData());
    fflush(consoleStream);

    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen()) {
        QTextStream out(&g_logFile);
        out << line << '\n';
        out.flush();
        g_logFile.flush();
    }

    if (type == QtFatalMsg) {
        abort();
    }
}

void LoggingManager::initialize()
{
    QMutexLocker locker(&g_logMutex);
    if (g_initialized) {
        return;
    }

    const QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString logsDir = QDir(appDataDir).filePath(QStringLiteral("logs"));
    QDir().mkpath(logsDir);

    const QString logFilePath = QDir(logsDir).filePath(QStringLiteral("tenco.log"));
    rotateLogs(logFilePath.toUtf8().constData(), 5, 2 * 1024 * 1024);

    g_logFile.setFileName(logFilePath);
    g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);

    qSetMessagePattern(QStringLiteral("%{time yyyy-MM-dd hh:mm:ss.zzz} [%{type}] %{category} - %{message}"));
    g_previousHandler = qInstallMessageHandler(tencoMessageHandler);
    g_initialized = true;
}

void LoggingManager::shutdown()
{
    QMutexLocker locker(&g_logMutex);
    if (!g_initialized) {
        return;
    }

    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;

    if (g_logFile.isOpen()) {
        g_logFile.flush();
        g_logFile.close();
    }

    g_initialized = false;
}
