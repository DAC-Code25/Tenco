#pragma once

namespace VehicleParameters {
// Nominal driving requests; zero and braking commands remain valid.
constexpr double minOperatingSpeedMps = 0.10;
constexpr double maxOperatingSpeedMps = 1.50;
constexpr double defaultTrackingSpeedMps = 0.10;
constexpr double defaultRowMaxSpeedMps = 0.20;
}
