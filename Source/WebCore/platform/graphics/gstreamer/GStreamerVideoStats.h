/*
 * Copyright (C) 2026 Glass contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 */

#pragma once

#if ENABLE(VIDEO) && USE(GSTREAMER) && OS(WINDOWS)

#include "DisplayRefreshMonitorWin.h"
#include <atomic>
#include <cstdlib>
#include <gst/gst.h>
#include <mutex>
#include <wtf/MonotonicTime.h>

// Opt-in video performance counters for the experimental Windows port. Set WEBKIT_GST_VIDEO_STATS=1 in the
// environment of the browser; then GST_DEBUG=webkitvideostats:4 (and GST_DEBUG_FILE) shows one summary line per
// second for each web process playing video. Hot paths only add to atomics; nothing is logged per frame.
namespace WebCore::GStreamerVideoStats {

inline bool enabled()
{
    static const bool isEnabled = [] {
        auto* value = std::getenv("WEBKIT_GST_VIDEO_STATS");
        return value && *value && *value != '0';
    }();
    return isEnabled;
}

inline uint64_t nowMicroseconds() { return static_cast<uint64_t>(MonotonicTime::now().secondsSinceEpoch().microseconds()); }

struct Duration {
    std::atomic<uint64_t> totalUs { 0 };
    std::atomic<uint64_t> maxUs { 0 };
    std::atomic<uint64_t> count { 0 };

    void add(uint64_t us)
    {
        totalUs += us;
        ++count;
        auto previous = maxUs.load();
        while (us > previous && !maxUs.compare_exchange_weak(previous, us)) { }
    }

    // Average and maximum in milliseconds since the last call, then starts over.
    void take(double& averageMs, double& maxMs, uint64_t& samples)
    {
        samples = count.exchange(0);
        auto total = totalUs.exchange(0);
        maxMs = maxUs.exchange(0) / 1000.0;
        averageMs = samples ? total / 1000.0 / samples : 0;
    }
};

struct Counters {
    std::atomic<uint64_t> samplesArrived { 0 };   // frames handed to the sink's repaint signal
    std::atomic<uint64_t> repaintsFired { 0 };    // main-thread repaint() calls (after timer coalescing)
    std::atomic<uint64_t> paints { 0 };           // paint() calls that drew a frame
    std::atomic<uint64_t> newFramesPainted { 0 }; // ...of which showed a frame not painted before
    std::atomic<uint64_t> gpuFramesSent { 0 };    // frames handed to the compositor as GPU textures (no copy to memory)
    std::atomic<uint64_t> gpuFrameFallbacks { 0 }; // frames on the GPU that went over through memory instead
    std::atomic<uint64_t> lastArrivalUs { 0 };
    std::atomic<uint64_t> lastRepaintUs { 0 };
    Duration arrivalToRepaint;  // streaming thread -> main thread (timers, GLib pump)
    Duration repaintToPaint;    // invalidation -> paint (rendering update, compositor back-pressure)
    Duration paint;             // whole paint() of a frame
    Duration copyImage;         // VideoFrame::copyNativeImage(), conversion included
    Duration convert;           // frame format conversion only
    Duration tickToPaint;       // display tick reaching the main thread -> a new frame painted
    // Media Source video track (WebKitMediaSrc): frames handed to the decoder, times its queue was found empty,
    // how long it then waited for the next frame (that frame comes through the main thread), and how far the
    // newest frame handed on was ahead of each frame reaching the sink.
    std::atomic<uint64_t> msePushed { 0 };
    std::atomic<uint64_t> mseStarved { 0 };
    Duration mseStarveWait;
    std::atomic<uint64_t> mseLastPushedPts { GST_CLOCK_TIME_NONE };
    Duration mseAhead;
    std::atomic<uint64_t> mseAheadUnder50ms { 0 };
    // The page's view: the media element's buffered end minus its current time (sampled on the main thread), and
    // when the video track last received appended data.
    Duration mseBufferedAhead;
    std::atomic<uint64_t> mseBufferedUnder250ms { 0 };
    std::atomic<int64_t> mseLastBufferedAheadMs { -1 };
    std::atomic<uint64_t> mseLastVideoAppendUs { 0 };
    Duration pacingError;       // |time between two new frames painted - time between their timestamps|
    std::atomic<uint64_t> hitches { 0 };          // ...of which over 50 ms: a stall or a skip that is easy to see
    uint64_t lastNewFrameUs { 0 };                // main thread only
    GstClockTime lastNewFramePts { GST_CLOCK_TIME_NONE };
    std::atomic<uint64_t> lastReportUs { 0 };
    std::atomic<uint64_t> lastSinkRendered { 0 };
    std::atomic<uint64_t> lastSinkDropped { 0 };
};

inline Counters& counters()
{
    static Counters instance;
    return instance;
}

inline GstDebugCategory* category()
{
    static GstDebugCategory* debugCategory;
    static std::once_flag once;
    std::call_once(once, [] {
        GST_DEBUG_CATEGORY_INIT(debugCategory, "webkitvideostats", 0, "WebKit video performance counters (Windows)");
    });
    return debugCategory;
}

// Measures a scope into a Duration when the counters are on.
class ScopedTimer {
public:
    explicit ScopedTimer(Duration& duration)
        : m_duration(enabled() ? &duration : nullptr)
        , m_startUs(m_duration ? nowMicroseconds() : 0)
    {
    }
    ~ScopedTimer()
    {
        if (m_duration)
            m_duration->add(nowMicroseconds() - m_startUs);
    }

private:
    Duration* m_duration;
    uint64_t m_startUs;
};

// Frame pacing, called on the main thread when a frame is painted for the first time. Smooth playback paints frames
// as far apart as their timestamps are; the difference is how unevenly they reach the screen (judder). Pairs across
// a seek, a pause or a stream switch (timestamps going back or jumping by more than a second) are not counted.
inline void noteNewFramePainted(GstClockTime pts)
{
    if (!enabled())
        return;
    auto& c = counters();
    auto now = nowMicroseconds();
    if (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(c.lastNewFramePts) && pts > c.lastNewFramePts) {
        auto ptsDeltaUs = (pts - c.lastNewFramePts) / 1000;
        auto wallDeltaUs = now - c.lastNewFrameUs;
        if (ptsDeltaUs < 1000000 && wallDeltaUs < 1000000) {
            auto error = wallDeltaUs > ptsDeltaUs ? wallDeltaUs - ptsDeltaUs : ptsDeltaUs - wallDeltaUs;
            c.pacingError.add(error);
            if (error > 50000)
                ++c.hitches;
        }
    }
    c.lastNewFramePts = pts;
    c.lastNewFrameUs = now;
}

// The sink's own totals: frames it rendered and frames it dropped for arriving too late (QoS).
inline void sinkTotals(GstElement* sink, uint64_t& rendered, uint64_t& dropped)
{
    rendered = dropped = 0;
    GstElement* element = sink;
    GstElement* found = nullptr;
    while (element && GST_IS_BIN(element)) {
        GstIterator* iterator = gst_bin_iterate_sinks(GST_BIN_CAST(element));
        GValue value = G_VALUE_INIT;
        element = nullptr;
        if (gst_iterator_next(iterator, &value) == GST_ITERATOR_OK) {
            element = GST_ELEMENT_CAST(g_value_get_object(&value));
            if (found)
                gst_object_unref(found);
            found = GST_ELEMENT_CAST(gst_object_ref(element));
            g_value_unset(&value);
        }
        gst_iterator_free(iterator);
    }
    GstElement* baseSink = found ? found : sink;
    if (baseSink && g_object_class_find_property(G_OBJECT_GET_CLASS(baseSink), "stats")) {
        GstStructure* stats = nullptr;
        g_object_get(baseSink, "stats", &stats, nullptr);
        if (stats) {
            guint64 value = 0;
            if (gst_structure_get_uint64(stats, "rendered", &value))
                rendered = value;
            if (gst_structure_get_uint64(stats, "dropped", &value))
                dropped = value;
            gst_structure_free(stats);
        }
    }
    if (found)
        gst_object_unref(found);
}

// Logs one summary line if at least a second has passed since the last one.
inline void reportIfDue(GstElement* sink)
{
    if (!enabled())
        return;
    auto& c = counters();
    auto now = nowMicroseconds();
    auto last = c.lastReportUs.load();
    if (!last) {
        c.lastReportUs.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 1000000 || !c.lastReportUs.compare_exchange_strong(last, now))
        return;

    uint64_t rendered = 0, dropped = 0;
    if (sink)
        sinkTotals(sink, rendered, dropped);
    // The sink's totals start over when the player sets up a new sink (a new video, a quality switch).
    auto lastRendered = c.lastSinkRendered.exchange(rendered);
    auto lastDropped = c.lastSinkDropped.exchange(dropped);
    auto renderedDelta = rendered >= lastRendered ? rendered - lastRendered : rendered;
    auto droppedDelta = dropped >= lastDropped ? dropped - lastDropped : dropped;

    double arrivalAvg, arrivalMax, repaintAvg, repaintMax, paintAvg, paintMax, imageAvg, imageMax, convertAvg, convertMax;
    uint64_t n1, n2, n3, n4, n5;
    c.arrivalToRepaint.take(arrivalAvg, arrivalMax, n1);
    c.repaintToPaint.take(repaintAvg, repaintMax, n2);
    c.paint.take(paintAvg, paintMax, n3);
    c.copyImage.take(imageAvg, imageMax, n4);
    c.convert.take(convertAvg, convertMax, n5);
    double pacingAvg, pacingMax;
    uint64_t n6;
    c.pacingError.take(pacingAvg, pacingMax, n6);
    double tickToPaintAvg, tickToPaintMax;
    uint64_t n7;
    c.tickToPaint.take(tickToPaintAvg, tickToPaintMax, n7);
    char tickStats[512];
    DisplayRefreshMonitorWin::takeTickStats(tickStats, sizeof(tickStats));
    double starveAvg, starveMax, aheadAvg, aheadMax;
    uint64_t n8, n9;
    c.mseStarveWait.take(starveAvg, starveMax, n8);
    c.mseAhead.take(aheadAvg, aheadMax, n9);
    double bufferedAvg, bufferedMax;
    uint64_t n10;
    c.mseBufferedAhead.take(bufferedAvg, bufferedMax, n10);

    auto* debugCategory = category();
    GST_CAT_INFO(debugCategory,
        "window=%.0fms arrived=%" G_GUINT64_FORMAT " sinkRendered=%" G_GUINT64_FORMAT " sinkDroppedLate=%" G_GUINT64_FORMAT
        " repaints=%" G_GUINT64_FORMAT " paints=%" G_GUINT64_FORMAT " newFramesPainted=%" G_GUINT64_FORMAT " gpuFrames=%" G_GUINT64_FORMAT " gpuFallbacks=%" G_GUINT64_FORMAT
        " arrival->repaint avg=%.1f max=%.1fms repaint->paint avg=%.1f max=%.1fms"
        " paint avg=%.1f max=%.1fms image avg=%.1f max=%.1fms (n=%" G_GUINT64_FORMAT ") convert avg=%.1f max=%.1fms (n=%" G_GUINT64_FORMAT ")"
        " pacingError avg=%.1f max=%.1fms hitches=%" G_GUINT64_FORMAT " tick->paint avg=%.1f max=%.1fms %s mse pushed=%" G_GUINT64_FORMAT " starved=%" G_GUINT64_FORMAT " starveWait avg=%.1f max=%.1fms ahead avg=%.0f max=%.0fms aheadUnder50ms=%" G_GUINT64_FORMAT " buffered avg=%.0f max=%.0fms bufferedUnder250ms=%" G_GUINT64_FORMAT,
        (now - last) / 1000.0, c.samplesArrived.exchange(0), renderedDelta, droppedDelta,
        c.repaintsFired.exchange(0), c.paints.exchange(0), c.newFramesPainted.exchange(0), c.gpuFramesSent.exchange(0), c.gpuFrameFallbacks.exchange(0),
        arrivalAvg, arrivalMax, repaintAvg, repaintMax, paintAvg, paintMax, imageAvg, imageMax, n4, convertAvg, convertMax, n5,
        pacingAvg, pacingMax, c.hitches.exchange(0), tickToPaintAvg, tickToPaintMax, tickStats,
        c.msePushed.exchange(0), c.mseStarved.exchange(0), starveAvg, starveMax, aheadAvg, aheadMax, c.mseAheadUnder50ms.exchange(0), bufferedAvg, bufferedMax, c.mseBufferedUnder250ms.exchange(0));

    // Every 20 s, the display clock's event trace for the last two seconds.
    static std::atomic<uint64_t> lastTraceUs;
    auto lastTrace = lastTraceUs.load();
    if (!lastTrace)
        lastTraceUs.compare_exchange_strong(lastTrace, now);
    else if (now - lastTrace >= 20000000 && lastTraceUs.compare_exchange_strong(lastTrace, now)) {
        DisplayRefreshMonitorWin::writeTrace(2_s, [](const char* line) {
            GST_CAT_INFO(category(), "%s", line);
        });
    }
}

// Logs a line when the video's size, frame rate or playback rate changes (an adaptive-bitrate quality
// switch, a catch-up to the live edge). Called on the streaming thread only.
inline void noteSampleFormat(GstSample* sample)
{
    static GstCaps* lastCaps;
    static int lastWidth, lastHeight, lastFpsN, lastFpsD;
    static double lastRate = 1, lastAppliedRate = 1;

    auto* segment = gst_sample_get_segment(sample);
    double rate = segment ? segment->rate : 1;
    double appliedRate = segment ? segment->applied_rate : 1;
    auto* caps = gst_sample_get_caps(sample);
    if (caps == lastCaps && rate == lastRate && appliedRate == lastAppliedRate)
        return;
    lastCaps = caps;

    int width = 0, height = 0, fpsN = 0, fpsD = 1;
    if (caps && gst_caps_get_size(caps)) {
        auto* structure = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(structure, "width", &width);
        gst_structure_get_int(structure, "height", &height);
        gst_structure_get_fraction(structure, "framerate", &fpsN, &fpsD);
    }
    if (width == lastWidth && height == lastHeight && fpsN == lastFpsN && fpsD == lastFpsD && rate == lastRate && appliedRate == lastAppliedRate)
        return;
    lastWidth = width;
    lastHeight = height;
    lastFpsN = fpsN;
    lastFpsD = fpsD;
    lastRate = rate;
    lastAppliedRate = appliedRate;
    GST_CAT_INFO(category(), "format %dx%d framerate=%d/%d segment rate=%.3f appliedRate=%.3f", width, height, fpsN, fpsD, rate, appliedRate);
}

// The Media Source video track has waited for data since `starvedSinceUs`; called about once a second while it
// waits. Logs a line every 2 s once the wait passes 2 s, so a frozen picture can be told apart: no data
// appended by the page, or appended data not reaching the track.
inline void noteVideoStarved(uint64_t starvedSinceUs)
{
    static std::atomic<uint64_t> lastLogUs;
    auto now = nowMicroseconds();
    if (now - starvedSinceUs < 2000000 || now - lastLogUs.load() < 2000000)
        return;
    lastLogUs = now;
    auto& c = counters();
    auto lastAppend = c.mseLastVideoAppendUs.load();
    GST_CAT_INFO(category(), "video track starved for %.1fs; last video append %.1fs ago; last buffered ahead %lldms",
        (now - starvedSinceUs) / 1000000.0, lastAppend ? (now - lastAppend) / 1000000.0 : -1.0, static_cast<long long>(c.mseLastBufferedAheadMs.load()));
}

} // namespace WebCore::GStreamerVideoStats

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && OS(WINDOWS)
