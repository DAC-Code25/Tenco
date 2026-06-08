#include "homegimbalkeystate.h"
#include "homemanualinputstate.h"

#include <QtTest>

class HomeInputStateTest : public QObject
{
    Q_OBJECT

private slots:
    void manualInputRespectsSourceEnablement();
    void gimbalKeyboardCombinesCtrlAndArrows();
};

void HomeInputStateTest::manualInputRespectsSourceEnablement()
{
    HomeManualInputState state;
    state.setHeld(HomeManualInputState::Direction::Forward, HomeManualInputState::Source::Button, true);
    state.setHeld(HomeManualInputState::Direction::TurnLeft, HomeManualInputState::Source::Keyboard, true);

    QVERIFY(state.isActive(HomeManualInputState::Direction::Forward, true, false));
    QVERIFY(!state.isActive(HomeManualInputState::Direction::Forward, false, true));
    QVERIFY(state.isActive(HomeManualInputState::Direction::TurnLeft, false, true));
    QVERIFY(!state.isActive(HomeManualInputState::Direction::TurnLeft, true, false));

    state.clearMotionInputs();
    QVERIFY(!state.isActive(HomeManualInputState::Direction::Forward, true, true));
    QVERIFY(!state.isActive(HomeManualInputState::Direction::TurnLeft, true, true));
}

void HomeInputStateTest::gimbalKeyboardCombinesCtrlAndArrows()
{
    HomeGimbalKeyState state;
    QVERIFY(state.handlePress(Qt::Key_Up, Qt::NoModifier));
    QCOMPARE(state.currentAction(), HomeGimbalKeyState::Action::HeightUp);

    QVERIFY(state.handlePress(Qt::Key_Control, Qt::ControlModifier));
    QCOMPARE(state.currentAction(), HomeGimbalKeyState::Action::PitchUp);

    QVERIFY(state.handleRelease(Qt::Key_Up));
    QVERIFY(state.handlePress(Qt::Key_Left, Qt::ControlModifier));
    QCOMPARE(state.currentAction(), HomeGimbalKeyState::Action::YawLeft);

    state.setActiveAction(HomeGimbalKeyState::Action::YawLeft);
    QVERIFY(state.hasActiveMotion());
    state.clearHeldKeys();
    QCOMPARE(state.currentAction(), HomeGimbalKeyState::Action::None);
    QVERIFY(!state.hasActiveMotion());
}

QTEST_MAIN(HomeInputStateTest)
#include "test_homeinputstate.moc"
