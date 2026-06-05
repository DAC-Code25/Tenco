#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include "configmanager.h"

#include <QObject>
#include <QSqlDatabase>
#include <QString>

class DatabaseManager : public QObject
{
    Q_OBJECT

public:
    static DatabaseManager &instance();

    bool initialize(const ConfigManager::DatabaseConfig &config, QString *errorMessage = nullptr);
    void shutdown();

    bool isOpen() const;
    QString backend() const { return m_config.backend; }
    QString connectionName() const { return m_connectionName; }
    QString databasePath() const { return m_databasePath; }
    int schemaVersion(QString *errorMessage = nullptr) const;
    QSqlDatabase database() const;

private:
    explicit DatabaseManager(QObject *parent = nullptr);

    bool configureSQLite(QString *errorMessage);
    bool configurePostgreSQL(QString *errorMessage);
    bool applySQLitePragmas(QSqlDatabase &db, QString *errorMessage);
    bool initializeSchema(QSqlDatabase &db, QString *errorMessage);
    bool execute(QSqlDatabase &db, const QString &sql, QString *errorMessage) const;
    QString defaultSQLitePath() const;

    ConfigManager::DatabaseConfig m_config;
    QString m_connectionName;
    QString m_databasePath;
    bool m_initialized = false;
};

#endif // DATABASEMANAGER_H
