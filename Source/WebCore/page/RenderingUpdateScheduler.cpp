/*
 * Copyright (C) 2019-2020 Apple Inc. All rights reserved.
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
#include "RenderingUpdateScheduler.h"

#include "Chrome.h"
#include "ChromeClient.h"
#include "DisplayRefreshMonitorManager.h"
#include "Logging.h"
#include "Page.h"
#include "Timer.h"
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RenderingUpdateScheduler);

RenderingUpdateScheduler::RenderingUpdateScheduler(Page& page)
    : m_page(page)
{
    windowScreenDidChange(page.chrome().displayID());
}

RenderingUpdateScheduler::~RenderingUpdateScheduler() = default;

bool RenderingUpdateScheduler::scheduleAnimation()
{
    if (m_useTimer)
        return false;

    return DisplayRefreshMonitorManager::sharedManager().scheduleAnimation(*this);
}

void RenderingUpdateScheduler::adjustRenderingUpdateFrequency()
{
    auto renderingUpdateFramesPerSecond = protect(m_page)->preferredRenderingUpdateFramesPerSecond();
    if (renderingUpdateFramesPerSecond) {
        setPreferredFramesPerSecond(renderingUpdateFramesPerSecond.value());
        m_useTimer = false;
    } else
        m_useTimer = true;

    if (m_refreshTimer) {
        clearScheduled();
        scheduleRenderingUpdate();
    }
#if PLATFORM(WIN)
    // Still waiting on the display link, perhaps since before the rate changed (a page leaving the throttled,
    // visually idle state when its tab is shown again): make sure the link is running, or the update never comes.
    else if (DisplayRefreshMonitorClient::isScheduled() && !m_useTimer)
        DisplayRefreshMonitorManager::sharedManager().scheduleAnimation(*this);
#endif
}

void RenderingUpdateScheduler::scheduleRenderingUpdate()
{
    LOG_WITH_STREAM(EventLoop, stream << "RenderingUpdateScheduler for page " << m_page.ptr() << " scheduleTimedRenderingUpdate() - already scheduled " << isScheduled() << " page visible " << m_page->isVisible());

    if (isScheduled())
        return;

    // Optimize the case when an invisible page wants just to schedule layer flush.
    if (!m_page->isVisible()) {
        triggerRenderingUpdate();
        return;
    }

    tracePoint(ScheduleRenderingUpdate);

    if (!scheduleAnimation()) {
        LOG_WITH_STREAM(DisplayLink, stream << "RenderingUpdateScheduler::scheduleRenderingUpdate for interval " << protect(m_page)->preferredRenderingUpdateInterval() << " falling back to timer");
        auto interval = protect(m_page)->preferredRenderingUpdateInterval();
#if OS(WINDOWS)
        // Act like a display clock: fire one interval after the previous update, not one interval after
        // this request. Otherwise a request made just after an update (a new video frame, say) waits a
        // whole interval, and updates alternate between one and two intervals apart.
        auto elapsed = MonotonicTime::now() - m_lastTimerFireTime;
        interval = elapsed >= interval ? 0_s : interval - elapsed;
#endif
        startTimer(interval);
    }
    
    protect(m_page)->didScheduleRenderingUpdate();
}

bool RenderingUpdateScheduler::isScheduled() const
{
    return m_refreshTimer || DisplayRefreshMonitorClient::isScheduled();
}
    
void RenderingUpdateScheduler::startTimer(Seconds delay)
{
    LOG_WITH_STREAM(EventLoop, stream << "RenderingUpdateScheduler for page " << m_page.ptr() << " startTimer(" << delay << ")");

    ASSERT(!m_refreshTimer);
    m_refreshTimer = makeUnique<Timer>(*this, &RenderingUpdateScheduler::displayRefreshFired);
    m_refreshTimer->startOneShot(delay);
}

void RenderingUpdateScheduler::clearScheduled()
{
    m_refreshTimer = nullptr;
}

DisplayRefreshMonitorFactory* RenderingUpdateScheduler::displayRefreshMonitorFactory() const
{
    return m_page->chrome().client().displayRefreshMonitorFactory();
}

void RenderingUpdateScheduler::windowScreenDidChange(PlatformDisplayID displayID)
{
    adjustRenderingUpdateFrequency();
    DisplayRefreshMonitorManager::sharedManager().windowScreenDidChange(displayID, *this);
}

#if OS(WINDOWS)
bool setDeliveringDisplayTick(bool); // DisplayRefreshMonitorWin.cpp
#endif

void RenderingUpdateScheduler::displayRefreshFired()
{
    LOG_WITH_STREAM(EventLoop, stream << "RenderingUpdateScheduler for page " << m_page.ptr() << " displayRefreshFired()");

    tracePoint(TriggerRenderingUpdate);

    clearScheduled();
#if OS(WINDOWS)
    m_lastTimerFireTime = MonotonicTime::now();
#endif

    if (m_page->chrome().client().shouldTriggerRenderingUpdate(m_rescheduledRenderingUpdateCount)) {
#if OS(WINDOWS)
        // A paced update (the display clock or this scheduler's timer): the drawing area runs it right away, and
        // waits for the next one for anything else. Not set for an invisible page's update, which
        // scheduleRenderingUpdate() triggers synchronously from wherever it was requested (mid style update, say).
        bool wasDelivering = setDeliveringDisplayTick(true);
        triggerRenderingUpdate();
        setDeliveringDisplayTick(wasDelivering);
#else
        triggerRenderingUpdate();
#endif
        m_rescheduledRenderingUpdateCount = 0;
    } else {
        scheduleRenderingUpdate();
        ++m_rescheduledRenderingUpdateCount;
    }
}

void RenderingUpdateScheduler::triggerRenderingUpdate()
{
    m_page->chrome().client().triggerRenderingUpdate();
}

} // namespace WebCore
