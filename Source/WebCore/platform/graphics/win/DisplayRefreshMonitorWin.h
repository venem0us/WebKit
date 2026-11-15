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

#pragma once

#include "DisplayRefreshMonitor.h"
#include <wtf/Seconds.h>

namespace WebCore {

class DisplayRefreshMonitorWin : public DisplayRefreshMonitor {
public:
    static RefPtr<DisplayRefreshMonitorWin> create(PlatformDisplayID);
    ~DisplayRefreshMonitorWin();

    // Called on any thread when a video frame is handed to the page, so ticks can land just after frames.
    // Returns the arrival time (MonotonicTime seconds) to pass to noteVideoFrameRepaintRequested().
    static double noteVideoFrameArrival();
    // Called on the main thread when the repaint request for the frame that arrived at that time is made.
    static void noteVideoFrameRepaintRequested(double arrival);

    // Diagnostics for the video stats log: the tick counters since the last call, and the time
    // (MonotonicTime seconds) the last tick reached the main thread.
    static void takeTickStats(char* buffer, size_t);
    static double lastDeliveredTickTime();
    static void noteNewVideoFramePainted();
    // Writes the clock's event trace for the last `duration`, one line at a time.
    static void writeTrace(Seconds duration, void (*writeLine)(const char*));

    class Clock;

private:
    explicit DisplayRefreshMonitorWin(PlatformDisplayID);

    bool startNotificationMechanism() final;
    void stopNotificationMechanism() final;

    bool displayLinkCallbackFired(); // Returns whether the tick ran a rendering update.

    RefPtr<Clock> m_clock;
    DisplayUpdate m_currentUpdate;
};

} // namespace WebCore