#include "statusprotocol.h"

#include <QJsonObject>

namespace StatusProtocol
{
QJsonArray defaultReadRequests()
{
    QJsonArray requests;
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kBatteryPercent)},
                                {QStringLiteral("type"), QStringLiteral("uint8")},
                                {QStringLiteral("len"), 1}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kBatteryVoltage)},
                                {QStringLiteral("type"), QStringLiteral("float")},
                                {QStringLiteral("len"), 4}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kWorkMode)},
                                {QStringLiteral("type"), QStringLiteral("uint8")},
                                {QStringLiteral("len"), 1}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kVehiclePose)},
                                {QStringLiteral("type"), QStringLiteral("float")},
                                {QStringLiteral("len"), 12}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kMapName)},
                                {QStringLiteral("type"), QStringLiteral("string")},
                                {QStringLiteral("len"), 32}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kVelocity)},
                                {QStringLiteral("type"), QStringLiteral("float")},
                                {QStringLiteral("len"), 4}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kBatteryTemperature)},
                                {QStringLiteral("type"), QStringLiteral("uint8")},
                                {QStringLiteral("len"), 1}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kChargeDischargeState)},
                                {QStringLiteral("type"), QStringLiteral("uint8")},
                                {QStringLiteral("len"), 1}});
    requests.append(QJsonObject{{QStringLiteral("address"), QString::fromUtf8(Address::kRunTime)},
                                {QStringLiteral("type"), QStringLiteral("uint8")},
                                {QStringLiteral("len"), 3}});
    return requests;
}

FieldId fieldIdFromAddress(const QString &address)
{
    if (address == QLatin1String(Address::kBatteryPercent)) {
        return FieldId::BatteryPercent;
    }
    if (address == QLatin1String(Address::kBatteryVoltage)) {
        return FieldId::BatteryVoltage;
    }
    if (address == QLatin1String(Address::kWorkMode)) {
        return FieldId::WorkMode;
    }
    if (address == QLatin1String(Address::kVehiclePose)) {
        return FieldId::VehiclePose;
    }
    if (address == QLatin1String(Address::kMapName)) {
        return FieldId::MapName;
    }
    if (address == QLatin1String(Address::kVelocity)) {
        return FieldId::Velocity;
    }
    if (address == QLatin1String(Address::kBatteryTemperature)) {
        return FieldId::BatteryTemperature;
    }
    if (address == QLatin1String(Address::kChargeDischargeState)) {
        return FieldId::ChargeDischargeState;
    }
    if (address == QLatin1String(Address::kRunTime)) {
        return FieldId::RunTime;
    }
    return FieldId::Unknown;
}
} // namespace StatusProtocol
