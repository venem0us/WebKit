/*
 * Copyright (C) 2020 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "DisplayRefreshMonitorWin.h"

#include <cmath>
#include <cstdlib>
#include <mmsystem.h>
#include <windows.h>
#include <algorithm>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>
#include <wtf/RunLoop.h>
#include <wtf/SetForScope.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/Threading.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace WebCore {

constexpr FramesPerSecond DefaultFramesPerSecond = 60;

// Time of the last video frame handed to a page, in MonotonicTime seconds (0 for none).
static std::atomic<double> lastVideoFrameArrival;

// How long after a video frame arrives its tick comes: enough for the frame's repaint request to reach
// the main thread first, plus some slack for the frames' timing jitter.
constexpr Seconds videoFrameTickDelay = 4_ms;
constexpr Seconds minimumTickInterval = 4_ms;

// Diagnostics for the video stats log (see takeTickStats()).
namespace {
struct DurationStat {
    std::atomic<uint64_t> sumUs { 0 }, maxUs { 0 }, count { 0 };
    void add(Seconds duration)
    {
        auto us = static_cast<uint64_t>(std::max(0.0, duration.microseconds()));
        sumUs += us;
        ++count;
        auto max = maxUs.load();
        while (us > max && !maxUs.compare_exchange_weak(max, us)) { }
    }
    void take(double& avgMs, double& maxMs)
    {
        auto n = count.exchange(0);
        auto sum = sumUs.exchange(0);
        avgMs = n ? sum / 1000.0 / n : 0;
        maxMs = maxUs.exchange(0) / 1000.0;
    }
};
struct TickStats {
    std::atomic<uint64_t> ticks, skippedPending, delivered, unscheduled, previousNotDone, starts, stops, wakes;
    std::atomic<uint64_t> arrivals, bunchedArrivals, gappedArrivals, lateTicks;
    DurationStat tickAfterArrival, dispatch;
    std::atomic<double> lastDeliveredTick;
};
TickStats& tickStats()
{
    static TickStats stats;
    return stats;
}

// The counters and the trace are kept only while video stats are recorded (WEBKIT_GST_VIDEO_STATS, set by Glass's
// "Record video stats" setting).
bool statsEnabled()
{
    static const bool enabled = [] {
        auto* value = std::getenv("WEBKIT_GST_VIDEO_STATS");
        return value && *value && *value != '0';
    }();
    return enabled;
}
}

// A trace of the clock's last few thousand events, logged for two seconds every 20 s with the video stats.
namespace {
struct TraceEvent {
    std::atomic<double> time { 0 };
    std::atomic<char> type { 0 };
    std::atomic<double> value { 0 };
};
constexpr unsigned traceSize = 4096;
TraceEvent traceRing[traceSize];
std::atomic<uint32_t> traceIndex;
}

static void trace(char type, double value = 0)
{
    if (!statsEnabled())
        return;
    auto& event = traceRing[traceIndex++ % traceSize];
    event.time = 0;
    event.type = type;
    event.value = value;
    event.time = MonotonicTime::now().secondsSinceEpoch().seconds();
}

void DisplayRefreshMonitorWin::writeTrace(Seconds duration, void (*writeLine)(const char*))
{
    struct Entry {
        double time;
        char type;
        double value;
    };
    Vector<Entry> entries;
    auto end = MonotonicTime::now().secondsSinceEpoch().seconds();
    auto start = end - duration.seconds();
    for (auto& event : traceRing) {
        double time = event.time;
        if (time >= start && time <= end)
            entries.append({ time, event.type.load(), event.value.load() });
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.time < b.time; });
    writeLine("trace: A=frame arrived  R=repaint request (v=frame) S=clock start (v=1 if it was stopped) F=frame wake W=wake received"
        "  N=next tick chosen (v=time, kind: n=new frame f=fallback p=fallback, tick pending g=grid)  T=tick fired (v=1 dispatched)"
        "  D=tick delivered (v=1 update, 0 nothing scheduled, 2 previous not done)  P=new frame painted  U=rendering update started  Z=update queued on a timer  Q=update deferred until the GPU replies  G=GPU reply. Times in ms.");
    for (auto& entry : entries) {
        char line[128];
        switch (entry.type) {
        case 'R':
            snprintf(line, sizeof(line), "trace %9.2f R frame=%.2f", (entry.time - start) * 1000, (entry.value - start) * 1000);
            break;
        case 'n':
        case 'f':
        case 'p':
        case 'g':
            snprintf(line, sizeof(line), "trace %9.2f N %c at=%.2f", (entry.time - start) * 1000, entry.type, (entry.value - start) * 1000);
            break;
        default:
            snprintf(line, sizeof(line), "trace %9.2f %c %g", (entry.time - start) * 1000, entry.type, entry.value);
        }
        writeLine(line);
    }
}

void DisplayRefreshMonitorWin::noteNewVideoFramePainted()
{
    trace('P');
}

static bool deliveringDisplayTick; // main thread only

void DisplayRefreshMonitor::traceRenderingEvent(char type)
{
    trace(type);
}

bool DisplayRefreshMonitor::isDeliveringDisplayTick()
{
    return deliveringDisplayTick;
}
// Arrival time of the newest frame whose repaint request has reached the main thread (main thread only).
static std::atomic<double> lastRequestedVideoFrame;

// The running clocks, so a video frame's repaint request can wake them.
static Lock clocksLock;
static Vector<DisplayRefreshMonitorWin::Clock*>& clocks() WTF_REQUIRES_LOCK(clocksLock)
{
    static NeverDestroyed<Vector<DisplayRefreshMonitorWin::Clock*>> clocks;
    return clocks;
}

double DisplayRefreshMonitorWin::noteVideoFrameArrival()
{
    auto now = MonotonicTime::now().secondsSinceEpoch().seconds();
    trace('A');
    auto previous = lastVideoFrameArrival.exchange(now);
    if (!statsEnabled())
        return now;
    auto& stats = tickStats();
    ++stats.arrivals;
    if (previous) {
        auto gap = now - previous;
        if (gap < 0.008)
            ++stats.bunchedArrivals;
        else if (gap > 0.025 && gap < 0.1)
            ++stats.gappedArrivals;
    }
    return now;
}

double DisplayRefreshMonitorWin::lastDeliveredTickTime()
{
    return tickStats().lastDeliveredTick.load();
}

void DisplayRefreshMonitorWin::takeTickStats(char* buffer, size_t size)
{
    auto& s = tickStats();
    double afterArrivalAvg, afterArrivalMax, dispatchAvg, dispatchMax;
    s.tickAfterArrival.take(afterArrivalAvg, afterArrivalMax);
    s.dispatch.take(dispatchAvg, dispatchMax);
    snprintf(buffer, size, "ticks=%llu skippedPending=%llu delivered=%llu unscheduled=%llu previousNotDone=%llu clockStarts=%llu clockStops=%llu clockWakes=%llu"
        " tickAfterFrame avg=%.1f max=%.1fms lateTicks=%llu tick->main avg=%.1f max=%.1fms arrivals=%llu bunched=%llu gapped=%llu",
        static_cast<unsigned long long>(s.ticks.exchange(0)), static_cast<unsigned long long>(s.skippedPending.exchange(0)),
        static_cast<unsigned long long>(s.delivered.exchange(0)), static_cast<unsigned long long>(s.unscheduled.exchange(0)),
        static_cast<unsigned long long>(s.previousNotDone.exchange(0)), static_cast<unsigned long long>(s.starts.exchange(0)),
        static_cast<unsigned long long>(s.stops.exchange(0)), static_cast<unsigned long long>(s.wakes.exchange(0)),
        afterArrivalAvg, afterArrivalMax, static_cast<unsigned long long>(s.lateTicks.exchange(0)), dispatchAvg, dispatchMax,
        static_cast<unsigned long long>(s.arrivals.exchange(0)), static_cast<unsigned long long>(s.bunchedArrivals.exchange(0)),
        static_cast<unsigned long long>(s.gappedArrivals.exchange(0)));
}

// A 60 Hz clock on its own thread. It waits on a high-resolution waitable timer for points on a fixed
// grid, so ticks stay 16.7 ms apart whatever the process's timer resolution is. (A 16 ms RunLoop timer
// at Windows' default 15.6 ms resolution ticked 15.6 or 31.2 ms apart: about 42 rendering updates a second.)
class DisplayRefreshMonitorWin::Clock : public ThreadSafeRefCounted<Clock> {
public:
    static Ref<Clock> create(DisplayRefreshMonitorWin& monitor)
    {
        Ref clock = adoptRef(*new Clock(monitor));
        {
            Locker locker { clocksLock };
            clocks().append(clock.ptr());
        }
        Thread::create("DisplayRefreshMonitorWin"_s, [clock] {
            clock->run();
        }, ThreadType::Graphics, ThreadQOS::UserInteractive)->detach();
        return clock;
    }

    ~Clock()
    {
        CloseHandle(m_timer);
        CloseHandle(m_wakeEvent);
    }

    void start()
    {
        trace('S', m_running ? 0 : 1);
        m_running = true;
        SetEvent(m_wakeEvent);
    }

    // A video frame's repaint request reached the main thread. The request may not reach the monitor (the
    // page can already be waiting for an update it asked for itself), so wake the clock directly to give
    // the frame its own tick.
    void wakeForFrame()
    {
        if (!m_running)
            return;
        trace('F');
        SetEvent(m_wakeEvent);
    }

    void stop() { m_running = false; }

    void exit()
    {
        {
            Locker locker { clocksLock };
            clocks().removeFirst(this);
        }
        m_running = false;
        m_exit = true;
        SetEvent(m_wakeEvent);
    }

private:
    explicit Clock(DisplayRefreshMonitorWin& monitor)
        : m_monitor(static_cast<DisplayRefreshMonitor&>(monitor))
        , m_wakeEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr))
    {
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_timer) {
            // Before Windows 10 1803: an ordinary waitable timer, with the resolution raised while the thread runs.
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            m_needsTimerResolution = true;
        }
    }

    void run()
    {
        if (m_needsTimerResolution)
            timeBeginPeriod(1);
        const Seconds period = 1_s / DefaultFramesPerSecond;
        MonotonicTime anchor, lastTick;
        while (!m_exit) {
            if (!m_running) {
                WaitForSingleObject(m_wakeEvent, INFINITE);
                continue;
            }

            auto now = MonotonicTime::now();
            MonotonicTime next;
            double arrival = lastVideoFrameArrival.load();
            auto arrivalTime = MonotonicTime::fromRawSeconds(arrival);
            if (arrival && now - arrivalTime < 100_ms) {
                // While a video is playing, each new frame gets its own tick shortly after it arrives. Video
                // frames come at the same 60 Hz as the ticks but on the media clock, and a few milliseconds of
                // jitter is enough for a tick on a fixed grid to land just before a frame and find nothing to
                // update; that frame then shares the next tick with the following one and is never shown.
                // A repaint request wakes this thread when a frame arrives (it restarts a stopped clock), so
                // there is no need to predict the next frame. With no new frame, keep ticking a little slower
                // than 60 Hz for animations, leaving room for a late frame to get its own tick.
                char kind;
                if (arrival > m_lastServedArrival.load() && !m_tickPending) {
                    next = std::max({ arrivalTime + videoFrameTickDelay, lastTick + minimumTickInterval, now });
                    kind = 'n';
                } else {
                    next = std::max(lastTick + period + videoFrameTickDelay, now);
                    kind = m_tickPending ? 'p' : 'f';
                }
                trace(kind, next.secondsSinceEpoch().seconds());
                anchor = next;
            } else {
                // The next point on the grid, skipping one too close to the last tick.
                if (!anchor)
                    anchor = now;
                next = anchor + period * std::ceil((now - anchor) / period);
                if (lastTick && next - lastTick < period / 2)
                    next = next + period;
                trace('g', next.secondsSinceEpoch().seconds());
            }
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>((next - now).seconds() * 10000000));
            SetWaitableTimer(m_timer, &dueTime, 0, nullptr, nullptr, FALSE);
            HANDLE handles[] = { m_timer, m_wakeEvent };
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
                if (statsEnabled())
                    ++tickStats().wakes;
                trace('W');
                continue;
            }
            lastTick = next;
            if (!m_running) {
                trace('T', 0);
                continue;
            }

            auto& stats = tickStats();
            bool recordStats = statsEnabled();
            auto tickTime = recordStats ? MonotonicTime::now() : MonotonicTime();
            if (recordStats) {
                ++stats.ticks;
                if (double arrival = lastVideoFrameArrival.load()) {
                    auto sinceArrival = tickTime - MonotonicTime::fromRawSeconds(arrival);
                    if (sinceArrival < 100_ms) {
                        stats.tickAfterArrival.add(sinceArrival);
                        if (sinceArrival > 12_ms)
                            ++stats.lateTicks;
                    }
                }
            }

            // Skip the tick if the main thread hasn't handled the last one yet.
            if (m_tickPending.exchange(true)) {
                if (recordStats)
                    ++stats.skippedPending;
                trace('T', 0);
                continue;
            }
            trace('T', 1);
            RunLoop::mainSingleton().dispatch([clock = Ref { *this }, tickTime] {
                clock->m_tickPending = false;
                if (!clock->m_running)
                    return;
                if (statsEnabled()) {
                    auto& stats = tickStats();
                    auto now = MonotonicTime::now();
                    stats.dispatch.add(now - tickTime);
                    stats.lastDeliveredTick = now.secondsSinceEpoch().seconds();
                    ++stats.delivered;
                }
                // A frame counts as served once a tick reaches the main thread after the frame's repaint
                // request did: either the tick runs the update, or there was nothing left to update. A tick
                // that comes before the request doesn't count; the frame still gets a tick of its own.
                clock->m_lastServedArrival = lastRequestedVideoFrame.load();
                if (RefPtr monitor = clock->m_monitor.get()) {
                    SetForScope delivering(deliveringDisplayTick, true);
                    static_cast<DisplayRefreshMonitorWin&>(*monitor).displayLinkCallbackFired();
                } else
                    trace('D', 3);
            });
        }
        if (m_needsTimerResolution)
            timeEndPeriod(1);
    }

    ThreadSafeWeakPtr<DisplayRefreshMonitor> m_monitor;
    HANDLE m_timer { nullptr };
    HANDLE m_wakeEvent { nullptr };
    bool m_needsTimerResolution { false };
    std::atomic<bool> m_running { false };
    std::atomic<bool> m_exit { false };
    std::atomic<bool> m_tickPending { false };
    std::atomic<double> m_lastServedArrival { 0 };
};

void DisplayRefreshMonitorWin::noteVideoFrameRepaintRequested(double arrival)
{
    trace('R', arrival);
    if (arrival > lastRequestedVideoFrame.load())
        lastRequestedVideoFrame = arrival;
    Locker locker { clocksLock };
    for (auto* clock : clocks())
        clock->wakeForFrame();
}

RefPtr<DisplayRefreshMonitorWin> DisplayRefreshMonitorWin::create(PlatformDisplayID displayID)
{
    return adoptRef(*new DisplayRefreshMonitorWin(displayID));
}

DisplayRefreshMonitorWin::DisplayRefreshMonitorWin(PlatformDisplayID displayID)
    : DisplayRefreshMonitor(displayID)
    , m_currentUpdate({ 0, DefaultFramesPerSecond })
{
}

DisplayRefreshMonitorWin::~DisplayRefreshMonitorWin()
{
    if (m_clock)
        m_clock->exit();
}

bool DisplayRefreshMonitorWin::displayLinkCallbackFired()
{
    bool willUpdate = false;
    {
        Locker locker { lock() };
        bool recordStats = statsEnabled();
        if (!isPreviousFrameDone()) {
            if (recordStats)
                ++tickStats().previousNotDone;
        } else if (!isScheduled()) {
            if (recordStats)
                ++tickStats().unscheduled;
        } else
            willUpdate = true;
        trace('D', !isPreviousFrameDone() ? 2 : willUpdate ? 1 : 0);
    }
    displayLinkFired(m_currentUpdate);
    m_currentUpdate = m_currentUpdate.nextUpdate();
    return willUpdate;
}

bool DisplayRefreshMonitorWin::startNotificationMechanism()
{
    if (!m_clock)
        m_clock = Clock::create(*this);
    if (statsEnabled())
        ++tickStats().starts;
    m_clock->start();
    return true;
}

void DisplayRefreshMonitorWin::stopNotificationMechanism()
{
    if (statsEnabled())
        ++tickStats().stops;
    if (m_clock)
        m_clock->stop();
}

} // namespace WebCore