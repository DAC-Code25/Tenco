#include "homecontrolcoordinator.h"

#include <QtMath>

RouteFollower::ControlParams HomeControlCoordinator::routeFollowerParamsFromConfig(const ConfigManager::ControlConfig &config)
{
    RouteFollower::ControlParams params;
    params.maxLinearSpeed = config.maxLinearSpeed;
    params.maxAngularSpeed = config.maxAngularSpeed;
    params.arrivalDistanceThreshold = config.arrivalDistanceThreshold;
    params.arrivalAngleThresholdRad = qDegreesToRadians(config.arrivalAngleThresholdDeg);
    params.linearGain = config.linearGain;
    params.angularGain = config.angularGain;
    params.headingStopThresholdRad = qDegreesToRadians(config.headingStopThresholdDeg);
    params.headingSlowdownThresholdRad = qDegreesToRadians(config.headingSlowdownThresholdDeg);
    params.headingSlowdownFactor = config.headingSlowdownFactor;
    params.nearTargetDistanceMultiplier = config.nearTargetDistanceMultiplier;
    params.nearTargetSpeedMultiplier = config.nearTargetSpeedMultiplier;
    params.linearAccelerationLimit = config.linearAccelerationLimit;
    params.linearDecelerationLimit = config.linearDecelerationLimit;
    params.angularAccelerationLimit = config.angularAccelerationLimit;
    params.angularDecelerationLimit = config.angularDecelerationLimit;
    params.finalAdjustLinearSpeed = config.finalAdjustLinearSpeed;
    params.finalAdjustAngularSpeed = config.finalAdjustAngularSpeed;
    return params;
}

int HomeControlCoordinator::boundedManualHeartbeatMs(const ConfigManager::ControlConfig &config)
{
    return qBound(20, config.manualMotionRepeatIntervalMs, 1000);
}

MotionCommandArbiter::ManualCommandConfig HomeControlCoordinator::manualCommandConfig(double linearSpeed,
                                                                                      double angularSpeed,
                                                                                      bool buttonsEnabled,
                                                                                      bool keysEnabled)
{
    MotionCommandArbiter::ManualCommandConfig config;
    config.linearSpeed = linearSpeed;
    config.angularSpeed = angularSpeed;
    config.buttonsEnabled = buttonsEnabled;
    config.keysEnabled = keysEnabled;
    return config;
}
