#include "routefollower.h"

#include <QLineF>

#include <QtMath>

#include <algorithm>
#include <cmath>

RouteFollower::RouteFollower(QObject *parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &RouteFollower::onTick);
    m_dtSeconds = static_cast<double>(m_timer.interval()) / 1000.0;
}

void RouteFollower::setControlParams(const ControlParams &params)
{
    m_params = params;
}

void RouteFollower::setUpdateIntervalMs(int intervalMs)
{
    const int ms = std::max(20, intervalMs);
    m_timer.setInterval(ms);
    m_dtSeconds = static_cast<double>(ms) / 1000.0;
}

void RouteFollower::updatePose(double x, double y, double theta)
{
    m_poseX = x;
    m_poseY = y;
    m_poseTheta = normalizeAngle(theta);
    m_poseValid = true;
}

void RouteFollower::setPoseValid(bool valid)
{
    m_poseValid = valid;
}

void RouteFollower::enqueueSegment(const QList<QPointF> &polyline, double startTheta, double endTheta)
{
    SegmentExecution seg;
    seg.polyline = polyline;
    if (seg.polyline.isEmpty()) {
        seg.polyline.append(QPointF(m_poseX, m_poseY));
    }
    seg.startTheta = startTheta;
    seg.endTheta = endTheta;
    seg.targetIndex = 0;
    m_queue.enqueue(seg);

    tryStart();
}

void RouteFollower::cancel()
{
    m_queue.clear();
    m_active = false;
    if (m_timer.isActive()) {
        m_timer.stop();
    }
    resetCommandState();
    emit velocityCommand(0.0, 0.0);
}

void RouteFollower::tryStart()
{
    if (m_active) {
        return;
    }
    if (m_queue.isEmpty()) {
        if (m_timer.isActive()) {
            m_timer.stop();
        }
        return;
    }

    startNextSegment();
    if (!m_timer.isActive()) {
        m_timer.start();
    }
    onTick();
}

void RouteFollower::startNextSegment()
{
    if (m_queue.isEmpty()) {
        m_active = false;
        if (m_timer.isActive()) {
            m_timer.stop();
        }
        resetCommandState();
        emit velocityCommand(0.0, 0.0);
        return;
    }

    m_activeSegment = m_queue.dequeue();
    if (m_activeSegment.polyline.isEmpty()) {
        m_activeSegment.polyline.append(QPointF(m_poseX, m_poseY));
    }
    m_activeSegment.targetIndex = 0;
    resetCommandState();
    m_active = true;
}

void RouteFollower::finalizeSegment(bool success)
{
    m_active = false;
    resetCommandState();
    emit velocityCommand(0.0, 0.0);
    emit segmentCompleted(success);

    // If a new segment was enqueued while emitting segmentCompleted, continue seamlessly.
    tryStart();
}

void RouteFollower::onTick()
{
    if (!m_active) {
        tryStart();
        return;
    }

    if (!m_poseValid) {
        sendLimitedVelocity(0.0, 0.0);
        return;
    }

    SegmentExecution &seg = m_activeSegment;
    if (seg.polyline.isEmpty()) {
        finalizeSegment(true);
        return;
    }
    if (seg.targetIndex >= seg.polyline.size()) {
        finalizeSegment(true);
        return;
    }

    // Ensure heading matches the desired start orientation before moving.
    if (seg.targetIndex == 0) {
        const double startHeadingError = normalizeAngle(seg.startTheta - m_poseTheta);
        if (std::abs(startHeadingError) > m_params.arrivalAngleThresholdRad) {
            const double angularTarget =
                std::clamp(m_params.angularGain * startHeadingError, -m_params.maxAngularSpeed, m_params.maxAngularSpeed);
            const double angularCmd =
                std::clamp(angularTarget, -m_params.finalAdjustAngularSpeed, m_params.finalAdjustAngularSpeed);
            sendLimitedVelocity(0.0, angularCmd);
            return;
        }
        if (seg.polyline.size() > 1) {
            seg.targetIndex = 1;
        }
    }

    const QPointF current(m_poseX, m_poseY);
    const QPointF target = seg.polyline.at(seg.targetIndex);
    const double distance = QLineF(current, target).length();

    if (seg.targetIndex == seg.polyline.size() - 1) {
        if (distance <= m_params.arrivalDistanceThreshold) {
            const double headingError = normalizeAngle(seg.endTheta - m_poseTheta);
            if (std::abs(headingError) <= m_params.arrivalAngleThresholdRad) {
                finalizeSegment(true);
                return;
            }
            const double angularTarget =
                std::clamp(m_params.angularGain * headingError, -m_params.maxAngularSpeed, m_params.maxAngularSpeed);
            const double angularCmd =
                std::clamp(angularTarget, -m_params.finalAdjustAngularSpeed, m_params.finalAdjustAngularSpeed);
            sendLimitedVelocity(0.0, angularCmd);
            return;
        }
    } else if (distance <= m_params.arrivalDistanceThreshold) {
        ++seg.targetIndex;
        return;
    }

    const double angleToTarget = std::atan2(target.y() - current.y(), target.x() - current.x());
    const double headingError = normalizeAngle(angleToTarget - m_poseTheta);

    double linear = m_params.linearGain * distance;
    linear = std::min(linear, m_params.maxLinearSpeed);

    if (std::abs(headingError) > m_params.headingStopThresholdRad) {
        linear = 0.0;
    } else if (std::abs(headingError) > m_params.headingSlowdownThresholdRad) {
        linear *= m_params.headingSlowdownFactor;
    }

    if (distance < m_params.arrivalDistanceThreshold * m_params.nearTargetDistanceMultiplier) {
        linear = std::min(linear, m_params.maxLinearSpeed * m_params.nearTargetSpeedMultiplier);
        linear = std::min(linear, m_params.finalAdjustLinearSpeed);
    }

    const double angular =
        std::clamp(m_params.angularGain * headingError, -m_params.maxAngularSpeed, m_params.maxAngularSpeed);

    sendLimitedVelocity(linear, angular);
}

void RouteFollower::sendLimitedVelocity(double linear, double angular)
{
    const double limitedLinear =
        applySlewRate(linear, m_lastLinear, m_params.linearAccelerationLimit, m_params.linearDecelerationLimit, m_dtSeconds);
    const double limitedAngular = applySlewRate(angular,
                                                m_lastAngular,
                                                m_params.angularAccelerationLimit,
                                                m_params.angularDecelerationLimit,
                                                m_dtSeconds);
    m_lastLinear = limitedLinear;
    m_lastAngular = limitedAngular;
    emit velocityCommand(limitedLinear, limitedAngular);
}

double RouteFollower::applySlewRate(double target, double lastValue, double accelLimit, double decelLimit, double dt) const
{
    if (dt <= 0.0) {
        return target;
    }
    const double delta = target - lastValue;
    if (qFuzzyIsNull(delta)) {
        return target;
    }
    const double limit = (delta >= 0.0 ? accelLimit : decelLimit) * dt;
    if (limit <= 0.0) {
        return target;
    }
    if (std::abs(delta) <= limit) {
        return target;
    }
    return lastValue + std::copysign(limit, delta);
}

void RouteFollower::resetCommandState()
{
    m_lastLinear = 0.0;
    m_lastAngular = 0.0;
}

double RouteFollower::normalizeAngle(double angle) const
{
    while (angle > M_PI) {
        angle -= 2.0 * M_PI;
    }
    while (angle < -M_PI) {
        angle += 2.0 * M_PI;
    }
    return angle;
}

