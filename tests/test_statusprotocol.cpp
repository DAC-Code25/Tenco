#include <QtTest>

#include "../statusprotocol.h"

class StatusProtocolTest : public QObject
{
    Q_OBJECT

private slots:
    void buildsDefaultReadRequests();
    void mapsAddressToFieldId();
};

void StatusProtocolTest::buildsDefaultReadRequests()
{
    const QJsonArray requests = StatusProtocol::defaultReadRequests();
    QCOMPARE(requests.size(), 9);

    const QJsonObject first = requests.at(0).toObject();
    QCOMPARE(first.value(QStringLiteral("address")).toString(), QString::fromUtf8(StatusProtocol::Address::kBatteryPercent));
    QCOMPARE(first.value(QStringLiteral("type")).toString(), QStringLiteral("uint8"));
    QCOMPARE(first.value(QStringLiteral("len")).toInt(), 1);
}

void StatusProtocolTest::mapsAddressToFieldId()
{
    QCOMPARE(StatusProtocol::fieldIdFromAddress(QString::fromUtf8(StatusProtocol::Address::kVehiclePose)),
             StatusProtocol::FieldId::VehiclePose);
    QCOMPARE(StatusProtocol::fieldIdFromAddress(QString::fromUtf8(StatusProtocol::Address::kMapName)),
             StatusProtocol::FieldId::MapName);
    QCOMPARE(StatusProtocol::fieldIdFromAddress(QStringLiteral("not-exists")),
             StatusProtocol::FieldId::Unknown);
}

QTEST_MAIN(StatusProtocolTest)
#include "test_statusprotocol.moc"
