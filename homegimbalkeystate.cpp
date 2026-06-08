#include "homegimbalkeystate.h"

bool HomeGimbalKeyState::handlePress(int key, Qt::KeyboardModifiers modifiers)
{
    if (key == Qt::Key_Control) {
        m_ctrlHeld = true;
        return true;
    }

    if (modifiers.testFlag(Qt::ControlModifier)) {
        m_ctrlHeld = true;
    }

    switch (key) {
    case Qt::Key_Up:
        m_upHeld = true;
        return true;
    case Qt::Key_Down:
        m_downHeld = true;
        return true;
    case Qt::Key_Left:
        m_leftHeld = true;
        return true;
    case Qt::Key_Right:
        m_rightHeld = true;
        return true;
    default:
        return false;
    }
}

bool HomeGimbalKeyState::handleRelease(int key)
{
    switch (key) {
    case Qt::Key_Control:
        m_ctrlHeld = false;
        return true;
    case Qt::Key_Up:
        m_upHeld = false;
        return true;
    case Qt::Key_Down:
        m_downHeld = false;
        return true;
    case Qt::Key_Left:
        m_leftHeld = false;
        return true;
    case Qt::Key_Right:
        m_rightHeld = false;
        return true;
    default:
        return false;
    }
}

HomeGimbalKeyState::Action HomeGimbalKeyState::currentAction() const
{
    if (m_ctrlHeld && m_upHeld && !m_downHeld) {
        return Action::PitchUp;
    }
    if (m_ctrlHeld && m_downHeld && !m_upHeld) {
        return Action::PitchDown;
    }
    if (!m_ctrlHeld && m_upHeld && !m_downHeld) {
        return Action::HeightUp;
    }
    if (!m_ctrlHeld && m_downHeld && !m_upHeld) {
        return Action::HeightDown;
    }
    if (m_leftHeld && !m_rightHeld) {
        return Action::YawLeft;
    }
    if (m_rightHeld && !m_leftHeld) {
        return Action::YawRight;
    }
    return Action::None;
}

void HomeGimbalKeyState::clearHeldKeys()
{
    m_ctrlHeld = false;
    m_upHeld = false;
    m_downHeld = false;
    m_leftHeld = false;
    m_rightHeld = false;
    clearActiveAction();
}
