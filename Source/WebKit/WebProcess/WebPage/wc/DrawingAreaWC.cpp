/*
 * Copyright (C) 2021 Sony Interactive Entertainment Inc.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "DrawingAreaWC.h"

#if USE(GRAPHICS_LAYER_WC)

#include "DrawingAreaProxyMessages.h"
#include "GraphicsLayerWC.h"
#include "ImageBufferShareableBitmapBackend.h"
#include "MessageSenderInlines.h"
#include "RemoteRenderingBackendProxy.h"
#include "RemoteWCLayerTreeHostProxy.h"
#include "UpdateInfo.h"
#include "WebFrame.h"
#include "WebPage.h"
#include "WebPageCreationParameters.h"
#include "WebPageInlines.h"
#include "WebPreferencesKeys.h"
#include "WebProcess.h"
#include <WebCore/DisplayRefreshMonitor.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/Page.h>
#include <WebCore/Settings.h>
#include <wtf/Borrow.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
using namespace WebCore;

DrawingAreaWC::DrawingAreaWC(WebPage& webPage, const WebPageCreationParameters& parameters)
    : DrawingArea(parameters.drawingAreaIdentifier, webPage)
    , m_remoteWCLayerTreeHostProxy(makeUniqueWithoutRefCountedCheck<RemoteWCLayerTreeHostProxy>(webPage, parameters.usesOffscreenRendering))
    , m_layerFactory(*this)
    , m_updateRenderingTimer(*this, &DrawingAreaWC::updateRendering)
    , m_commitQueue(WorkQueue::create("DrawingAreaWC CommitQueue"_s))
{
}

DrawingAreaWC::~DrawingAreaWC()
{
    while (auto layer = m_liveGraphicsLayers.removeHead())
        layer->clearObserver();
}

GraphicsLayerFactory* DrawingAreaWC::graphicsLayerFactory()
{
    return &m_layerFactory;
}

void DrawingAreaWC::updateRootLayers()
{
    for (auto& rootLayer : borrow(m_rootLayers).get()) {
        Vector<Ref<GraphicsLayer>> children;
        if (rootLayer.contentLayer) {
            children.append(*rootLayer.contentLayer);
            if (rootLayer.viewOverlayRootLayer)
                children.append(*rootLayer.viewOverlayRootLayer);
        }
        rootLayer.layer->setChildren(WTF::move(children));
    }
    triggerRenderingUpdate();
}

void DrawingAreaWC::setRootCompositingLayer(WebCore::Frame& frame, GraphicsLayer* rootGraphicsLayer)
{
    for (auto& rootLayer : m_rootLayers) {
        if (rootLayer.frameID == frame.frameID())
            rootLayer.contentLayer = rootGraphicsLayer;
    }
    if (rootGraphicsLayer)
        send(Messages::DrawingAreaProxy::EnterAcceleratedCompositingMode(m_backingStoreStateID, { }));
    updateRootLayers();
}

void DrawingAreaWC::addRootFrame(WebCore::FrameIdentifier frameID)
{
    auto layer = GraphicsLayer::create(graphicsLayerFactory(), this->m_rootLayerClient);
    layer->setName(makeString("drawing area root "_s, frameID.toUInt64()));
    layer->setAnchorPoint({ });
    m_rootLayers.append(RootLayerInfo {
        WTF::move(layer),
        nullptr,
        nullptr,
        frameID
    });
}

void DrawingAreaWC::attachViewOverlayGraphicsLayer(WebCore::FrameIdentifier frameID, GraphicsLayer* layer)
{
    if (auto* layerInfo = rootLayerInfoWithFrameIdentifier(frameID)) {
        layerInfo->viewOverlayRootLayer = layer;
        updateRootLayers();
    }
}

void DrawingAreaWC::updatePreferences(const WebPreferencesStore& store)
{
    Settings& settings = m_webPage->corePage()->settings();
    settings.setAcceleratedCompositingForFixedPositionEnabled(settings.acceleratedCompositingEnabled());
    settings.setForceCompositingMode(store.getBoolValueForKey(WebPreferencesKey::siteIsolationEnabledKey()));
}

bool DrawingAreaWC::shouldUseTiledBackingForFrameView(const WebCore::LocalFrameView& frameView) const
{
    return frameView.frame().isRootFrame();
}

void DrawingAreaWC::setLayerTreeStateIsFrozen(bool isFrozen)
{
    if (m_isRenderingSuspended == isFrozen)
        return;
    m_isRenderingSuspended = isFrozen;
    if (!m_isRenderingSuspended && m_hasDeferredRenderingUpdate) {
        m_hasDeferredRenderingUpdate = false;
        triggerRenderingUpdate();
    }
}

void DrawingAreaWC::updateGeometryWC(uint64_t backingStoreStateID, IntSize viewSize, float deviceScaleFactor, float intrinsicDeviceScaleFactor)
{
    m_backingStoreStateID = backingStoreStateID;
    m_webPage->setDeviceScaleFactor(deviceScaleFactor);
    m_webPage->setIntrinsicDeviceScaleFactor(intrinsicDeviceScaleFactor);
    m_webPage->setSize(viewSize);
}

void DrawingAreaWC::setNeedsDisplay()
{
    m_dirtyRegion = m_webPage->bounds();
    m_scrollRect = { };
    m_scrollOffset = { };
    triggerRenderingUpdate();
}

void DrawingAreaWC::setNeedsDisplayInRect(const IntRect& rect)
{
    if (isCompositingMode())
        return;
    IntRect dirtyRect = rect;
    dirtyRect.intersect(m_webPage->bounds());
    m_dirtyRegion.unite(dirtyRect);
    triggerRenderingUpdate();
}

void DrawingAreaWC::scroll(const IntRect& scrollRect, const IntSize& scrollDelta)
{
    if (isCompositingMode())
        return;

    if (scrollRect != m_scrollRect) {
        // Just repaint the entire current scroll rect, we'll scroll the new rect instead.
        setNeedsDisplayInRect(m_scrollRect);
        m_scrollRect = { };
        m_scrollOffset = { };
    }
    // Get the part of the dirty region that is in the scroll rect.
    Region dirtyRegionInScrollRect = intersect(scrollRect, m_dirtyRegion);
    if (!dirtyRegionInScrollRect.isEmpty()) {
        // There are parts of the dirty region that are inside the scroll rect.
        // We need to subtract them from the region, move them and re-add them.
        m_dirtyRegion.subtract(scrollRect);
        // Move the dirty parts.
        Region movedDirtyRegionInScrollRect = intersect(translate(dirtyRegionInScrollRect, scrollDelta), scrollRect);
        // And add them back.
        m_dirtyRegion.unite(movedDirtyRegionInScrollRect);
    }
    // Compute the scroll repaint region.
    Region scrollRepaintRegion = subtract(scrollRect, translate(scrollRect, scrollDelta));
    m_dirtyRegion.unite(scrollRepaintRegion);
    m_scrollRect = scrollRect;
    m_scrollOffset += scrollDelta;
    triggerRenderingUpdate();
}

void DrawingAreaWC::updateRenderingWithForcedRepaintAsync(WebPage&, CompletionHandler<void()>&& completionHandler)
{
    m_forceRepaintCompletionHandler = WTF::move(completionHandler);
    m_isForceRepaintCompletionHandlerDeferred = m_waitDidUpdate;
    setNeedsDisplay();
}

void DrawingAreaWC::triggerRenderingUpdate()
{
    if (m_isRenderingSuspended || m_waitDidUpdate) {
#if OS(WINDOWS)
        if (m_renderingStats && m_waitDidUpdate)
            ++m_renderingStats->deferredTriggers;
        DisplayRefreshMonitor::traceRenderingEvent('Q');
#endif
        m_hasDeferredRenderingUpdate = true;
        return;
    }
#if OS(WINDOWS)
    // A display tick is delivered at the top of the run loop, so update right away. A zero-delay timer
    // waits behind the page's own timers and tasks, often long enough for the next video frame to arrive
    // and replace the one this tick was for.
    // Composited pages only: without compositing, every repaint triggers an update directly, and those
    // land between ticks.
    if (DisplayRefreshMonitor::isDeliveringDisplayTick() && !m_inUpdateRendering && isCompositingMode()) {
        m_updateRenderingTimer.stop();
        updateRendering();
        return;
    }
#endif
    if (m_updateRenderingTimer.isActive())
        return;
#if OS(WINDOWS)
    DisplayRefreshMonitor::traceRenderingEvent('Z');
#endif
    m_updateRenderingTimer.startOneShot(0_s);
}

bool DrawingAreaWC::isCompositingMode()
{
    auto& mainWebFrame = m_webPage->mainWebFrame();
    if (!mainWebFrame.isRootFrame())
        return true;
    return rootLayerInfoWithFrameIdentifier(mainWebFrame.frameID())->contentLayer;
}

void DrawingAreaWC::updateRendering()
{
    if (m_isRenderingSuspended) {
        m_hasDeferredRenderingUpdate = true;
        return;
    }

    // This function is not reentrant, e.g. a rAF callback may force repaint.
    if (m_inUpdateRendering)
        return;
    SetForScope change(m_inUpdateRendering, true);

    ASSERT(!m_waitDidUpdate);
    m_waitDidUpdate = true;
#if OS(WINDOWS)
    DisplayRefreshMonitor::traceRenderingEvent('U');
#endif
#if OS(WINDOWS)
    if (m_renderingStats) {
        m_renderingStats->updateStartUs = RenderingUpdateStats::now();
        m_renderingStats->fenceDoneUs = 0;
        m_renderingStats->compositorSentUs = 0;
        m_renderingStats->uiSentUs = 0;
        ++m_renderingStats->updates;
    }
#endif

    Ref webPage = m_webPage.get();
    webPage->updateRendering();
    webPage->flushPendingEditorStateUpdate();
#if OS(WINDOWS)
    uint64_t pageDoneUs = m_renderingStats ? RenderingUpdateStats::now() : 0;
    if (m_renderingStats && m_renderingStats->updateStartUs)
        m_renderingStats->page.add(pageDoneUs - m_renderingStats->updateStartUs);
#endif

    OptionSet<FinalizeRenderingUpdateFlags> flags;
    webPage->finalizeRenderingUpdate(flags);
    webPage->didUpdateRendering();
#if OS(WINDOWS)
    if (m_renderingStats && pageDoneUs)
        m_renderingStats->flush.add(RenderingUpdateStats::now() - pageDoneUs);
#endif

    willStartRenderingUpdateDisplay();

    if (isCompositingMode())
        sendUpdateAC();
    else
        sendUpdateNonAC();
#if OS(WINDOWS)
    if (m_renderingStats && m_renderingStats->updateStartUs)
        m_renderingStats->web.add(RenderingUpdateStats::now() - m_renderingStats->updateStartUs);
#endif
}

void DrawingAreaWC::sendUpdateAC()
{
    bool didProcessMainFrame = false;
    Borrow rootLayers = m_rootLayers;
    for (size_t i = 0; i < rootLayers->size(); i++) {
        auto& rootLayer = rootLayers.get()[i];
        auto frame = WebProcess::singleton().webFrame(rootLayer.frameID);
        ASSERT(frame);
        m_updateInfo.remoteContextHostedIdentifier = frame->layerHostingContextIdentifier();
        m_updateInfo.rootLayer = rootLayer.layer->primaryLayerID();

        bool isLastFrame = (i + 1) == rootLayers->size();
        bool isMainFrame = frame->isMainFrame();
        didProcessMainFrame = didProcessMainFrame || isMainFrame;
        bool willCallDisplayDidRefresh = isMainFrame || (!didProcessMainFrame && isLastFrame);
        IntSize size;
        if (isMainFrame)
            size = m_webPage->size();
        else
            size = frame->size();
        FloatSize viewport { size };
        viewport.scale(m_webPage->intrinsicDeviceScaleFactor());
        m_updateInfo.viewport = WebCore::expandedIntSize(viewport);
        updateRootLayerDeviceScaleFactor(rootLayer.layer);
        rootLayer.layer->flushCompositingStateForThisLayerOnly();

        // Because our view-relative overlay root layer is not attached to the FrameView's GraphicsLayer tree, we need to flush it manually.
        FloatRect visibleRect({ }, size);
        if (rootLayer.viewOverlayRootLayer)
            rootLayer.viewOverlayRootLayer->flushCompositingState(visibleRect);

        auto fence = m_webPage->ensureRemoteRenderingBackendProxy().flushImageBuffers();

#if OS(WINDOWS)
        RefPtr stats = willCallDisplayDidRefresh ? m_renderingStats : nullptr;
#endif
        m_commitQueue->dispatch([this, weakThis = WeakPtr(*this), stateID = m_backingStoreStateID, updateInfo = std::exchange(m_updateInfo, { }), fence = WTF::move(fence), willCallDisplayDidRefresh
#if OS(WINDOWS)
            , stats
#endif
            ]() mutable {
#if OS(WINDOWS)
            auto fenceStart = stats ? RenderingUpdateStats::now() : 0;
#endif
            fence();
#if OS(WINDOWS)
            if (stats) {
                auto done = RenderingUpdateStats::now();
                stats->gpuPaint.add(done - fenceStart);
                stats->fenceDoneUs = done;
            }
#endif
            RunLoop::mainSingleton().dispatch([this, weakThis = WTF::move(weakThis), stateID, updateInfo = WTF::move(updateInfo), willCallDisplayDidRefresh
#if OS(WINDOWS)
                , stats
#endif
                ]() mutable {
                if (!weakThis)
                    return;
#if OS(WINDOWS)
                if (stats) {
                    auto nowUs = RenderingUpdateStats::now();
                    if (auto done = stats->fenceDoneUs.load())
                        stats->hop.add(nowUs - done);
                    stats->compositorSentUs = nowUs;
                    ++stats->compositedUpdates;
                }
#endif
                m_remoteWCLayerTreeHostProxy->update(WTF::move(updateInfo), [this, weakThis = WTF::move(weakThis), stateID, willCallDisplayDidRefresh
#if OS(WINDOWS)
                    , stats
#endif
                    ](std::optional<UpdateInfo> updateInfo) {
                    if (!weakThis)
                        return;
#if OS(WINDOWS)
                    if (stats) {
                        auto nowUs = RenderingUpdateStats::now();
                        if (auto sent = stats->compositorSentUs.load())
                            stats->compositor.add(nowUs - sent);
                        if (updateInfo && stateID == m_backingStoreStateID)
                            stats->uiSentUs = nowUs;
                    }
#endif
                    if (updateInfo && stateID == m_backingStoreStateID) {
                        ASSERT(willCallDisplayDidRefresh);
                        send(Messages::DrawingAreaProxy::Update(m_backingStoreStateID, WTF::move(*updateInfo)));
                        return;
                    }
                    if (willCallDisplayDidRefresh)
                        displayDidRefresh();
                });
            });
        });
    }
}

void DrawingAreaWC::updateRootLayerDeviceScaleFactor(WebCore::GraphicsLayer& layer)
{
    TransformationMatrix m;
    m.scale(m_webPage->intrinsicDeviceScaleFactor());
    layer.setTransform(m);
}

static bool shouldPaintBoundsRect(const IntRect& bounds, const Vector<IntRect, 1>& rects)
{
    constexpr size_t rectThreshold = 10;
    constexpr double wastedSpaceThreshold = 0.75;

    if (rects.size() <= 1 || rects.size() > rectThreshold)
        return true;
    // Attempt to guess whether or not we should use the region bounds rect or the individual rects.
    // We do this by computing the percentage of "wasted space" in the bounds. If that wasted space
    // is too large, then we will do individual rect painting instead.
    unsigned boundsArea = bounds.width() * bounds.height();
    unsigned rectsArea = 0;
    for (size_t i = 0; i < rects.size(); ++i)
        rectsArea += rects[i].width() * rects[i].height();

    double wastedSpace = 1 - (static_cast<double>(rectsArea) / boundsArea);
    return wastedSpace <= wastedSpaceThreshold;
}

void DrawingAreaWC::sendUpdateNonAC()
{
    if (m_dirtyRegion.isEmpty()) {
        displayDidRefresh();
        return;
    }
    IntRect bounds = m_dirtyRegion.bounds();
    Ref webPage = m_webPage.get();
    ASSERT(webPage->bounds().contains(bounds));
    IntSize bitmapSize = bounds.size();
    float deviceScaleFactor = webPage->corePage()->deviceScaleFactor();
    auto image = createImageBuffer(bitmapSize, deviceScaleFactor);
    auto rects = m_dirtyRegion.rects();
    if (shouldPaintBoundsRect(bounds, rects)) {
        rects.clear();
        rects.append(bounds);
    }
    m_dirtyRegion = { };

    UpdateInfo updateInfo;
    updateInfo.viewSize = webPage->size();
    updateInfo.deviceScaleFactor = deviceScaleFactor;
    updateInfo.updateRectBounds = bounds;
    updateInfo.updateRects = rects;
    updateInfo.scrollRect = m_scrollRect;
    updateInfo.scrollOffset = m_scrollOffset;
    m_scrollRect = { };
    m_scrollOffset = { };

    auto& graphicsContext = image->context();
    graphicsContext.translate(-bounds.x(), -bounds.y());
    for (const auto& rect : rects)
        webPage->drawRect(graphicsContext, rect);
    image->flushDrawingContextAsync();

    auto fence = m_webPage->ensureRemoteRenderingBackendProxy().flushImageBuffers();

#if OS(WINDOWS)
    RefPtr stats = m_renderingStats;
#endif
    m_commitQueue->dispatch([this, weakThis = WeakPtr(*this), stateID = m_backingStoreStateID, updateInfo = WTF::move(updateInfo), image = WTF::move(image), fence = WTF::move(fence)
#if OS(WINDOWS)
        , stats
#endif
        ]() mutable {
#if OS(WINDOWS)
        auto fenceStart = stats ? RenderingUpdateStats::now() : 0;
#endif
        fence();
#if OS(WINDOWS)
        if (stats) {
            auto done = RenderingUpdateStats::now();
            stats->gpuPaint.add(done - fenceStart);
            stats->fenceDoneUs = done;
        }
#endif
        RunLoop::mainSingleton().dispatch([this, weakThis = WTF::move(weakThis), stateID, updateInfo = WTF::move(updateInfo), image = WTF::move(image)
#if OS(WINDOWS)
            , stats
#endif
            ]() mutable {
            if (!weakThis)
                return;
#if OS(WINDOWS)
            if (stats) {
                auto nowUs = RenderingUpdateStats::now();
                if (auto done = stats->fenceDoneUs.load())
                    stats->hop.add(nowUs - done);
                stats->uiSentUs = nowUs;
            }
#endif
            if (stateID != m_backingStoreStateID) {
                displayDidRefresh();
                return;
            }

            auto* sharing = image->toBackendSharing();
            if (is<ImageBufferBackendHandleSharing>(sharing)) {
                if (auto handle = downcast<ImageBufferBackendHandleSharing>(*sharing).createBackendHandle())
                    updateInfo.bitmapHandle = std::get<ShareableBitmap::Handle>(WTF::move(*handle));
            }

            send(Messages::DrawingAreaProxy::Update(stateID, WTF::move(updateInfo)));
        });
    });
}

void DrawingAreaWC::graphicsLayerAdded(GraphicsLayerWC& layer)
{
    m_liveGraphicsLayers.append(&layer);
    m_updateInfo.addedLayers.append(*layer.primaryLayerID());
}

void DrawingAreaWC::graphicsLayerRemoved(GraphicsLayerWC& layer)
{
    m_liveGraphicsLayers.remove(&layer);
    m_updateInfo.removedLayers.append(*layer.primaryLayerID());
}

void DrawingAreaWC::commitLayerUpdateInfo(WCLayerUpdateInfo&& info)
{
    m_updateInfo.changedLayers.append(WTF::move(info));
}

RefPtr<ImageBuffer> DrawingAreaWC::createImageBuffer(FloatSize size, float deviceScaleFactor)
{
    if (WebProcess::singleton().shouldUseRemoteRenderingFor(RenderingPurpose::DOM))
        return Ref { m_webPage.get() }->ensureRemoteRenderingBackendProxy().createImageBuffer(size, RenderingMode::Unaccelerated, RenderingPurpose::DOM, deviceScaleFactor, ColorSpace::SRGB(), { PixelFormat::BGRA8 });
    return ImageBuffer::create<ImageBufferShareableBitmapBackend>(size, deviceScaleFactor, ColorSpace::SRGB(), { PixelFormat::BGRA8 }, RenderingPurpose::DOM, { });
}

#if ENABLE(VIDEO) && OS(WINDOWS)
RefPtr<WebCore::MediaPlayerCompositorFrameSink> DrawingAreaWC::createVideoFrameSink(WebCore::PlatformLayerIdentifier layer)
{
    return m_remoteWCLayerTreeHostProxy->createVideoFrameSink(layer);
}
#endif

void DrawingAreaWC::displayDidRefresh(MonotonicTime)
{
#if OS(WINDOWS)
    if (m_renderingStats) {
        auto nowUs = RenderingUpdateStats::now();
        if (auto start = m_renderingStats->updateStartUs.exchange(0))
            m_renderingStats->cycle.add(nowUs - start);
        if (auto sent = m_renderingStats->uiSentUs.exchange(0))
            m_renderingStats->ui.add(nowUs - sent);
        m_renderingStats->reportIfDue();
    }
#endif
    m_waitDidUpdate = false;
#if OS(WINDOWS)
    DisplayRefreshMonitor::traceRenderingEvent('G');
#endif
    didCompleteRenderingUpdateDisplay();
    didCompleteRenderingFrame();
    if (m_forceRepaintCompletionHandler) {
        if (m_isForceRepaintCompletionHandlerDeferred)
            m_isForceRepaintCompletionHandlerDeferred = false;
        else
            m_forceRepaintCompletionHandler();
    }
    if (m_hasDeferredRenderingUpdate) {
        m_hasDeferredRenderingUpdate = false;
#if OS(WINDOWS)
        // The GPU's reply is handled at the top of the run loop, like a display tick, so run the update
        // that was waiting for it right away instead of queuing it behind the page's timers and tasks.
        // Composited pages only, for the same reason as in triggerRenderingUpdate().
        if (!m_isRenderingSuspended && !m_inUpdateRendering && isCompositingMode()) {
            m_updateRenderingTimer.stop();
            updateRendering();
            return;
        }
#endif
        triggerRenderingUpdate();
    }
}

DrawingAreaWC::RootLayerInfo* DrawingAreaWC::rootLayerInfoWithFrameIdentifier(WebCore::FrameIdentifier frameID)
{
    auto index = m_rootLayers.findIf([&] (const auto& layer) {
        return layer.frameID == frameID;
    });
    if (index == WTF::notFound) {
        ASSERT_NOT_REACHED();
        return nullptr;
    }
    return &m_rootLayers[index];
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
