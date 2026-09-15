#ifndef MOTIONCOMMANDARBITER_H
#define MOTIONCOMMANDARBITER_H

#include <QObject>
#include <QString>
#include <QTimer>

#include <array>

class MotionCommandArbiter : public QObject
{
    Q_OBJECT

public:
    enum class ManualInput {
        Forward = 0,
        Backward,
        TurnLeft,
        TurnRight
    };

    struct ManualCommandConfig {
        double linearSpeed = 0.0;
        double angularSpeed = 0.0;
        bool buttonsEnabled = false;
        bool keysEnabled = false;
    };

    explicit MotionCommandArbiter(QObject *parent = nullptr);

    void setManualCommandConfig(const ManualCommandConfig &config);
    ManualCommandConfig manualCommandConfig() const { return m_manualConfig; }

    void setHeartbeatIntervalMs(int intervalMs);
    int heartbeatIntervalMs() const { return m_heartbeatIntervalMs; }

    void setChassisConnected(bool connected);
    bool chassisConnected() const { return m_chassisConnected; }

    void setManualInputActive(ManualInput input, bool active, bool fromKeyboard);
    void setManualPermission(bool granted);
    bool manualPermission() const { return m_manualPermission; }
    void prepareAutomatic();
    void emergencyStop(const QString &reason);
    void stopAll(const QString &reason);

signals:
    void manualTakeoverRequested();
    void velocityCommand(double linear, double angular);
    void safetyStopRequested(const QString &reason);

private slots:
    void sendManualHeartbeat();

private:
    struct Command {
        double linear = 0.0;
        double angular = 0.0;
    };

    static int inputIndex(ManualInput input);
    static bool commandsEqual(const Command &lhs, const Command &rhs);
    static bool isNonZero(const Command &command);

    bool hasEffectiveManualInput(ManualInput input) const;
    bool hasAnyHeldInput() const;
    Command currentManualCommand() const;
    void clearHeldInputs();
    void evaluateManualCommand(bool forceEmit);
    void emitVelocity(const Command &command, bool forceEmit);
    void rememberZeroCommand();
    QString normalizedReason(const QString &reason, const QString &fallback) const;

    ManualCommandConfig m_manualConfig;
    QTimer m_manualHeartbeat;
    int m_heartbeatIntervalMs = 40;
    bool m_chassisConnected = false;

    std::array<bool, 4> m_buttonInputs{};
    std::array<bool, 4> m_keyInputs{};

    bool m_manualPermission = false;

    bool m_hasLastCommand = false;
    Command m_lastCommand;
};

#endif // MOTIONCOMMANDARBITER_H
