#ifndef STATUSPROTOCOL_H
#define STATUSPROTOCOL_H

#include <QJsonArray>
#include <QString>

namespace StatusProtocol
{
enum class FieldId {
    BatteryPercent,
    BatteryVoltage,
    WorkMode,
    VehiclePose,
    MapName,
    Velocity,
    BatteryTemperature,
    ChargeDischargeState,
    RunTime,
    Unknown
};

namespace Address
{
inline constexpr const char *kBatteryPercent = "3f";
inline constexpr const char *kBatteryVoltage = "38";
inline constexpr const char *kWorkMode = "3c";
inline constexpr const char *kVehiclePose = "100";
inline constexpr const char *kMapName = "320";
inline constexpr const char *kVelocity = "20";
inline constexpr const char *kBatteryTemperature = "13";
inline constexpr const char *kChargeDischargeState = "14";
inline constexpr const char *kRunTime = "15";
} // namespace Address

QJsonArray defaultReadRequests();
FieldId fieldIdFromAddress(const QString &address);
} // namespace StatusProtocol

#endif // STATUSPROTOCOL_H
