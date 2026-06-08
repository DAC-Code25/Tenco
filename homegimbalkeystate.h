#ifndef HOMEGIMBALKEYSTATE_H
#define HOMEGIMBALKEYSTATE_H

#include <Qt>

class HomeGimbalKeyState
{
public:
    enum class Action {
        None,
        HeightUp,
        HeightDown,
        YawLeft,
        YawRight,
        PitchUp,
        PitchDown
    };

    bool handlePress(int key, Qt::KeyboardModifiers modifiers);
    bool handleRelease(int key);

    Action currentAction() const;
    Action activeAction() const { return m_activeAction; }
    bool hasActiveMotion() const { return m_activeAction != Action::None; }
    void setActiveAction(Action action) { m_activeAction = action; }
    void clearActiveAction() { m_activeAction = Action::None; }
    void clearHeldKeys();

private:
    bool m_ctrlHeld = false;
    bool m_upHeld = false;
    bool m_downHeld = false;
    bool m_leftHeld = false;
    bool m_rightHeld = false;
    Action m_activeAction = Action::None;
};

#endif // HOMEGIMBALKEYSTATE_H
