#include "../motioncommandarbiter.h"

#include <QSignalSpy>

#include <QtTest>

class MotionCommandArbiterTest : public QObject
{
    Q_OBJECT

private slots:
    void manualInputHeartbeatsUntilRelease();
    void manualInputsAreComposedAndCancelled();
    void routeCommandOverridesManualHeartbeat();
    void clearingRouteStopsWhenNoManualInput();
    void emergencyStopClearsInputsAndEmitsZero();
    void disconnectSuppressesFurtherMotionAndReportsSafetyStop();
};

namespace {

MotionCommandArbiter::ManualCommandConfig enabledManualConfig()
{
    MotionCommandArbiter::ManualCommandConfig config;
    config.linearSpeed = 1.2;
    config.angularSpeed = 0.7;
    config.buttonsEnabled = true;
    config.keysEnabled = true;
    return config;
}

void expectVelocity(const QList<QVariant> &args, double linear, double angular)
{
    QCOMPARE(args.size(), 2);
    QCOMPARE(args.at(0).toDouble(), linear);
    QCOMPARE(args.at(1).toDouble(), angular);
}

} // namespace

void MotionCommandArbiterTest::manualInputHeartbeatsUntilRelease()
{
    MotionCommandArbiter arbiter;
    arbiter.setChassisConnected(true);
    arbiter.setHeartbeatIntervalMs(20);
    arbiter.setManualCommandConfig(enabledManualConfig());

    QSignalSpy velocitySpy(&arbiter, &MotionCommandArbiter::velocityCommand);
    QVERIFY(velocitySpy.isValid());

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
    QVERIFY(velocitySpy.count() >= 1);
    expectVelocity(velocitySpy.at(0), 1.2, 0.0);

    QTRY_VERIFY_WITH_TIMEOUT(velocitySpy.count() >= 2, 120);

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, false, false);
    expectVelocity(velocitySpy.takeLast(), 0.0, 0.0);
}

void MotionCommandArbiterTest::manualInputsAreComposedAndCancelled()
{
    MotionCommandArbiter arbiter;
    arbiter.setChassisConnected(true);
    arbiter.setManualCommandConfig(enabledManualConfig());

    QSignalSpy velocitySpy(&arbiter, &MotionCommandArbiter::velocityCommand);
    QVERIFY(velocitySpy.isValid());

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
    expectVelocity(velocitySpy.takeLast(), 1.2, 0.0);

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::TurnLeft, true, true);
    expectVelocity(velocitySpy.takeLast(), 1.2, 0.7);

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, false, false);
    expectVelocity(velocitySpy.takeLast(), 0.0, 0.7);

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::TurnRight, true, true);
    expectVelocity(velocitySpy.takeLast(), 0.0, 0.0);

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::TurnRight, false, true);
    expectVelocity(velocitySpy.takeLast(), 0.0, 0.7);
}

void MotionCommandArbiterTest::routeCommandOverridesManualHeartbeat()
{
    MotionCommandArbiter arbiter;
    arbiter.setChassisConnected(true);
    arbiter.setHeartbeatIntervalMs(20);
    arbiter.setManualCommandConfig(enabledManualConfig());

    QSignalSpy velocitySpy(&arbiter, &MotionCommandArbiter::velocityCommand);
    QVERIFY(velocitySpy.isValid());

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
    expectVelocity(velocitySpy.takeLast(), 1.2, 0.0);

    arbiter.setRouteCommand(0.3, -0.2);
    expectVelocity(velocitySpy.takeLast(), 0.3, -0.2);

    const int countAfterRoute = velocitySpy.count();
    QTest::qWait(80);
    QCOMPARE(velocitySpy.count(), countAfterRoute);

    arbiter.clearRouteCommand();
    expectVelocity(velocitySpy.takeLast(), 1.2, 0.0);
}

void MotionCommandArbiterTest::clearingRouteStopsWhenNoManualInput()
{
    MotionCommandArbiter arbiter;
    arbiter.setChassisConnected(true);
    arbiter.setManualCommandConfig(enabledManualConfig());

    QSignalSpy velocitySpy(&arbiter, &MotionCommandArbiter::velocityCommand);
    QVERIFY(velocitySpy.isValid());

    arbiter.setRouteCommand(0.4, 0.1);
    expectVelocity(velocitySpy.takeLast(), 0.4, 0.1);

    arbiter.clearRouteCommand();
    expectVelocity(velocitySpy.takeLast(), 0.0, 0.0);
}

void MotionCommandArbiterTest::emergencyStopClearsInputsAndEmitsZero()
{
    MotionCommandArbiter arbiter;
    arbiter.setChassisConnected(true);
    arbiter.setManualCommandConfig(enabledManualConfig());

    QSignalSpy velocitySpy(&arbiter, &MotionCommandArbiter::velocityCommand);
    QSignalSpy safetySpy(&arbiter, &MotionCommandArbiter::safetyStopRequested);
    QVERIFY(velocitySpy.isValid());
    QVERIFY(safetySpy.isValid());

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
    expectVelocity(velocitySpy.takeLast(), 1.2, 0.0);

    arbiter.emergencyStop(QStringLiteral("home_stop_button"));
    expectVelocity(velocitySpy.takeLast(), 0.0, 0.0);
    QCOMPARE(safetySpy.takeLast().at(0).toString(), QStringLiteral("home_stop_button"));

    const int countAfterStop = velocitySpy.count();
    QTest::qWait(80);
    QCOMPARE(velocitySpy.count(), countAfterStop);
}

void MotionCommandArbiterTest::disconnectSuppressesFurtherMotionAndReportsSafetyStop()
{
    MotionCommandArbiter arbiter;
    arbiter.setChassisConnected(true);
    arbiter.setManualCommandConfig(enabledManualConfig());

    QSignalSpy velocitySpy(&arbiter, &MotionCommandArbiter::velocityCommand);
    QSignalSpy safetySpy(&arbiter, &MotionCommandArbiter::safetyStopRequested);
    QVERIFY(velocitySpy.isValid());
    QVERIFY(safetySpy.isValid());

    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
    expectVelocity(velocitySpy.takeLast(), 1.2, 0.0);

    arbiter.setChassisConnected(false);
    QCOMPARE(safetySpy.takeLast().at(0).toString(), QStringLiteral("chassis_disconnected"));

    const int countAfterDisconnect = velocitySpy.count();
    arbiter.setManualInputActive(MotionCommandArbiter::ManualInput::TurnLeft, true, false);
    arbiter.setRouteCommand(0.5, 0.5);
    QTest::qWait(40);
    QCOMPARE(velocitySpy.count(), countAfterDisconnect);
}

QTEST_MAIN(MotionCommandArbiterTest)
#include "test_motioncommandarbiter.moc"
