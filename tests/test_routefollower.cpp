#include "routefollower.h"

#include <QSignalSpy>

#include <QtTest>

class RouteFollowerTest : public QObject
{
    Q_OBJECT

private slots:
    void completesSinglePointSegmentImmediately();
    void cancelAlwaysEmitsStopCommand();
};

void RouteFollowerTest::completesSinglePointSegmentImmediately()
{
    RouteFollower follower;
    RouteFollower::ControlParams params = follower.controlParams();
    params.arrivalDistanceThreshold = 0.2;
    params.arrivalAngleThresholdRad = 0.01;
    follower.setControlParams(params);
    follower.updatePose(0.0, 0.0, 0.0);

    QSignalSpy completedSpy(&follower, &RouteFollower::segmentCompleted);
    QSignalSpy velocitySpy(&follower, &RouteFollower::velocityCommand);

    follower.enqueueSegment({QPointF(0.0, 0.0)}, 0.0, 0.0);

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 100);
    QVERIFY(velocitySpy.count() >= 1);

    const QList<QVariant> completedArgs = completedSpy.takeFirst();
    QCOMPARE(completedArgs.at(0).toBool(), true);

    const QList<QVariant> lastVelocity = velocitySpy.takeLast();
    QCOMPARE(lastVelocity.at(0).toDouble(), 0.0);
    QCOMPARE(lastVelocity.at(1).toDouble(), 0.0);
}

void RouteFollowerTest::cancelAlwaysEmitsStopCommand()
{
    RouteFollower follower;
    follower.updatePose(0.0, 0.0, 0.0);
    follower.enqueueSegment({QPointF(5.0, 0.0)}, 0.0, 0.0);

    QSignalSpy velocitySpy(&follower, &RouteFollower::velocityCommand);
    follower.cancel();

    QTRY_VERIFY_WITH_TIMEOUT(velocitySpy.count() >= 1, 100);

    const QList<QVariant> lastVelocity = velocitySpy.takeLast();
    QCOMPARE(lastVelocity.at(0).toDouble(), 0.0);
    QCOMPARE(lastVelocity.at(1).toDouble(), 0.0);
    QCOMPARE(follower.isActive(), false);
}

QTEST_MAIN(RouteFollowerTest)
#include "test_routefollower.moc"
