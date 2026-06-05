#include "../databasemanager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <QtTest>

class DatabaseManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void sqliteInitializeCreatesSchema();
};

void DatabaseManagerTest::sqliteInitializeCreatesSchema()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    ConfigManager::DatabaseConfig config;
    config.backend = QStringLiteral("sqlite");
    config.connectionName = QStringLiteral("tenco_test_db");
    config.sqliteFilePath = tempDir.filePath(QStringLiteral("tenco.sqlite3"));
    config.useWAL = true;
    config.foreignKeys = true;
    config.synchronousNormal = true;
    config.busyTimeoutMs = 1000;
    config.cacheSizePages = 512;

    QString errorMessage;
    DatabaseManager &dbm = DatabaseManager::instance();
    QVERIFY2(dbm.initialize(config, &errorMessage), qPrintable(errorMessage));
    QVERIFY(dbm.isOpen());
    QCOMPARE(dbm.backend(), QStringLiteral("sqlite"));
    QVERIFY(QFile::exists(dbm.databasePath()));

    {
        QSqlQuery query(dbm.database());
        QVERIFY(query.exec(QStringLiteral("SELECT version FROM schema_migrations ORDER BY version DESC LIMIT 1")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
    }

    dbm.shutdown();
}

QTEST_MAIN(DatabaseManagerTest)
#include "test_databasemanager.moc"
