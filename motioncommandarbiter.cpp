#include "motioncommandarbiter.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

MotionCommandArbiter::MotionCommandArbiter(QObject *parent)
    : QObject(parent)
{
    m_manualHeartbeat.setTimerType(Qt::PreciseTimer);
    m_manualHeartbeat.setSingleShot(false);
    m_manualHeartbeat.setInterval(m_heartbeatIntervalMs);
    connect(&m_manualHeartbeat, &QTimer::timeout, this, &MotionCommandArbiter::sendManualHeartbeat);
}

void MotionCommandArbiter::setManualCommandConfig(const ManualCommandConfig &config)
{
    const bool changed =
        !qFuzzyCompare(m_manualConfig.linearSpeed + 1.0, config.linearSpeed + 1.0) ||
        !qFuzzyCompare(m_manualConfig.angularSpeed + 1.0, config.angularSpeed + 1.0) ||
        m_manualConfig.buttonsEnabled != config.buttonsEnabled ||
        m_manualConfig.keysEnabled != config.keysEnabled;
    if (!changed) {
        return;
    }

    m_manualConfig = config;
    evaluateManualCommand(false);
}

void MotionCommandArbiter::setHeartbeatIntervalMs(int intervalMs)
{
    const int boundedInterval = qBound(20, intervalMs, 1000);
    if (m_heartbeatIntervalMs == boundedInterval) {
        return;
    }

    m_heartbeatIntervalMs = boundedInterval;
    m_manualHeartbeat.setInterval(m_heartbeatIntervalMs);
    if (m_manualHeartbeat.isActive()) {
        m_manualHeartbeat.start(m_heartbeatIntervalMs);
    }
}

void MotionCommandArbiter::setChassisConnected(bool connected)
{
    if (m_chassisConnected == connected) {
        return;
    }

    if (!connected) {
        const bool hadMotionContext =
            m_routeActive ||
            hasAnyHeldInput() ||
            m_manualHeartbeat.isActive() ||
            (m_hasLastCommand && isNonZero(m_lastCommand));
        m_chassisConnected = false;
        m_manualHeartbeat.stop();
        clearHeldInputs();
        m_routeActive = false;
        m_routeLinear = 0.0;
        m_routeAngular = 0.0;
        rememberZeroCommand();
        if (hadMotionContext) {
            emit safetyStopRequested(QStringLiteral("chassis_disconnected"));
        }
        return;
    }

    m_chassisConnected = true;
}

void MotionCommandArbiter::setManualInputActive(ManualInput input, bool active, bool fromKeyboard)
{
    const int index = inputIndex(input);
    if (index < 0) {
        return;
    }

    auto &inputs = fromKeyboard ? m_keyInputs : m_buttonInputs;
    if (active && !m_chassisConnected) {
        return;
    }

    if (inputs[static_cast<size_t>(index)] == active) {
        return;
    }

    inputs[static_cast<size_t>(index)] = active;
    evaluateManualCommand(true);
}

void MotionCommandArbiter::setRouteCommand(double linear, double angular)
{
    if (!m_chassisConnected) {
        return;
    }

    m_routeActive = true;
    m_routeLinear = linear;
    m_routeAngular = angular;
    if (m_manualHeartbeat.isActive()) {
        m_manualHeartbeat.stop();
    }
    emitVelocity(currentRouteCommand(), true);
}

void MotionCommandArbiter::clearRouteCommand()
{
    if (!m_routeActive) {
        return;
    }

    m_routeActive = false;
    m_routeLinear = 0.0;
    m_routeAngular = 0.0;
    evaluateManualCommand(true);
}

void MotionCommandArbiter::emergencyStop(const QString &reason)
{
    stopAll(normalizedReason(reason, QStringLiteral("emergency_stop")));
}

void MotionCommandArbiter::stopAll(const QString &reason)
{
    const bool hadMotionContext =
        m_routeActive ||
        hasAnyHeldInput() ||
        m_manualHeartbeat.isActive() ||
        (m_hasLastCommand && isNonZero(m_lastCommand));

    m_manualHeartbeat.stop();
    clearHeldInputs();
    m_routeActive = false;
    m_routeLinear = 0.0;
    m_routeAngular = 0.0;

    if (hadMotionContext) {
        emit safetyStopRequested(normalizedReason(reason, QStringLiteral("stop_all")));
    }

    const Command stopCommand;
    if (m_chassisConnected) {
        emitVelocity(stopCommand, true);
    } else {
        rememberZeroCommand();
    }
}

void MotionCommandArbiter::sendManualHeartbeat()
{
    if (m_routeActive) {
        m_manualHeartbeat.stop();
        return;
    }

    const Command manual = currentManualCommand();
    if (!isNonZero(manual)) {
        m_manualHeartbeat.stop();
        return;
    }

    emitVelocity(manual, true);
}

int MotionCommandArbiter::inputIndex(ManualInput input)
{
    const int index = static_cast<int>(input);
    if (index < 0 || index >= 4) {
        return -1;
    }
    return index;
}

bool MotionCommandArbiter::commandsEqual(const Command &lhs, const Command &rhs)
{
    return qFuzzyCompare(lhs.linear + 1.0, rhs.linear + 1.0) &&
           qFuzzyCompare(lhs.angular + 1.0, rhs.angular + 1.0);
}

bool MotionCommandArbiter::isNonZero(const Command &command)
{
    return !qFuzzyIsNull(command.linear) || !qFuzzyIsNull(command.angular);
}

bool MotionCommandArbiter::hasEffectiveManualInput(ManualInput input) const
{
    const int index = inputIndex(input);
    if (index < 0) {
        return false;
    }
    const size_t arrayIndex = static_cast<size_t>(index);
    return (m_buttonInputs[arrayIndex] && m_manualConfig.buttonsEnabled) ||
           (m_keyInputs[arrayIndex] && m_manualConfig.keysEnabled);
}

bool MotionCommandArbiter::hasAnyHeldInput() const
{
    return std::any_of(m_buttonInputs.cbegin(), m_buttonInputs.cend(), [](bool held) { return held; }) ||
           std::any_of(m_keyInputs.cbegin(), m_keyInputs.cend(), [](bool held) { return held; });
}

MotionCommandArbiter::Command MotionCommandArbiter::currentManualCommand() const
{
    Command command;

    const bool forward = hasEffectiveManualInput(ManualInput::Forward);
    const bool backward = hasEffectiveManualInput(ManualInput::Backward);
    if (forward && !backward) {
        command.linear = m_manualConfig.linearSpeed;
    } else if (backward && !forward) {
        command.linear = -m_manualConfig.linearSpeed;
    }

    const bool turnLeft = hasEffectiveManualInput(ManualInput::TurnLeft);
    const bool turnRight = hasEffectiveManualInput(ManualInput::TurnRight);
    if (turnLeft && !turnRight) {
        command.angular = m_manualConfig.angularSpeed;
    } else if (turnRight && !turnLeft) {
        command.angular = -m_manualConfig.angularSpeed;
    }

    return command;
}

MotionCommandArbiter::Command MotionCommandArbiter::currentRouteCommand() const
{
    Command command;
    command.linear = m_routeLinear;
    command.angular = m_routeAngular;
    return command;
}

void MotionCommandArbiter::clearHeldInputs()
{
    m_buttonInputs.fill(false);
    m_keyInputs.fill(false);
}

void MotionCommandArbiter::evaluateManualCommand(bool forceEmit)
{
    if (!m_chassisConnected) {
        m_manualHeartbeat.stop();
        return;
    }
    if (m_routeActive) {
        m_manualHeartbeat.stop();
        return;
    }

    const Command manual = currentManualCommand();
    if (isNonZero(manual)) {
        if (!m_manualHeartbeat.isActive()) {
            m_manualHeartbeat.start(m_heartbeatIntervalMs);
        }
        emitVelocity(manual, forceEmit);
        return;
    }

    m_manualHeartbeat.stop();
    emitVelocity(Command{}, forceEmit);
}

void MotionCommandArbiter::emitVelocity(const Command &command, bool forceEmit)
{
    if (!m_chassisConnected) {
        if (!isNonZero(command)) {
            rememberZeroCommand();
        }
        return;
    }
    if (!forceEmit && m_hasLastCommand && commandsEqual(m_lastCommand, command)) {
        return;
    }

    m_lastCommand = command;
    m_hasLastCommand = true;
    emit velocityCommand(command.linear, command.angular);
}

void MotionCommandArbiter::rememberZeroCommand()
{
    m_lastCommand = Command{};
    m_hasLastCommand = true;
}

QString MotionCommandArbiter::normalizedReason(const QString &reason, const QString &fallback) const
{
    const QString trimmed = reason.trimmed();
    return trimmed.isEmpty() ? fallback : trimmed;
}
