/*
 * Copyright (C) 2026 Glass contributors
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 */

#pragma once

#if OS(WINDOWS)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <process.h>
#include <string>
#include <wtf/MonotonicTime.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebKit {

// Opt-in counters for one page's rendering-update pipeline on Windows (WEBKIT_GST_VIDEO_STATS=1, the same switch as
// the video counters). A page renders again only after the previous update has been drawn by the GPU process and
// composited, so the stages below add up to the time between updates. One summary line per second is appended to
// rendering-stats-<pid>.log beside GST_DEBUG_FILE (or in %TEMP%).
class RenderingUpdateStats final : public ThreadSafeRefCounted<RenderingUpdateStats> {
public:
    static bool enabled()
    {
        static const bool isEnabled = [] {
            auto* value = std::getenv("WEBKIT_GST_VIDEO_STATS");
            return value && *value && *value != '0';
        }();
        return isEnabled;
    }

    static RefPtr<RenderingUpdateStats> createIfEnabled()
    {
        return enabled() ? adoptRef(new RenderingUpdateStats) : nullptr;
    }

    // A tiled layer repainted a large area in one update: tiles are rasterized on the CPU in the GPU process and
    // uploaded, so a big repaint can hold up rendering for a long time. Logs the layer and how much it repainted.
    static void noteLargeRepaint(const char* layerName, uint64_t pixels, unsigned tiles, int layerWidth, int layerHeight)
    {
        if (!enabled())
            return;
        if (auto* file = openLog()) {
            std::fprintf(file, "@%.3fs large repaint: %.2f Mpx in %u tiles of layer '%s' (%dx%d)\n",
                (now() - processStartUs()) / 1000000.0, pixels / 1000000.0, tiles, layerName, layerWidth, layerHeight);
            std::fflush(file);
        }
    }

    static uint64_t now() { return static_cast<uint64_t>(MonotonicTime::now().secondsSinceEpoch().microseconds()); }

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
        void take(double& average, double& max)
        {
            auto n = count.exchange(0);
            auto total = totalUs.exchange(0);
            max = maxUs.exchange(0) / 1000.0;
            average = n ? total / 1000.0 / n : 0;
        }
    };

    // Timestamps of the update in flight (only one at a time: the next waits for displayDidRefresh).
    std::atomic<uint64_t> updateStartUs { 0 };
    std::atomic<uint64_t> fenceDoneUs { 0 };
    std::atomic<uint64_t> compositorSentUs { 0 };
    std::atomic<uint64_t> uiSentUs { 0 };

    std::atomic<uint64_t> updates { 0 };
    std::atomic<uint64_t> deferredTriggers { 0 };   // render requests made while an update was still in flight
    std::atomic<uint64_t> compositedUpdates { 0 };  // accelerated-compositing updates (vs. painting the whole view)
    Duration web;          // main thread: updateRendering() (style, layout, paint recording, layer flush)
    Duration page;         //   of which the page's update: rAF callbacks and other script, style, layout
    Duration flush;        //   of which finalizing: compositing layer flush and paint recording
    Duration gpuPaint;     // waiting for the GPU process to draw this update's recorded tiles (the fence)
    Duration hop;          // commit queue back to the main thread
    Duration compositor;   // GPU process compositing the layer tree (update request -> reply)
    Duration ui;           // the UI process presenting a painted update (Update -> displayDidRefresh)
    Duration cycle;        // whole update, start -> displayDidRefresh

    void reportIfDue()
    {
        auto nowUs = now();
        if (!m_lastReportUs) {
            m_lastReportUs = nowUs;
            return;
        }
        if (nowUs - m_lastReportUs < 1000000)
            return;
        double cycleAvg, cycleMax, webAvg, webMax, pageAvg, pageMax, flushAvg, flushMax, paintAvg, paintMax, hopAvg, hopMax, compAvg, compMax, uiAvg, uiMax;
        cycle.take(cycleAvg, cycleMax);
        web.take(webAvg, webMax);
        page.take(pageAvg, pageMax);
        flush.take(flushAvg, flushMax);
        gpuPaint.take(paintAvg, paintMax);
        hop.take(hopAvg, hopMax);
        compositor.take(compAvg, compMax);
        ui.take(uiAvg, uiMax);
        auto window = (nowUs - m_lastReportUs) / 1000.0;
        m_lastReportUs = nowUs;
        if (auto* file = openLog()) {
            std::fprintf(file, "+%.1fs window=%.0fms updates=%llu composited=%llu deferredTriggers=%llu cycle avg=%.1f max=%.1fms web avg=%.1f max=%.1fms (page avg=%.1f max=%.1fms flush avg=%.1f max=%.1fms) gpuPaint avg=%.1f max=%.1fms hop avg=%.1f max=%.1fms compositor avg=%.1f max=%.1fms ui avg=%.1f max=%.1fms\n",
                (nowUs - m_createdUs) / 1000000.0, window,
                static_cast<unsigned long long>(updates.exchange(0)), static_cast<unsigned long long>(compositedUpdates.exchange(0)),
                static_cast<unsigned long long>(deferredTriggers.exchange(0)),
                cycleAvg, cycleMax, webAvg, webMax, pageAvg, pageMax, flushAvg, flushMax, paintAvg, paintMax, hopAvg, hopMax, compAvg, compMax, uiAvg, uiMax);
            std::fflush(file);
        }
    }

private:
    RenderingUpdateStats() = default;

    // Times in the log count from the first page's stats, so the per-second lines and large repaints line up.
    static uint64_t processStartUs()
    {
        static const uint64_t start = now();
        return start;
    }

    static std::FILE* openLog()
    {
        static std::FILE* file = [] () -> std::FILE* {
            std::string directory;
            if (auto* debugFile = std::getenv("GST_DEBUG_FILE")) {
                directory = debugFile;
                auto slash = directory.find_last_of("\\/");
                directory = slash == std::string::npos ? std::string() : directory.substr(0, slash + 1);
            }
            if (directory.empty()) {
                auto* temp = std::getenv("TEMP");
                directory = temp ? std::string(temp) + "\\" : std::string();
            }
            // Named after the process's start as well as its id (as the video stats are): Windows reuses process ids, and
            // appending to an older process's log mixed two sessions in one file.
            std::time_t startTime = std::time(nullptr);
            std::tm local { };
            localtime_s(&local, &startTime);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
            auto path = directory + "rendering-stats-" + stamp + "-" + std::to_string(_getpid()) + ".log";
            return std::fopen(path.c_str(), "a");
        }();
        return file;
    }

    uint64_t m_createdUs { processStartUs() };
    uint64_t m_lastReportUs { 0 };
};

} // namespace WebKit

#endif // OS(WINDOWS)
