#include "homemanualinputstate.h"

void HomeManualInputState::setHeld(Direction direction, Source source, bool held)
{
    m_held[directionIndex(direction)][sourceIndex(source)] = held;
}

bool HomeManualInputState::isHeld(Direction direction, Source source) const
{
    return m_held[directionIndex(direction)][sourceIndex(source)];
}

bool HomeManualInputState::isActive(Direction direction, bool buttonsEnabled, bool keysEnabled) const
{
    return (buttonsEnabled && isHeld(direction, Source::Button)) ||
           (keysEnabled && isHeld(direction, Source::Keyboard));
}

void HomeManualInputState::clearMotionInputs()
{
    for (auto &sources : m_held) {
        sources.fill(false);
    }
}

int HomeManualInputState::directionIndex(Direction direction)
{
    return static_cast<int>(direction);
}

int HomeManualInputState::sourceIndex(Source source)
{
    return static_cast<int>(source);
}
