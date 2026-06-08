#ifndef HOMECONTROLCOORDINATOR_H
#define HOMECONTROLCOORDINATOR_H

#include "configmanager.h"
#include "motioncommandarbiter.h"
#include "routefollower.h"

class HomeControlCoordinator
{
public:
    static RouteFollower::ControlParams routeFollowerParamsFromConfig(const ConfigManager::ControlConfig &config);
    static int boundedManualHeartbeatMs(const ConfigManager::ControlConfig &config);
    static MotionCommandArbiter::ManualCommandConfig manualCommandConfig(double linearSpeed,
                                                                         double angularSpeed,
                                                                         bool buttonsEnabled,
                                                                         bool keysEnabled);
};

#endif // HOMECONTROLCOORDINATOR_H
