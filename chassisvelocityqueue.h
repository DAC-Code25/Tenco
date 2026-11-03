#ifndef CHASSISVELOCITYQUEUE_H
#define CHASSISVELOCITYQUEUE_H

#include <QtGlobal>

// Bounds only this application's unsent velocity and locally buffered writes.
// Socket drain is not acknowledgement from the chassis, and the wire format
// has no command TTL. Bytes already handed to TCP cannot be recalled here.
class ChassisVelocityQueue
{
public:
    static constexpr qint64 MaxAgeMs = 150;
    static constexpr qint64 MaxBufferedBytes = 4096;
    struct Velocity { double linear = 0; double angular = 0; };
    enum class Kind { None, Send, Disconnect };
    enum class Failure { None, ClockRewound, TooManyBytes, WriteExpired, SampleExpired, StopBlocked };
    struct Action { Kind kind = Kind::None; Velocity velocity; Failure failure = Failure::None; };

    Action submit(qint64 nowMs, qint64 bufferedBytes, Velocity velocity) {
        const auto observed = observe(nowMs, bufferedBytes);
        if (observed.kind == Kind::Disconnect) return observed;
        if (velocity.linear == 0 && velocity.angular == 0 && bufferedBytes > 0)
            return fail(Failure::StopBlocked);
        m_pending = velocity;
        m_pendingAt = nowMs;
        m_hasPending = true;
        return bufferedBytes == 0 ? send(nowMs) : Action{};
    }
    Action advance(qint64 nowMs, qint64 bufferedBytes) {
        const auto observed = observe(nowMs, bufferedBytes);
        if (observed.kind == Kind::Disconnect) return observed;
        return bufferedBytes == 0 && m_hasPending ? send(nowMs) : Action{};
    }
    bool needsService() const { return m_hasPending || m_writeOutstanding; }
    void clear() { *this = ChassisVelocityQueue{}; }

private:
    Action fail(Failure failure) {
        clear();
        return {Kind::Disconnect, {}, failure};
    }
    Action observe(qint64 nowMs, qint64 bufferedBytes) {
        if (m_hasClock && nowMs < m_lastNow) return fail(Failure::ClockRewound);
        m_hasClock = true;
        m_lastNow = nowMs;
        if (bufferedBytes < 0 || bufferedBytes > MaxBufferedBytes) return fail(Failure::TooManyBytes);
        if (m_writeOutstanding && bufferedBytes > 0 && nowMs - m_writeAt >= MaxAgeMs)
            return fail(Failure::WriteExpired);
        if (m_hasPending && nowMs - m_pendingAt >= MaxAgeMs) return fail(Failure::SampleExpired);
        if (bufferedBytes == 0) m_writeOutstanding = false;
        else if (!m_writeOutstanding) { m_writeOutstanding = true; m_writeAt = nowMs; }
        return {};
    }
    Action send(qint64 nowMs) {
        const auto velocity = m_pending;
        m_hasPending = false;
        m_writeOutstanding = true;
        m_writeAt = nowMs;
        return {Kind::Send, velocity, Failure::None};
    }
    Velocity m_pending;
    qint64 m_pendingAt = 0, m_writeAt = 0, m_lastNow = 0;
    bool m_hasPending = false, m_writeOutstanding = false, m_hasClock = false;
};

#endif // CHASSISVELOCITYQUEUE_H
