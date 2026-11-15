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
#if OS(WINDOWS)
    // After the other clients (the rendering backend's image buffers) have handled the close too.
    m_remoteWCLayerTreeHostProxy->setGPUProcessDidCloseHandler([weakThis = WeakPtr { *this }](GPUProcessConnectionIdentifier connection) {
        RunLoop::mainSingleton().dispatch([weakThis, connection] {
            if (weakThis)
                weakThis->gpuProcessDidClose(connection);
        });
    });
#endif
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
#if OS(WINDOWS)
    if (frame.isMainFrame())
        updateLayerTreeHostFrozen();
#endif
    updateRootLayers();
}

#if OS(WINDOWS)
// The GPU process's layer tree host presents the page's last composited frame again up to a second after an update (see
// RemoteWCLayerTreeHost::scheduleShownPresents). Not while rendering is suspended (a process swap), and not once the page
// has left compositing mode: the browser then paints the page itself, and the old frame presented over that paint stayed
// until something repainted that part (1.19.69 on the GPU PC: about:blank after the New Tab page showed the New Tab page
// for minutes; a BBC page kept its consent box's dark overlay after the box had gone, until a scroll).
void DrawingAreaWC::updateLayerTreeHostFrozen()
{
    // As isCompositingMode(), without its assertion: this can run before the main frame's root layer is added.
    auto& mainWebFrame = m_webPage->mainWebFrame();
    auto index = m_rootLayers.findIf([&](const auto& layer) {
        return layer.frameID == mainWebFrame.frameID();
    });
    bool composited = !mainWebFrame.isRootFrame() || (index != notFound && m_rootLayers[index].contentLayer);
    m_remoteWCLayerTreeHostProxy->setFrozen(m_isRenderingSuspended || !composited);
}
#endif

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
    // Glass: GLASS_FORCE_COMPOSITING=1 composites every page while accelerated compositing is on, as Chrome does,
    // instead of only pages that need layers (a page without them is painted as one bitmap for the UI process). Off by
    // default: it cost MotionMark about 5% (a layer-tree flush on every frame) with nothing drawn on the GPU yet.
    static const bool forceCompositing = [] {
        const char* force = getenv("GLASS_FORCE_COMPOSITING");
        return force && !strcmp(force, "1");
    }();
    settings.setForceCompositingMode(store.getBoolValueForKey(WebPreferencesKey::siteIsolationEnabledKey())
        || (forceCompositing && settings.acceleratedCompositingEnabled()));
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
#if OS(WINDOWS)
    updateLayerTreeHostFrozen();
#endif
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
#if OS(WINDOWS)
    m_forceRepaintUpdatesToSkip = m_updatesInFlight;
#else
    m_forceRepaintUpdatesToSkip = m_waitDidUpdate ? 1 : 0;
#endif
    setNeedsDisplay();
}

void DrawingAreaWC::triggerRenderingUpdate()
{
#if OS(WINDOWS)
    bool isTick = DisplayRefreshMonitor::isDeliveringDisplayTick();
#endif
    if (m_isRenderingSuspended || m_waitDidUpdate) {
#if OS(WINDOWS)
        if (m_renderingStats && m_waitDidUpdate)
            ++m_renderingStats->deferredTriggers;
        DisplayRefreshMonitor::traceRenderingEvent('Q');
        // A tick that came while the GPU was still busy. Not a request made during the update itself (a rAF callback
        // repainting): the tick's flag is still set then, but running that one right after the GPU's reply would
        // double the update rate.
        if (isTick && !m_inUpdateRendering)
            m_deferredRenderingUpdateIsTick = true;
#endif
        m_hasDeferredRenderingUpdate = true;
        return;
    }
#if OS(WINDOWS)
    // A display tick is delivered at the top of the run loop, so update right away. A zero-delay timer
    // waits behind the page's own timers and tasks, often long enough for the next video frame to arrive
    // and replace the one this tick was for. (RenderingUpdateScheduler marks the updates it fires, from the
    // display clock or its timer, as ticks.)
    if (isTick && !m_inUpdateRendering) {
        m_updateRenderingTimer.stop();
        updateRendering();
        return;
    }
    // Anything else on a visible page (a repaint of a page that isn't composited, a scroll, a forced repaint)
    // waits for the next tick. It used to start an update straight away, and every update runs the page's
    // requestAnimationFrame callbacks: a callback that repaints (moving an element) asked for the next update itself,
    // so the loop ran as fast as an update completes, about 650 callbacks a second on a 60 Hz display (60 only
    // where timers fire late). An invisible page's scheduler triggers its update synchronously, from wherever the
    // request was made, so those (and a tick that comes during an update) keep the zero-delay timer below.
    static bool isSchedulingRenderingUpdate; // the scheduler can call back synchronously
    if (!isTick && !isSchedulingRenderingUpdate) {
        if (RefPtr page = m_webPage->corePage(); page && page->isVisible()) {
            SetForScope scheduling(isSchedulingRenderingUpdate, true);
            DisplayRefreshMonitor::traceRenderingEvent('Z');
            page->scheduleRenderingUpdate(WebCore::RenderingUpdateStep::LayerFlush);
            return;
        }
    }
#endif
    if (m_updateRenderingTimer.isActive())
        return;
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
    bool pipelined = m_updatesInFlight++;
    DisplayRefreshMonitor::traceRenderingEvent('U');
#endif
#if OS(WINDOWS)
    if (m_renderingStats) {
        m_renderingStats->updateStartUs = RenderingUpdateStats::now();
        m_updateStartsUs.append(m_renderingStats->updateStartUs);
        if (pipelined)
            ++m_renderingStats->pipelinedUpdates;
        m_renderingStats->fenceDoneUs = 0;
        m_renderingStats->compositorSentUs = 0;
        m_renderingStats->uiSentUs = 0;
        ++m_renderingStats->updates;
    }
    RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::Web);
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
    // The next update (the next display tick's) may start while this one is still being drawn and composited.
    if (m_waitDidUpdate && m_updatesInFlight == 1 && canPipelineRenderingUpdate())
        m_waitDidUpdate = false;
#endif
}

#if OS(WINDOWS)
// Glass: with GPU tiles, waiting for the GPU process to draw an update's tiles (the fence) took longer than drawing them
// on the processor (each small processor-drawn image is a texture upload), and the next update couldn't start until
// the whole update was displayed: the page's script plus that wait made frames miss the display (MotionMark Images 20%
// lower). The next update's script and painting now overlap this one's drawing and compositing. That is safe for
// tiles: every tile update draws into a new image buffer, which the compositor copies in update order, and the GPU
// process draws both updates' tiles in the order they were recorded. Not with a WebGL layer that changed (its display
// buffers are shown directly), with a forced repaint waiting, or on a page that isn't composited.
// Only with GLASS_PIPELINE_UPDATES=1: on the GPU PC it didn't help (1.19.63: the page's script got as much slower as the
// wait it hid, and MotionMark Images scored 7% lower), the cost being small layers' GPU tiles (drawsLayerOnProcessor).
bool DrawingAreaWC::canPipelineRenderingUpdate()
{
    static const bool enabled = [] {
        auto* value = std::getenv("GLASS_PIPELINE_UPDATES");
        return value && *value == '1';
    }();
    return enabled && RemoteRenderingBackendProxy::gpuRasterEnabled(RenderingPurpose::DOM) && !m_lastUpdateChangedPlatformLayers
        && !m_forceRepaintCompletionHandler && isCompositingMode();
}
#endif

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
        if (!i)
            m_lastUpdateChangedPlatformLayers = false;
        for (auto& layer : m_updateInfo.changedLayers) {
            if (!layer.platformLayer.identifiers.isEmpty())
                m_lastUpdateChangedPlatformLayers = true;
        }
#endif
        m_commitQueue->dispatch([this, weakThis = WeakPtr(*this), stateID = m_backingStoreStateID, updateInfo = std::exchange(m_updateInfo, { }), fence = WTF::move(fence), willCallDisplayDidRefresh
#if OS(WINDOWS)
            , stats
#endif
            ]() mutable {
#if OS(WINDOWS)
            auto fenceStart = stats ? RenderingUpdateStats::now() : 0;
            RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::Fence);
#endif
            fence();
#if OS(WINDOWS)
            if (stats) {
                auto done = RenderingUpdateStats::now();
                stats->gpuPaint.add(done - fenceStart);
                stats->fenceDoneUs = done;
            }
            RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::Hop);
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
                RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::Compositor);
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
#if OS(WINDOWS)
                        RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::UI);
#endif
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
    auto image = createBitmapImageBuffer(bitmapSize, deviceScaleFactor);
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
        RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::Fence);
#endif
        fence();
#if OS(WINDOWS)
        if (stats) {
            auto done = RenderingUpdateStats::now();
            stats->gpuPaint.add(done - fenceStart);
            stats->fenceDoneUs = done;
        }
        RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::Hop);
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

#if OS(WINDOWS)
            RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::UI);
#endif
            send(Messages::DrawingAreaProxy::Update(stateID, WTF::move(updateInfo)));
        });
    });
}

#if OS(WINDOWS)
void DrawingAreaWC::gpuProcessDidClose(GPUProcessConnectionIdentifier connection)
{
    // Glass: the GPU process has gone (a crash, or Windows resetting the graphics driver), and the next update starts a
    // new one whose scene is empty. Every layer is described to it again, whole, with its tiles painted anew. It used to
    // get only what changed next, layers it didn't have were skipped, and composited pages stayed blank.
    bool onProcessor = RemoteRenderingBackendProxy::noteGPUProcessLost();
    // The page's image buffers first (an update still waiting on the gone process gives up), then its layers.
    Ref { m_webPage.get() }->ensureRemoteRenderingBackendProxy().gpuProcessDidClose(connection);
    unsigned layers = 0;
    for (auto* layer = m_liveGraphicsLayers.head(); layer; layer = layer->next(), ++layers) {
        m_updateInfo.addedLayers.append(*layer->primaryLayerID());
        layer->resyncWithNewCompositor();
    }
    RenderingUpdateStats::noteGPUProcessResync(layers, onProcessor);
    triggerRenderingUpdate();
}
#endif

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

#if OS(WINDOWS)
// Glass: a small layer's tiles are drawn on the processor even with GLASS_GPU_RASTER. MotionMark's Images test puts
// hundreds of 50x50 canvases on their own layers and repaints each every frame; on the GPU every one of those tiles cost
// a surface, a flush, a texture upload of the canvas and a copy into the compositor's texture, which made the test 25%
// slower than drawing them all on the processor. GLASS_GPU_TILE_MIN_PIXELS sets the smallest layer (in device pixels)
// drawn on the GPU: 128x128 by default, 0 for every layer.
static bool drawsLayerOnProcessor(FloatSize layerSize, float deviceScaleFactor)
{
    static const double minimumPixels = [] {
        auto* value = std::getenv("GLASS_GPU_TILE_MIN_PIXELS");
        return value && *value ? std::atof(value) : 128.0 * 128.0;
    }();
    return layerSize.area() * deviceScaleFactor * deviceScaleFactor < minimumPixels;
}
#endif

RefPtr<ImageBuffer> DrawingAreaWC::createImageBuffer(FloatSize size, float deviceScaleFactor, FloatSize layerSize)
{
#if OS(WINDOWS)
    // GLASS_GPU_RASTER=1: layer tiles are drawn on the GPU, in the GPU process, which falls back to a bitmap where it can't.
    if (RemoteRenderingBackendProxy::gpuRasterEnabled(RenderingPurpose::DOM) && WebProcess::singleton().shouldUseRemoteRenderingFor(RenderingPurpose::DOM)) {
        if (!drawsLayerOnProcessor(layerSize, deviceScaleFactor))
            return Ref { m_webPage.get() }->ensureRemoteRenderingBackendProxy().createImageBuffer(size, RenderingMode::Accelerated, RenderingPurpose::DOM, deviceScaleFactor, ColorSpace::SRGB(), { PixelFormat::BGRA8 });
        if (m_renderingStats)
            ++m_renderingStats->smallLayerTiles;
    }
#else
    UNUSED_PARAM(layerSize);
#endif
    return createBitmapImageBuffer(size, deviceScaleFactor);
}

RefPtr<ImageBuffer> DrawingAreaWC::createBitmapImageBuffer(FloatSize size, float deviceScaleFactor)
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

void DrawingAreaWC::displayDidRefresh(MonotonicTime uiTime)
{
#if OS(WINDOWS)
    if (m_renderingStats) {
        auto nowUs = RenderingUpdateStats::now();
        m_renderingStats->updateStartUs = 0;
        if (!m_updateStartsUs.isEmpty())
            m_renderingStats->cycle.add(nowUs - m_updateStartsUs.takeFirst());
        if (auto sent = m_renderingStats->uiSentUs.exchange(0)) {
            m_renderingStats->ui.add(nowUs - sent);
            // The UI process's reply carries the time it handled the update.
            auto handledUs = static_cast<uint64_t>(uiTime.secondsSinceEpoch().microseconds());
            if (handledUs >= sent && handledUs <= nowUs) {
                m_renderingStats->uiHandle.add(handledUs - sent);
                m_renderingStats->uiReturn.add(nowUs - handledUs);
            }
        }
        m_renderingStats->reportIfDue();
    }
#endif
#if OS(WINDOWS)
    if (m_updatesInFlight)
        --m_updatesInFlight;
    // One still in flight: another may start only if it could have started alongside it.
    m_waitDidUpdate = m_updatesInFlight && !canPipelineRenderingUpdate();
    DisplayRefreshMonitor::traceRenderingEvent('G');
    if (!m_updatesInFlight)
        RenderingUpdateStats::noteStage(RenderingUpdateStats::Stage::Idle);
#else
    m_waitDidUpdate = false;
#endif
    didCompleteRenderingUpdateDisplay();
    didCompleteRenderingFrame();
    if (m_forceRepaintCompletionHandler) {
        if (m_forceRepaintUpdatesToSkip)
            --m_forceRepaintUpdatesToSkip;
        else
            m_forceRepaintCompletionHandler();
    }
    if (m_hasDeferredRenderingUpdate && !m_waitDidUpdate) {
        m_hasDeferredRenderingUpdate = false;
#if OS(WINDOWS)
        // The GPU's reply is handled at the top of the run loop, like a display tick, so run the update
        // that was waiting for it right away instead of queuing it behind the page's timers and tasks.
        // On a composited page, or when the update that waited was a display tick's: a frame that took longer than
        // one tick then shows as soon as it can rather than a whole tick later (frames at 30 per second instead of
        // nearly 60, which cut MotionMark's score by 15%). A page that isn't composited and asked for the update
        // with a repaint waits for the next tick (see triggerRenderingUpdate()).
        bool deferredUpdateWasTick = std::exchange(m_deferredRenderingUpdateIsTick, false);
        if (!m_isRenderingSuspended && !m_inUpdateRendering && (isCompositingMode() || deferredUpdateWasTick)) {
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
