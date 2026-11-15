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
#include <mutex>
#include <process.h>
#include <string>
#include <wtf/MonotonicTime.h>

#if USE(SKIA)
#include <WebCore/D3D11SharedTexture.h>
#endif

namespace WebKit {

// Opt-in counters for the GPU process's drawing on Windows (WEBKIT_GST_VIDEO_STATS=1, the same switch as the video
// and rendering-update counters). They split the web process's "gpuPaint" wait into receiving images, drawing them
// and flushing tiles. "Large" images are at least 256x256 device pixels, which is where video frames land. One summary
// line per second is appended to gpu-rendering-stats-<pid>.log beside GST_DEBUG_FILE (or in %TEMP%).
class GPURenderingStats final {
public:
    static GPURenderingStats* singletonIfEnabled()
    {
        static GPURenderingStats* stats = [] () -> GPURenderingStats* {
            auto* value = std::getenv("WEBKIT_GST_VIDEO_STATS");
            if (!value || !*value || *value == '0')
                return nullptr;
            return new GPURenderingStats;
        }();
        return stats;
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
        // Returns the count; average and max are per call, total is the sum, all in milliseconds.
        uint64_t take(double& total, double& average, double& max)
        {
            auto n = count.exchange(0);
            auto sum = totalUs.exchange(0);
            max = maxUs.exchange(0) / 1000.0;
            total = sum / 1000.0;
            average = n ? total / n : 0;
            return n;
        }
    };

    // Times the rest of the enclosing scope into `field` when stats are on.
#define GPU_RENDERING_STATS_SCOPE(field) \
    std::optional<GPURenderingStats::ScopedTimer> gpuRenderingStatsScope; \
    if (auto* gpuRenderingStats = GPURenderingStats::singletonIfEnabled()) \
        gpuRenderingStatsScope.emplace(&gpuRenderingStats->field)

    class ScopedTimer {
    public:
        explicit ScopedTimer(Duration* duration)
            : m_duration(duration)
            , m_start(duration ? now() : 0)
        {
        }
        ~ScopedTimer()
        {
            if (m_duration)
                m_duration->add(now() - m_start);
        }
    private:
        Duration* m_duration;
        uint64_t m_start;
    };

    Duration receiveImage;     // cacheNativeImage: mapping a new image sent by the web process
    Duration drawLargeImage;   // drawNativeImage with a source of at least 256x256
    Duration drawSmallImage;   // every other drawNativeImage
    Duration flushTile;        // RemoteImageBuffer flushContext / flushContextSync
    Duration videoUpload;      // WCScene: uploading a video frame handed to the compositor into its texture
    Duration composite;        // WCScene::update: applying layer changes and compositing
    Duration paint;            // WCScene::update: painting the layer tree (part of composite)
    Duration present;          // WCScene::present: swapBuffers, after the web process has been answered
    // Drawing commands that can be expensive on the CPU (tiles and canvases are rasterized in software).
    Duration filter;           // drawFilteredImageBuffer (CSS/SVG filters such as blur)
    Duration imageBuffer;      // drawImageBuffer (one buffer drawn into another: canvases into tiles)
    Duration transparencyLayer; // endTransparencyLayer (compositing an opacity/blend group)
    Duration fill;             // fillRect variants
    Duration displayList;      // drawDisplayList
    Duration createBuffer;     // creating an image buffer (tiles, canvases, filter intermediates)
    std::atomic<uint64_t> receivedBytes { 0 };
    std::atomic<uint64_t> flushes { 0 };       // update fences (RemoteRenderingBackend::flush)
    // Video frames left on the GPU (WCScene): textures opened, shown, skipped for a newer one, and what went wrong.
    std::atomic<uint64_t> textureImports { 0 }, textureImportFailures { 0 }, texturesShown { 0 }, texturesSkipped { 0 }, texturesMissing { 0 }, textureAcquireTimeouts { 0 }, texturesNotShown { 0 }, texturesReleasedWithLayer { 0 };
    std::atomic<uint64_t> resizePresents { 0 }; // second presents after ANGLE resized a window's buffers (WCScene::present)
    // Layer tiles drawn on the GPU (GLASS_GPU_RASTER) and copied from their texture, and those whose texture wasn't found.
    std::atomic<uint64_t> gpuTiles { 0 }, gpuTileFallbacks { 0 }, gpuTilesMissing { 0 };
    // Accelerated image buffers made on the GPU, and those that fell back to a bitmap (D3D11SharedTextures::lastFailure).
    std::atomic<uint64_t> gpuBuffers { 0 }, gpuBufferFailures { 0 }, gpuBuffersReused { 0 };
    // Scene diagnostics: updates naming layers the scene doesn't have, layers whose tiles were dropped (no backing store),
    // and tiles whose first update didn't cover them (the rest of the tile has nothing until it is repainted).
    std::atomic<uint64_t> unknownLayers { 0 }, droppedBackingStores { 0 }, partialNewTiles { 0 };

    // Direct3D removed the device: this process ends right after, so that a new one, on a new device, takes over.
    void noteDeviceRemoved(long reason, bool simulated)
    {
        if (auto* file = openLog()) {
            std::fprintf(file, "device removed (0x%08lx%s): ending the GPU process for a new one\n", static_cast<unsigned long>(reason), simulated ? ", simulated" : "");
            std::fflush(file);
        }
    }

    // The latest reason an accelerated buffer couldn't be made on the GPU, reported once per reason change.
    void noteGPUBufferFailure(const char* reason)
    {
        std::lock_guard lock(m_failureLock);
        if (reason == m_lastFailure)
            return;
        m_lastFailure = reason;
        if (auto* file = openLog()) {
            std::fprintf(file, "gpu buffer fallback: %s\n", reason ? reason : "?");
            std::fflush(file);
        }
    }

    void noteLargeDraw(int sourceWidth, int sourceHeight, float sourceRectWidth, float sourceRectHeight, float destWidth, float destHeight, float ctmScale, bool accelerated, int quality, float clipWidth, float clipHeight)
    {
        std::lock_guard lock(m_lastDrawLock);
        m_lastDraw = { sourceWidth, sourceHeight, sourceRectWidth, sourceRectHeight, destWidth, destHeight, ctmScale, accelerated, quality, clipWidth, clipHeight };
    }

    void reportIfDue()
    {
        auto nowUs = now();
        auto last = m_lastReportUs.load();
        if (!last) {
            m_lastReportUs.compare_exchange_strong(last, nowUs);
            return;
        }
        if (nowUs - last < 1000000 || !m_lastReportUs.compare_exchange_strong(last, nowUs))
            return;

        double receiveTotal, receiveAvg, receiveMax, largeTotal, largeAvg, largeMax, smallTotal, smallAvg, smallMax, flushTotal, flushAvg, flushMax;
        auto receives = receiveImage.take(receiveTotal, receiveAvg, receiveMax);
        auto largeDraws = drawLargeImage.take(largeTotal, largeAvg, largeMax);
        auto smallDraws = drawSmallImage.take(smallTotal, smallAvg, smallMax);
        auto tileFlushes = flushTile.take(flushTotal, flushAvg, flushMax);
        double uploadTotal, uploadAvg, uploadMax, compositeTotal, compositeAvg, compositeMax;
        auto uploads = videoUpload.take(uploadTotal, uploadAvg, uploadMax);
        auto composites = composite.take(compositeTotal, compositeAvg, compositeMax);
        double paintTotal, paintAvg, paintMax, presentTotal, presentAvg, presentMax;
        auto paints = paint.take(paintTotal, paintAvg, paintMax);
        auto presents = present.take(presentTotal, presentAvg, presentMax);
        double filterTotal, filterAvg, filterMax, imageBufferTotal, imageBufferAvg, imageBufferMax, layerTotal, layerAvg, layerMax, fillTotal, fillAvg, fillMax, listTotal, listAvg, listMax, createTotal, createAvg, createMax;
        auto filters = filter.take(filterTotal, filterAvg, filterMax);
        auto imageBuffers = imageBuffer.take(imageBufferTotal, imageBufferAvg, imageBufferMax);
        auto layers = transparencyLayer.take(layerTotal, layerAvg, layerMax);
        auto fills = fill.take(fillTotal, fillAvg, fillMax);
        auto lists = displayList.take(listTotal, listAvg, listMax);
        auto creates = createBuffer.take(createTotal, createAvg, createMax);
        LastDraw draw;
        {
            std::lock_guard lock(m_lastDrawLock);
            draw = m_lastDraw;
        }
        if (auto* file = openLog()) {
            std::fprintf(file, "+%.1fs window=%.0fms fences=%llu receive n=%llu total=%.1fms avg=%.2f max=%.1fms bytes=%.1fMB drawLarge n=%llu total=%.1fms avg=%.2f max=%.1fms drawSmall n=%llu total=%.1fms max=%.1fms flushTile n=%llu total=%.1fms avg=%.2f max=%.1fms lastLarge src=%dx%d srcRect=%.0fx%.0f dest=%.0fx%.0f ctmScale=%.2f accelerated=%d quality=%d clip=%.0fx%.0f videoUpload n=%llu avg=%.2f max=%.1fms composite n=%llu avg=%.2f max=%.1fms paint n=%llu avg=%.2f max=%.1fms present n=%llu avg=%.2f max=%.1fms filter n=%llu total=%.1f max=%.1fms imageBuffer n=%llu total=%.1f max=%.1fms layer n=%llu total=%.1f max=%.1fms fill n=%llu total=%.1f max=%.1fms displayList n=%llu total=%.1f max=%.1fms createBuffer n=%llu total=%.1f max=%.1fms textures import=%llu importFail=%llu shown=%llu skipped=%llu missing=%llu acquireTimeout=%llu notShown=%llu releasedWithLayer=%llu resizePresents=%llu gpuTiles=%llu gpuTileFallbacks=%llu gpuTilesMissing=%llu gpuBuffers=%llu gpuBufferFailures=%llu gpuBuffersReused=%llu unknownLayers=%llu droppedBackingStores=%llu partialNewTiles=%llu",
                (nowUs - m_createdUs) / 1000000.0, (nowUs - last) / 1000.0,
                static_cast<unsigned long long>(flushes.exchange(0)),
                static_cast<unsigned long long>(receives), receiveTotal, receiveAvg, receiveMax, receivedBytes.exchange(0) / 1048576.0,
                static_cast<unsigned long long>(largeDraws), largeTotal, largeAvg, largeMax,
                static_cast<unsigned long long>(smallDraws), smallTotal, smallMax,
                static_cast<unsigned long long>(tileFlushes), flushTotal, flushAvg, flushMax,
                draw.sourceWidth, draw.sourceHeight, draw.sourceRectWidth, draw.sourceRectHeight, draw.destWidth, draw.destHeight, draw.ctmScale, draw.accelerated ? 1 : 0, draw.quality, draw.clipWidth, draw.clipHeight,
                static_cast<unsigned long long>(uploads), uploadAvg, uploadMax, static_cast<unsigned long long>(composites), compositeAvg, compositeMax,
                static_cast<unsigned long long>(paints), paintAvg, paintMax, static_cast<unsigned long long>(presents), presentAvg, presentMax,
                static_cast<unsigned long long>(filters), filterTotal, filterMax, static_cast<unsigned long long>(imageBuffers), imageBufferTotal, imageBufferMax,
                static_cast<unsigned long long>(layers), layerTotal, layerMax, static_cast<unsigned long long>(fills), fillTotal, fillMax,
                static_cast<unsigned long long>(lists), listTotal, listMax, static_cast<unsigned long long>(creates), createTotal, createMax,
                static_cast<unsigned long long>(textureImports.exchange(0)), static_cast<unsigned long long>(textureImportFailures.exchange(0)), static_cast<unsigned long long>(texturesShown.exchange(0)), static_cast<unsigned long long>(texturesSkipped.exchange(0)),
                static_cast<unsigned long long>(texturesMissing.exchange(0)), static_cast<unsigned long long>(textureAcquireTimeouts.exchange(0)), static_cast<unsigned long long>(texturesNotShown.exchange(0)), static_cast<unsigned long long>(texturesReleasedWithLayer.exchange(0)), static_cast<unsigned long long>(resizePresents.exchange(0)), static_cast<unsigned long long>(gpuTiles.exchange(0)), static_cast<unsigned long long>(gpuTileFallbacks.exchange(0)), static_cast<unsigned long long>(gpuTilesMissing.exchange(0)), static_cast<unsigned long long>(gpuBuffers.exchange(0)), static_cast<unsigned long long>(gpuBufferFailures.exchange(0)), static_cast<unsigned long long>(gpuBuffersReused.exchange(0)), static_cast<unsigned long long>(unknownLayers.exchange(0)), static_cast<unsigned long long>(droppedBackingStores.exchange(0)), static_cast<unsigned long long>(partialNewTiles.exchange(0)));
#if USE(SKIA)
            // GPU raster's textures (GLASS_GPU_RASTER): now, the most during the window, the budget, the drawing
            // threads' EGL images still alive, what their pools hold and their Skia caches.
            auto memory = WebCore::D3D11SharedTextures::takeMemoryUse();
            std::fprintf(file, " gpuMemory=%.1fMB gpuMemoryPeak=%.1fMB gpuMemoryBudget=%.0fMB gpuImages=%llu gpuPooled=%.1fMB skiaCache=%.1fMB releaseFlushes=%llu watchFlushes=%llu trims=%llu", memory.bytes / 1048576.0, memory.peakBytes / 1048576.0, memory.budgetBytes / 1048576.0, static_cast<unsigned long long>(memory.images), memory.pooledBytes / 1048576.0, memory.skiaCacheBytes / 1048576.0, static_cast<unsigned long long>(memory.releaseFlushes), static_cast<unsigned long long>(memory.watchFlushes), static_cast<unsigned long long>(memory.trims));
#endif
            std::fputc('\n', file);
            std::fflush(file);
        }
    }

private:
    GPURenderingStats() = default;

    struct LastDraw {
        int sourceWidth { 0 };
        int sourceHeight { 0 };
        float sourceRectWidth { 0 };
        float sourceRectHeight { 0 };
        float destWidth { 0 };
        float destHeight { 0 };
        float ctmScale { 0 };
        bool accelerated { false };
        int quality { 0 };
        float clipWidth { 0 };
        float clipHeight { 0 };
    };

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
            auto path = directory + "gpu-rendering-stats-" + stamp + "-" + std::to_string(_getpid()) + ".log";
            return std::fopen(path.c_str(), "a");
        }();
        return file;
    }

    uint64_t m_createdUs { now() };
    std::atomic<uint64_t> m_lastReportUs { 0 };
    std::mutex m_lastDrawLock;
    std::mutex m_failureLock;
    const char* m_lastFailure { nullptr };
    LastDraw m_lastDraw;
};

} // namespace WebKit

#endif // OS(WINDOWS)
