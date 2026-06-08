#ifndef HOMEMANUALINPUTSTATE_H
#define HOMEMANUALINPUTSTATE_H

#include <array>

class HomeManualInputState
{
public:
    enum class Direction {
        Forward = 0,
        Backward,
        TurnLeft,
        TurnRight
    };

    enum class Source {
        Button = 0,
        Keyboard
    };

    void setHeld(Direction direction, Source source, bool held);
    bool isHeld(Direction direction, Source source) const;
    bool isActive(Direction direction, bool buttonsEnabled, bool keysEnabled) const;
    void clearMotionInputs();

private:
    static int directionIndex(Direction direction);
    static int sourceIndex(Source source);

    std::array<std::array<bool, 2>, 4> m_held{};
};

#endif // HOMEMANUALINPUTSTATE_H
