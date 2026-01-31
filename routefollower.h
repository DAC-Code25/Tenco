#ifndef ROUTEFOLLOWER_H
#define ROUTEFOLLOWER_H

#include <QObject>

#include <QList>
#include <QPointF>
#include <QQueue>
#include <QTimer>

// Computes cmd_vel commands to follow a polyline segment with heading constraints.
// This module is UI-agnostic and emits velocityCommand() for the caller to send.
class RouteFollower : public QObject
{
    Q_OBJECT

public:
    struct ControlParams {
        double maxLinearSpeed = 0.5;
        double maxAngularSpeed = 0.5;
        double arrivalDistanceThreshold = 0.2;
        double arrivalAngleThresholdRad = 0.0;
        double linearGain = 0.8;
        double angularGain = 1.0;
        double headingStopThresholdRad = 1.5707963267948966;     // pi/2
        double headingSlowdownThresholdRad = 0.7853981633974483; // pi/4
        double headingSlowdownFactor = 0.3;
        double nearTargetDistanceMultiplier = 3.0;
        double nearTargetSpeedMultiplier = 0.4;
        double linearAccelerationLimit = 0.6;
        double linearDecelerationLimit = 0.8;
        double angularAccelerationLimit = 1.2;
        double angularDecelerationLimit = 1.5;
        double finalAdjustLinearSpeed = 0.2;
        double finalAdjustAngularSpeed = 0.6;
    };

    explicit RouteFollower(QObject *parent = nullptr);

    void setControlParams(const ControlParams &params);
    ControlParams controlParams() const { return m_params; }

    void setUpdateIntervalMs(int intervalMs);
    int updateIntervalMs() const { return m_timer.interval(); }

    void updatePose(double x, double y, double theta);
    void setPoseValid(bool valid);
    bool poseValid() const { return m_poseValid; }

    void enqueueSegment(const QList<QPointF> &polyline, double startTheta, double endTheta);
    void cancel();
    bool isActive() const { return m_active; }

signals:
    void velocityCommand(double linear, double angular);
    void segmentCompleted(bool success);

private slots:
    void onTick();

private:
    struct SegmentExecution {
        QList<QPointF> polyline;
        double startTheta = 0.0;
        double endTheta = 0.0;
        int targetIndex = 0;
    };

    void tryStart();
    void startNextSegment();
    void finalizeSegment(bool success);

    void sendLimitedVelocity(double linear, double angular);
    double applySlewRate(double target, double lastValue, double accelLimit, double decelLimit, double dt) const;
    void resetCommandState();
    double normalizeAngle(double angle) const;

    QTimer m_timer;
    double m_dtSeconds = 0.1;

    ControlParams m_params;

    bool m_poseValid = false;
    double m_poseX = 0.0;
    double m_poseY = 0.0;
    double m_poseTheta = 0.0;

    QQueue<SegmentExecution> m_queue;
    SegmentExecution m_activeSegment;
    bool m_active = false;

    double m_lastLinear = 0.0;
    double m_lastAngular = 0.0;
};

#endif // ROUTEFOLLOWER_H
