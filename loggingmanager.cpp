#include "loggingmanager.h"

#include "configmanager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QLoggingCategory>
#include <QThread>
#include <QWaitCondition>

#include <cstdio>
#include <cstdlib>

namespace {
QMutex g_logMutex;
QFile g_logFile;
QFile g_sessionLogFile;
QFile g_auditFile;
QtMessageHandler g_previousHandler = nullptr;
LoggingManager::Settings g_settings;
QString g_sessionId;
QString g_logDir;
QString g_logPath;
QString g_sessionLogPath;
QString g_auditPath;
QDateTime g_startTime;
bool g_initialized = false;
QElapsedTimer g_messageClock;
QHash<QString, QPair<qint64, quint64>> g_recentMessages;

struct QueuedLogLine {
    enum class Kind {
        Message,
        Audit
    };

    Kind kind = Kind::Message;
    QtMsgType type = QtInfoMsg;
    QString line;
};

class LogWriterThread : public QThread
{
public:
    void run() override;
};

QMutex g_logQueueMutex;
QWaitCondition g_logQueueReady;
QWaitCondition g_logQueueDrained;
QQueue<QueuedLogLine> g_logQueue;
constexpr int MaxQueuedLines = 2048;
constexpr int MaxLineCharacters = 4096;
quint64 g_droppedLines = 0;
LogWriterThread *g_logWriterThread = nullptr;
bool g_logWriterStop = false;
bool g_logWriterBusy = false;

QString normalizeLevel(QString level)
{
    level = level.trimmed().toLower();
    if (level == QStringLiteral("debug") ||
        level == QStringLiteral("info") ||
        level == QStringLiteral("warn") ||
        level == QStringLiteral("error") ||
        level == QStringLiteral("fatal")) {
        return level;
    }
    return QStringLiteral("info");
}

int levelRank(const QString &level)
{
    const QString normalized = normalizeLevel(level);
    if (normalized == QStringLiteral("debug")) {
        return 0;
    }
    if (normalized == QStringLiteral("info")) {
        return 1;
    }
    if (normalized == QStringLiteral("warn")) {
        return 2;
    }
    if (normalized == QStringLiteral("error")) {
        return 3;
    }
    return 4;
}

int messageRank(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return 0;
    case QtInfoMsg:
        return 1;
    case QtWarningMsg:
        return 2;
    case QtCriticalMsg:
        return 3;
    case QtFatalMsg:
        return 4;
    }
    return 4;
}

QString messageLevel(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("调试");
    case QtInfoMsg:
        return QStringLiteral("信息");
    case QtWarningMsg:
        return QStringLiteral("警告");
    case QtCriticalMsg:
        return QStringLiteral("错误");
    case QtFatalMsg:
        return QStringLiteral("致命");
    }
    return QStringLiteral("UNK");
}

QString defaultLogDirectory()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs"));
}

QString formatThreadId()
{
    const quintptr threadValue = reinterpret_cast<quintptr>(QThread::currentThreadId());
    return QStringLiteral("0x%1").arg(threadValue, 0, 16);
}

QString uptimeText()
{
    if (!g_startTime.isValid()) {
        return QStringLiteral("00:00:00.000");
    }
    const qint64 ms = g_startTime.msecsTo(QDateTime::currentDateTime());
    const qint64 hours = ms / 3600000;
    const qint64 minutes = (ms / 60000) % 60;
    const qint64 seconds = (ms / 1000) % 60;
    const qint64 millis = ms % 1000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(millis, 3, 10, QLatin1Char('0'));
}

QStringList effectiveFilterRules(const LoggingManager::Settings &settings)
{
    QStringList rules = settings.categoryRules;
    // Runtime logs contain key state changes only, even with an old debug config.
    rules << QStringLiteral("*.debug=false");
    return rules;
}

void applyFilterRules(const LoggingManager::Settings &settings)
{
    const QStringList rules = effectiveFilterRules(settings);
    QLoggingCategory::setFilterRules(rules.join(QLatin1Char('\n')));
}

bool ensureDirectory(const QString &path)
{
    if (path.trimmed().isEmpty()) {
        return false;
    }
    return QDir().mkpath(path);
}

void closeFile(QFile &file)
{
    if (!file.isOpen()) {
        return;
    }
    file.flush();
    file.close();
}

bool openAppendFile(QFile &file, const QString &path)
{
    closeFile(file);
    if (path.trimmed().isEmpty()) {
        return false;
    }
    file.setFileName(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void rotateFileSet(const QString &basePath, int maxFiles, qint64 maxBytes)
{
    if (basePath.trimmed().isEmpty() || maxFiles <= 0 || maxBytes <= 0) {
        return;
    }

    QFile baseFile(basePath);
    if (!baseFile.exists() || baseFile.size() < maxBytes) {
        return;
    }

    for (int i = maxFiles - 1; i >= 1; --i) {
        const QString src = QStringLiteral("%1.%2").arg(basePath).arg(i);
        const QString dst = QStringLiteral("%1.%2").arg(basePath).arg(i + 1);
        if (QFile::exists(dst)) {
            QFile::remove(dst);
        }
        if (QFile::exists(src)) {
            QFile::rename(src, dst);
        }
    }

    const QString firstBackup = QStringLiteral("%1.1").arg(basePath);
    if (QFile::exists(firstBackup)) {
        QFile::remove(firstBackup);
    }
    QFile::rename(basePath, firstBackup);
}

QString redactUrlQuery(const QString &text)
{
    QString redacted = text;
    const QStringList keys{
        QStringLiteral("token"),
        QStringLiteral("authToken"),
        QStringLiteral("password"),
        QStringLiteral("secret"),
        QStringLiteral("key")
    };
    for (const QString &key : keys) {
        const QRegularExpression rx(QStringLiteral("([?&]%1=)([^\\s&\\\"]+)").arg(QRegularExpression::escape(key)),
                                    QRegularExpression::CaseInsensitiveOption);
        redacted.replace(rx, QStringLiteral("\\1***REDACTED***"));
    }
    return redacted;
}

QString redactSensitiveText(const QString &text)
{
    QString redacted = text;
    redacted.replace(QRegularExpression(QStringLiteral("(Authorization\\s*[:=]\\s*Bearer\\s+)([^\\s,;\\\"]+)"),
                                        QRegularExpression::CaseInsensitiveOption),
                     QStringLiteral("\\1***REDACTED***"));
    redacted.replace(QRegularExpression(QStringLiteral("((authToken|token|password|secret)\\s*[=:]\\s*)([^\\s,;\\\"]+)"),
                                        QRegularExpression::CaseInsensitiveOption),
                     QStringLiteral("\\1***REDACTED***"));
    redacted.replace(QRegularExpression(QStringLiteral("(\"(authToken|token|password|secret)\"\\s*:\\s*\")([^\"]*)(\")"),
                                        QRegularExpression::CaseInsensitiveOption),
                     QStringLiteral("\\1***REDACTED***\\4"));
    return redactUrlQuery(redacted);
}

QString formatMessageLine(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QStringList parts;
    parts << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    parts << QStringLiteral("[%1]").arg(messageLevel(type));
    if (g_settings.includeCategory && context.category && QByteArray(context.category) != "default") {
        parts << QStringLiteral("[%1]").arg(QString::fromUtf8(context.category));
    }
    if (g_settings.includeThreadId) {
        parts << QStringLiteral("[tid=%1]").arg(formatThreadId());
    }
    if (!g_sessionId.isEmpty()) {
        parts << QStringLiteral("[sid=%1]").arg(g_sessionId);
    }
    parts << QStringLiteral("[uptime=%1]").arg(uptimeText());
    if (g_settings.includeSourceLocation && context.file) {
        parts << QStringLiteral("[%1:%2]").arg(QFileInfo(QString::fromUtf8(context.file)).fileName()).arg(context.line);
    }
    parts << message;
    QString line = parts.join(QLatin1Char(' '));
    if (g_settings.redactSensitiveData) {
        line = redactSensitiveText(line);
    }
    return line;
}

void writeLine(QFile &file, const QString &line)
{
    if (!file.isOpen()) {
        return;
    }
    file.write(line.toUtf8());
    file.write("\n", 1);
}

void writeConsole(QtMsgType type, const QString &line)
{
    if (!g_settings.consoleEnabled) {
        return;
    }
#ifdef Q_OS_WIN
    const QByteArray consoleLine = line.toLocal8Bit();
#else
    const QByteArray consoleLine = line.toUtf8();
#endif
    FILE *consoleStream = (type == QtDebugMsg || type == QtInfoMsg) ? stdout : stderr;
    fprintf(consoleStream, "%s\n", consoleLine.constData());
    fflush(consoleStream);
}

void writeQueuedLogLineLocked(const QueuedLogLine &entry)
{
    if (!g_settings.fileEnabled || !g_logFile.isOpen()) {
        return;
    }
    // Keep a single bounded file for this process run. At the limit retain the
    // newest half in the same file, including a visible truncation marker.
    const auto bytes = entry.line.toUtf8().size() + 1;
    if (g_logFile.size() + bytes > g_settings.maxFileBytes) {
        g_logFile.flush();
        QFile source(g_logPath);
        QByteArray tail;
        if (source.open(QIODevice::ReadOnly)) {
            source.seek(qMax<qint64>(0, source.size() - g_settings.maxFileBytes / 2));
            tail = source.readAll();
            const int newline = tail.indexOf('\n');
            if (newline >= 0) tail.remove(0, newline + 1);
        }
        source.close();
        closeFile(g_logFile);
        QSaveFile compact(g_logPath);
        if (compact.open(QIODevice::WriteOnly)) {
            compact.write(QStringLiteral("[运行 %1] 日志达到容量上限，保留本次运行的最新记录。\n").arg(g_sessionId).toUtf8());
            compact.write(tail);
            compact.commit();
        }
        openAppendFile(g_logFile, g_logPath);
        if (g_logFile.size() + bytes > g_settings.maxFileBytes) return;
    }
    writeLine(g_logFile, entry.line);
}

void LogWriterThread::run()
{
    while (true) {
        QQueue<QueuedLogLine> batch;
        {
            QMutexLocker queueLocker(&g_logQueueMutex);
            while (g_logQueue.isEmpty() && !g_logWriterStop) {
                g_logQueueReady.wait(&g_logQueueMutex);
            }
            if (g_logWriterStop && g_logQueue.isEmpty()) {
                g_logQueueDrained.wakeAll();
                return;
            }
            g_logWriterBusy = true;
            batch.swap(g_logQueue);
        }

        {
            QMutexLocker logLocker(&g_logMutex);
            while (!batch.isEmpty()) {
                writeQueuedLogLineLocked(batch.dequeue());
            }
            if (g_logFile.isOpen()) g_logFile.flush();
        }

        {
            QMutexLocker queueLocker(&g_logQueueMutex);
            g_logWriterBusy = false;
            if (g_logQueue.isEmpty()) {
                g_logQueueDrained.wakeAll();
            }
        }
    }
}

void enqueueLogLine(const QueuedLogLine &entry)
{
    {
        QMutexLocker queueLocker(&g_logQueueMutex);
        if (!g_logWriterThread) {
            queueLocker.unlock();
            QMutexLocker logLocker(&g_logMutex);
            writeQueuedLogLineLocked(entry);
            return;
        }
        if (g_logQueue.size() >= MaxQueuedLines) {
            ++g_droppedLines;
            if (entry.type == QtCriticalMsg || entry.type == QtFatalMsg) g_logQueue.dequeue();
            else return;
        }
        if (g_droppedLines && g_logQueue.size() < MaxQueuedLines - 1) {
            g_logQueue.enqueue({QueuedLogLine::Kind::Message, QtWarningMsg,
                QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz") +
                QStringLiteral(" [警告] 日志队列繁忙，已省略 %1 条重复或过量记录。").arg(g_droppedLines)});
            g_droppedLines = 0;
        }
        auto boundedEntry = entry;
        boundedEntry.line = boundedEntry.line.left(MaxLineCharacters);
        g_logQueue.enqueue(boundedEntry);
        g_logQueueReady.wakeOne();
    }
}

void flushLogQueue()
{
    QMutexLocker queueLocker(&g_logQueueMutex);
    while (!g_logQueue.isEmpty() || g_logWriterBusy) {
        g_logQueueDrained.wait(&g_logQueueMutex, 3000);
    }
}

void startLogWriterThread()
{
    QMutexLocker queueLocker(&g_logQueueMutex);
    if (g_logWriterThread) {
        return;
    }
    g_logWriterStop = false;
    g_logWriterBusy = false;
    g_logWriterThread = new LogWriterThread;
    g_logWriterThread->setObjectName(QStringLiteral("TencoLogWriter"));
    g_logWriterThread->start();
}

void stopLogWriterThread()
{
    LogWriterThread *thread = nullptr;
    {
        QMutexLocker queueLocker(&g_logQueueMutex);
        thread = g_logWriterThread;
        if (!thread) {
            return;
        }
        g_logWriterStop = true;
        g_logQueueReady.wakeAll();
    }

    // No forced thread termination while holding a logging mutex or file.
    thread->wait();

    {
        QMutexLocker queueLocker(&g_logQueueMutex);
        g_logWriterThread = nullptr;
        g_logWriterStop = false;
        g_logWriterBusy = false;
        g_logQueue.clear();
        g_logQueueDrained.wakeAll();
    }
    delete thread;
}

void tencoMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QString line;
    bool shouldWriteFile = false;
    {
        QMutexLocker locker(&g_logMutex);
        if (type == QtDebugMsg) return;
        if (messageRank(type) < levelRank(g_settings.level) && type != QtFatalMsg) {
            return;
        }

        QString boundedMessage = message.left(MaxLineCharacters);
        if (type != QtFatalMsg && g_messageClock.isValid()) {
            const QString key = QString::number(int(type)) + QLatin1Char(':') +
                QString::fromUtf8(context.category ? context.category : "default") + QLatin1Char(':') + boundedMessage.left(512);
            const qint64 now = g_messageClock.elapsed();
            auto it = g_recentMessages.find(key);
            if (it != g_recentMessages.end() && now - it.value().first < 5000) { ++it.value().second; return; }
            if (it != g_recentMessages.end() && it.value().second > 0)
                boundedMessage += QStringLiteral("（5 秒内相同记录已省略 %1 次）").arg(it.value().second);
            if (g_recentMessages.size() >= 256) g_recentMessages.clear();
            g_recentMessages.insert(key, {now, 0});
        }
        line = formatMessageLine(type, context, boundedMessage);
        writeConsole(type, line);
        shouldWriteFile = g_settings.fileEnabled;
    }

    if (shouldWriteFile) {
        enqueueLogLine(QueuedLogLine{QueuedLogLine::Kind::Message, type, line});
    }
    if (type == QtFatalMsg) {
        flushLogQueue();
        abort();
    }
}

QJsonObject redactedConfigSummary()
{
    QJsonObject root;
    root.insert(QStringLiteral("application"), QCoreApplication::applicationName());
    root.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
    root.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("runtimeDir"), QCoreApplication::applicationDirPath());
    root.insert(QStringLiteral("logDir"), g_logDir);
    root.insert(QStringLiteral("logFile"), g_logPath);
    root.insert(QStringLiteral("sessionLogFile"), g_sessionLogPath);
    root.insert(QStringLiteral("auditLogFile"), g_auditPath);
    root.insert(QStringLiteral("sessionId"), g_sessionId);
    root.insert(QStringLiteral("generatedAt"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));

    const auto settings = g_settings;
    root.insert(QStringLiteral("logging"), QJsonObject{
                                            {QStringLiteral("level"), settings.level},
                                            {QStringLiteral("consoleEnabled"), settings.consoleEnabled},
                                            {QStringLiteral("fileEnabled"), settings.fileEnabled},
                                            {QStringLiteral("includeSourceLocation"), settings.includeSourceLocation},
                                            {QStringLiteral("includeThreadId"), settings.includeThreadId},
                                            {QStringLiteral("includeCategory"), settings.includeCategory},
                                            {QStringLiteral("maxFileBytes"), settings.maxFileBytes},
                                            {QStringLiteral("maxBackupFiles"), settings.maxBackupFiles},
                                            {QStringLiteral("perSessionFile"), settings.perSessionFile},
                                            {QStringLiteral("auditEnabled"), settings.auditEnabled},
                                            {QStringLiteral("auditMaxFileBytes"), settings.auditMaxFileBytes},
                                            {QStringLiteral("auditMaxBackupFiles"), settings.auditMaxBackupFiles},
                                            {QStringLiteral("redactSensitiveData"), settings.redactSensitiveData},
                                            {QStringLiteral("categoryRules"), QJsonArray::fromStringList(settings.categoryRules)},
                                        });

    const ConfigManager &config = ConfigManager::instance();
    root.insert(QStringLiteral("config"), QJsonObject{
                                           {QStringLiteral("configFilePath"), config.configFilePath()},
                                           {QStringLiteral("loadedFromFile"), config.loadedFromFile()},
                                           {QStringLiteral("backupDirectory"), config.backupDirectoryPath()},
                                       });
    return root;
}

bool copyFileIfExists(const QString &sourcePath, const QString &targetPath, QString *errorMessage)
{
    if (sourcePath.isEmpty() || !QFile::exists(sourcePath)) {
        return true;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("无法读取文件：%1").arg(sourcePath);
        }
        return false;
    }
    const QFileInfo targetInfo(targetPath);
    if (!ensureDirectory(targetInfo.absolutePath())) {
        if (errorMessage) {
            *errorMessage = QObject::tr("无法创建目录：%1").arg(targetInfo.absolutePath());
        }
        return false;
    }
    QFile::remove(targetPath);
    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("无法写入文件：%1").arg(targetPath);
        }
        return false;
    }
    target.write(source.readAll());
    if (!target.commit()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("无法提交文件：%1").arg(targetPath);
        }
        return false;
    }
    return true;
}
}

void LoggingManager::rotateLogs(const QString &basePath, int maxFiles, qint64 maxBytes)
{
    rotateFileSet(basePath, maxFiles, maxBytes);
}

void LoggingManager::initialize()
{
    initialize(Settings{});
}

void LoggingManager::initialize(const Settings &settings)
{
    QMutexLocker locker(&g_logMutex);
    if (g_initialized) {
        return;
    }

    g_settings = settings;
    g_settings.level = normalizeLevel(g_settings.level);
    g_settings.maxFileBytes = qBound<qint64>(qint64(256 * 1024), g_settings.maxFileBytes, qint64(16 * 1024 * 1024));
    g_settings.maxBackupFiles = qBound(1, g_settings.maxBackupFiles, 99);
    g_settings.auditMaxFileBytes = qBound<qint64>(qint64(256 * 1024), g_settings.auditMaxFileBytes, qint64(256 * 1024 * 1024));
    g_settings.auditMaxBackupFiles = qBound(1, g_settings.auditMaxBackupFiles, 99);

    g_startTime = QDateTime::currentDateTime();
    g_messageClock.start();
    g_recentMessages.clear();
    g_sessionId = g_startTime.toString(QStringLiteral("yyyyMMdd_hhmmss_zzz")) + "_" + QString::number(QCoreApplication::applicationPid());
    g_logDir = defaultLogDirectory();
    ensureDirectory(g_logDir);
    g_logPath = QDir(g_logDir).filePath(QStringLiteral("tenco_%1.log").arg(g_sessionId));
    g_sessionLogPath = g_logPath;
    g_auditPath.clear();
    g_settings.perSessionFile = true;
    if (g_settings.fileEnabled) {
        g_logFile.setFileName(g_logPath);
        // NewOnly prevents appending a new run even after a clock rollback.
        int collision = 0;
        while (!g_logFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (!QFile::exists(g_logFile.fileName()) || ++collision > 100) break;
            g_logPath = QDir(g_logDir).filePath(QStringLiteral("tenco_%1_%2.log").arg(g_sessionId).arg(collision));
            g_logFile.setFileName(g_logPath);
        }
        g_sessionLogPath = g_logPath;
        writeLine(g_logFile, QStringLiteral("[%1] 上位机启动，关键操作与状态变更统一记录于本文件。容量上限 %2 MiB。")
            .arg(g_startTime.toString(Qt::ISODateWithMs)).arg(g_settings.maxFileBytes / (1024.0 * 1024.0), 0, 'f', 1));
    }

    startLogWriterThread();
    applyFilterRules(g_settings);
    g_previousHandler = qInstallMessageHandler(tencoMessageHandler);
    g_initialized = true;
}

void LoggingManager::reconfigure(const Settings &settings)
{
    flushLogQueue();
    QMutexLocker locker(&g_logMutex);
    if (!g_initialized) {
        g_settings = settings;
        return;
    }

    g_settings = settings;
    g_settings.level = normalizeLevel(g_settings.level);
    g_settings.maxFileBytes = qBound<qint64>(qint64(256 * 1024), g_settings.maxFileBytes, qint64(16 * 1024 * 1024));
    g_settings.perSessionFile = true;
    g_settings.maxBackupFiles = qBound(1, g_settings.maxBackupFiles, 99);
    g_settings.auditMaxFileBytes = qBound<qint64>(qint64(256 * 1024), g_settings.auditMaxFileBytes, qint64(256 * 1024 * 1024));
    g_settings.auditMaxBackupFiles = qBound(1, g_settings.auditMaxBackupFiles, 99);

    closeFile(g_logFile);
    closeFile(g_sessionLogFile);
    closeFile(g_auditFile);

    if (g_settings.fileEnabled) {
        openAppendFile(g_logFile, g_logPath);
    }

    applyFilterRules(g_settings);
}

void LoggingManager::shutdown()
{
    {
        QMutexLocker locker(&g_logMutex);
        if (!g_initialized) {
            return;
        }

        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
        g_initialized = false;
    }

    flushLogQueue();
    stopLogWriterThread();

    {
        QMutexLocker locker(&g_logMutex);
        if (g_logFile.isOpen()) writeQueuedLogLineLocked({QueuedLogLine::Kind::Message, QtInfoMsg,
            QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + QStringLiteral(" 上位机本次运行结束。")});
        closeFile(g_logFile);
        closeFile(g_sessionLogFile);
        closeFile(g_auditFile);
    }
}

LoggingManager::Settings LoggingManager::currentSettings()
{
    QMutexLocker locker(&g_logMutex);
    return g_settings;
}

QString LoggingManager::logDirectoryPath()
{
    QMutexLocker locker(&g_logMutex);
    return g_logDir.isEmpty() ? defaultLogDirectory() : g_logDir;
}

QString LoggingManager::currentLogFilePath()
{
    QMutexLocker locker(&g_logMutex);
    return g_logPath;
}

QString LoggingManager::sessionLogFilePath()
{
    QMutexLocker locker(&g_logMutex);
    return g_sessionLogPath;
}

QString LoggingManager::auditLogFilePath()
{
    QMutexLocker locker(&g_logMutex);
    return g_auditPath;
}

QString LoggingManager::diagnosticsDirectoryPath()
{
    return QDir(logDirectoryPath()).filePath(QStringLiteral("diagnostics"));
}

void LoggingManager::audit(const QString &action, const QString &result, const QMap<QString, QString> &fields)
{
    QString line;
    bool shouldQueue = false;
    {
        QMutexLocker locker(&g_logMutex);
        // Compatibility entry point: all operations share the runtime file.
        // Repeated motion samples and jog commands are deliberately omitted.
        if (!g_settings.fileEnabled || action == "chassis.manual_move" || action == "gimbal.jog" || action == "gimbal.stop") {
            return;
        }

        QStringList parts;
        parts << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
        parts << QStringLiteral("[操作]");
        static const QMap<QString, QString> actions{{"config.save", "保存配置"}, {"config.reload", "应用配置"},
            {"config.load", "读取配置"}, {"config.restore_latest", "恢复配置"}, {"diagnostics.export", "导出诊断"},
            {"safety.estop", "紧急停车"}, {"safety.motion_stop", "安全停止"}, {"map.load", "加载地图"},
            {"map.save", "保存地图"}, {"map.new", "新建地图"}, {"ipc.operation", "工控机维护"}};
        static const QMap<QString, QString> results{{"success", "成功"}, {"failed", "失败"}, {"rejected", "拒绝"},
            {"accepted", "已受理"}, {"applied", "已应用"}, {"saved", "已保存"}, {"preview", "预览"},
            {"submitted", "已提交"}, {"cancelled", "已取消"}};
        parts << actions.value(action, action.trimmed());
        parts << QStringLiteral("结果=%1").arg(results.value(result, result.trimmed()));
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            QString value = it.value();
            if (value.contains(QLatin1Char(' '))) {
                value = QStringLiteral("\"%1\"").arg(value);
            }
            parts << QStringLiteral("%1=%2").arg(it.key(), value);
        }
        line = parts.join(QLatin1Char(' ')).left(MaxLineCharacters);
        if (g_settings.redactSensitiveData) {
            line = redactSensitiveText(line);
        }
        shouldQueue = true;
    }

    if (shouldQueue) {
        enqueueLogLine(QueuedLogLine{QueuedLogLine::Kind::Audit, QtInfoMsg, line});
    }
}

QString LoggingManager::exportDiagnostics(const QString &targetDirectory, QString *errorMessage)
{
    flushLogQueue();

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
    const QString rootDir = targetDirectory.trimmed().isEmpty()
                                ? QDir(diagnosticsDirectoryPath()).filePath(QStringLiteral("tenco_diag_%1").arg(timestamp))
                                : targetDirectory;
    if (!ensureDirectory(rootDir)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("无法创建诊断目录：%1").arg(rootDir);
        }
        return QString();
    }

    const QString logsDir = QDir(rootDir).filePath(QStringLiteral("logs"));
    const QString configDir = QDir(rootDir).filePath(QStringLiteral("config"));
    const QString runtimeDir = QDir(rootDir).filePath(QStringLiteral("runtime"));
    ensureDirectory(logsDir);
    ensureDirectory(configDir);
    ensureDirectory(runtimeDir);

    {
        QMutexLocker locker(&g_logMutex);
        closeFile(g_logFile);
        closeFile(g_sessionLogFile);
        closeFile(g_auditFile);
        copyFileIfExists(g_logPath, QDir(logsDir).filePath(QFileInfo(g_logPath).fileName()), errorMessage);
        if (g_settings.fileEnabled) {
            openAppendFile(g_logFile, g_logPath);
        }
    }

    const ConfigManager &config = ConfigManager::instance();
    if (QFile::exists(config.configFilePath())) {
        QFile configFile(config.configFilePath());
        if (configFile.open(QIODevice::ReadOnly)) {
            QString text = QString::fromUtf8(configFile.readAll());
            text = redactSensitiveText(text);
            QSaveFile out(QDir(configDir).filePath(QStringLiteral("config.redacted.json")));
            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                out.write(text.toUtf8());
                out.commit();
            }
        }
    }

    QSaveFile diagnostics(QDir(rootDir).filePath(QStringLiteral("diagnostics.json")));
    if (diagnostics.open(QIODevice::WriteOnly | QIODevice::Text)) {
        diagnostics.write(QJsonDocument(redactedConfigSummary()).toJson(QJsonDocument::Indented));
        diagnostics.commit();
    }

    QSaveFile environment(QDir(runtimeDir).filePath(QStringLiteral("environment.txt")));
    if (environment.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&environment);
        out << "Application: " << QCoreApplication::applicationName() << '\n';
        out << "Version: " << QCoreApplication::applicationVersion() << '\n';
        out << "Qt: " << qVersion() << '\n';
        out << "RuntimeDir: " << QCoreApplication::applicationDirPath() << '\n';
        out << "ConfigPath: " << config.configFilePath() << '\n';
        out << "LogDir: " << logDirectoryPath() << '\n';
        out.flush();
        environment.commit();
    }

    audit(QStringLiteral("diagnostics.export"), QStringLiteral("success"), {{QStringLiteral("target"), rootDir}});
    return rootDir;
}

QString LoggingManager::redact(const QString &text)
{
    return redactSensitiveText(text);
}
