#include "databasemanager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStringList>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

Q_LOGGING_CATEGORY(lcDatabaseManager, "tenco.database")

namespace {
constexpr int kSchemaVersion = 1;

QString sqlErrorText(const QSqlError &error)
{
    const QString databaseText = error.databaseText().trimmed();
    const QString driverText = error.driverText().trimmed();
    if (!databaseText.isEmpty() && !driverText.isEmpty() && databaseText != driverText) {
        return QStringLiteral("%1; %2").arg(driverText, databaseText);
    }
    if (!databaseText.isEmpty()) {
        return databaseText;
    }
    return driverText;
}
}

DatabaseManager &DatabaseManager::instance()
{
    static DatabaseManager s_instance;
    return s_instance;
}

DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
{
}

bool DatabaseManager::initialize(const ConfigManager::DatabaseConfig &config, QString *errorMessage)
{
    shutdown();

    m_config = config;
    m_connectionName = config.connectionName.trimmed().isEmpty()
                           ? QStringLiteral("tenco_main")
                           : config.connectionName.trimmed();

    const bool ok = (config.backend == QStringLiteral("postgresql"))
                        ? configurePostgreSQL(errorMessage)
                        : configureSQLite(errorMessage);
    m_initialized = ok;
    if (ok) {
        qCInfo(lcDatabaseManager) << "Database initialized"
                                  << "backend=" << m_config.backend
                                  << "connection=" << m_connectionName
                                  << "database=" << m_databasePath;
    } else {
        qCWarning(lcDatabaseManager) << "Database initialization failed"
                                     << "backend=" << m_config.backend
                                     << "error=" << (errorMessage ? *errorMessage : QString());
    }
    return ok;
}

void DatabaseManager::shutdown()
{
    if (m_connectionName.isEmpty()) {
        return;
    }

    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isValid()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
    m_initialized = false;
    m_databasePath.clear();
}

bool DatabaseManager::isOpen() const
{
    if (!m_initialized || m_connectionName.isEmpty()) {
        return false;
    }
    const QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    return db.isValid() && db.isOpen();
}

int DatabaseManager::schemaVersion(QString *errorMessage) const
{
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen()) {
        if (errorMessage) {
            *errorMessage = tr("数据库未打开");
        }
        return -1;
    }

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT version FROM schema_migrations ORDER BY version DESC LIMIT 1"))) {
        if (errorMessage) {
            *errorMessage = sqlErrorText(query.lastError());
        }
        return -1;
    }
    if (!query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

QSqlDatabase DatabaseManager::database() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

bool DatabaseManager::configureSQLite(QString *errorMessage)
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        if (errorMessage) {
            *errorMessage = tr("当前 Qt 环境缺少 QSQLITE 驱动");
        }
        return false;
    }

    const QString sqlitePath = m_config.sqliteFilePath.trimmed().isEmpty()
                                   ? defaultSQLitePath()
                                   : m_config.sqliteFilePath.trimmed();
    const QFileInfo dbInfo(sqlitePath);
    const QDir dbDir = dbInfo.absoluteDir();
    if (!dbDir.exists() && !QDir().mkpath(dbDir.absolutePath())) {
        if (errorMessage) {
            *errorMessage = tr("无法创建数据库目录：%1").arg(dbDir.absolutePath());
        }
        return false;
    }

    bool success = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        db.setDatabaseName(dbInfo.absoluteFilePath());
        if (!db.open()) {
            if (errorMessage) {
                *errorMessage = sqlErrorText(db.lastError());
            }
        } else {
            m_databasePath = dbInfo.absoluteFilePath();
            if (applySQLitePragmas(db, errorMessage) && initializeSchema(db, errorMessage)) {
                success = true;
            }
        }
    }
    if (!success) {
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }
    return true;
}

bool DatabaseManager::configurePostgreSQL(QString *errorMessage)
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QPSQL"))) {
        if (errorMessage) {
            *errorMessage = tr("当前 Qt 环境缺少 QPSQL 驱动，生产中心库建议部署 PostgreSQL 服务后再启用");
        }
        return false;
    }

    bool success = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), m_connectionName);
        db.setHostName(m_config.host);
        db.setPort(m_config.port);
        db.setDatabaseName(m_config.databaseName);
        db.setUserName(m_config.userName);
        db.setPassword(m_config.password);
        db.setConnectOptions(QStringLiteral("connect_timeout=%1").arg(qMax(1, m_config.connectTimeoutMs / 1000)));
        if (!db.open()) {
            if (errorMessage) {
                *errorMessage = sqlErrorText(db.lastError());
            }
        } else {
            m_databasePath = QStringLiteral("%1:%2/%3").arg(m_config.host).arg(m_config.port).arg(m_config.databaseName);
            if (initializeSchema(db, errorMessage)) {
                success = true;
            }
        }
    }
    if (!success) {
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }
    return true;
}

bool DatabaseManager::applySQLitePragmas(QSqlDatabase &db, QString *errorMessage)
{
    if (m_config.busyTimeoutMs > 0 &&
        !execute(db, QStringLiteral("PRAGMA busy_timeout = %1").arg(m_config.busyTimeoutMs), errorMessage)) {
        return false;
    }
    if (m_config.foreignKeys && !execute(db, QStringLiteral("PRAGMA foreign_keys = ON"), errorMessage)) {
        return false;
    }
    if (m_config.useWAL && !execute(db, QStringLiteral("PRAGMA journal_mode = WAL"), errorMessage)) {
        return false;
    }
    if (m_config.synchronousNormal && !execute(db, QStringLiteral("PRAGMA synchronous = NORMAL"), errorMessage)) {
        return false;
    }
    if (m_config.cacheSizePages > 0 &&
        !execute(db, QStringLiteral("PRAGMA cache_size = -%1").arg(m_config.cacheSizePages), errorMessage)) {
        return false;
    }
    return true;
}

bool DatabaseManager::initializeSchema(QSqlDatabase &db, QString *errorMessage)
{
    const bool postgresDialect = (m_config.backend == QStringLiteral("postgresql"));
    const QStringList statements = postgresDialect
        ? QStringList{
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL
)
)SQL"),
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS app_metadata (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL
)
)SQL"),
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS maps (
    id BIGSERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    schema_version INTEGER NOT NULL,
    document_json JSONB NOT NULL,
    created_at TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL
)
)SQL"),
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS row_work_plans (
    id BIGSERIAL PRIMARY KEY,
    plan_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    name TEXT,
    plan_json JSONB NOT NULL,
    created_at TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL,
    UNIQUE(plan_id, version)
)
)SQL"),
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS row_mission_plans (
    id BIGSERIAL PRIMARY KEY,
    mission_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    name TEXT,
    plan_json JSONB NOT NULL,
    created_at TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL,
    UNIQUE(mission_id, version)
)
)SQL"),
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS operation_events (
    id BIGSERIAL PRIMARY KEY,
    occurred_at TIMESTAMPTZ NOT NULL,
    level TEXT NOT NULL,
    category TEXT NOT NULL,
    action TEXT NOT NULL,
    result TEXT NOT NULL,
    actor TEXT,
    target TEXT,
    details_json JSONB
)
)SQL"),
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS telemetry_samples (
    id BIGSERIAL PRIMARY KEY,
    sampled_at TIMESTAMPTZ NOT NULL,
    source TEXT NOT NULL,
    address TEXT,
    payload_json JSONB NOT NULL
)
)SQL"),
              QStringLiteral(R"SQL(
CREATE INDEX IF NOT EXISTS idx_telemetry_samples_time_source
ON telemetry_samples(sampled_at, source)
)SQL"),
              QStringLiteral(R"SQL(
CREATE INDEX IF NOT EXISTS idx_operation_events_time_action
ON operation_events(occurred_at, action)
)SQL")
          }
        : QStringList{
              QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
)
)SQL"),
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS app_metadata (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT NOT NULL
)
)SQL"),
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS maps (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    schema_version INTEGER NOT NULL,
    document_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
)
)SQL"),
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS row_work_plans (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    plan_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    name TEXT,
    plan_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(plan_id, version)
)
)SQL"),
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS row_mission_plans (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    mission_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    name TEXT,
    plan_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(mission_id, version)
)
)SQL"),
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS operation_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    occurred_at TEXT NOT NULL,
    level TEXT NOT NULL,
    category TEXT NOT NULL,
    action TEXT NOT NULL,
    result TEXT NOT NULL,
    actor TEXT,
    target TEXT,
    details_json TEXT
)
)SQL"),
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS telemetry_samples (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sampled_at TEXT NOT NULL,
    source TEXT NOT NULL,
    address TEXT,
    payload_json TEXT NOT NULL
)
)SQL"),
        QStringLiteral(R"SQL(
CREATE INDEX IF NOT EXISTS idx_telemetry_samples_time_source
ON telemetry_samples(sampled_at, source)
)SQL"),
        QStringLiteral(R"SQL(
CREATE INDEX IF NOT EXISTS idx_operation_events_time_action
ON operation_events(occurred_at, action)
)SQL")
          };

    if (!execute(db, QStringLiteral("BEGIN"), errorMessage)) {
        return false;
    }

    for (const QString &statement : statements) {
        if (!execute(db, statement, errorMessage)) {
            QString rollbackError;
            execute(db, QStringLiteral("ROLLBACK"), &rollbackError);
            return false;
        }
    }

    QSqlQuery migration(db);
    if (postgresDialect) {
        migration.prepare(QStringLiteral(
            "INSERT INTO schema_migrations(version, applied_at) VALUES(:version, :applied_at) "
            "ON CONFLICT (version) DO NOTHING"));
    } else {
        migration.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(:version, :applied_at)"));
    }
    migration.bindValue(QStringLiteral(":version"), kSchemaVersion);
    migration.bindValue(QStringLiteral(":applied_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!migration.exec()) {
        if (errorMessage) {
            *errorMessage = sqlErrorText(migration.lastError());
        }
        QString rollbackError;
        execute(db, QStringLiteral("ROLLBACK"), &rollbackError);
        return false;
    }

    return execute(db, QStringLiteral("COMMIT"), errorMessage);
}

bool DatabaseManager::execute(QSqlDatabase &db, const QString &sql, QString *errorMessage) const
{
    QSqlQuery query(db);
    if (query.exec(sql)) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = sqlErrorText(query.lastError());
    }
    qCWarning(lcDatabaseManager) << "SQL failed:" << sql << query.lastError();
    return false;
}

QString DatabaseManager::defaultSQLitePath() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QDir dir(appDir.isEmpty() ? QDir::currentPath() : appDir);
    return dir.filePath(QStringLiteral("data/tenco.sqlite3"));
}
