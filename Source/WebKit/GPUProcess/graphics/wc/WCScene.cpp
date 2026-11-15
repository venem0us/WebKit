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
#include "WCScene.h"

#if USE(GRAPHICS_LAYER_WC)

#include "GPURenderingStats.h"
#include "RemoteGraphicsContextGL.h"
#include "WCContentBuffer.h"
#include "WCContentBufferManager.h"
#include "WCRemoteFrameHostLayerManager.h"
#include "WCSceneContext.h"
#include "WCUpdateInfo.h"
#include <WebCore/BitmapTexturePool.h>
#include <WebCore/NativeImage.h>
#include <WebCore/ShareableBitmap.h>
#include <WebCore/TextureMapper.h>
#include <WebCore/TextureMapperGLHeaders.h>
#include <WebCore/TextureMapperLayer.h>
#include <WebCore/TextureMapperPlatformLayer.h>
#include <WebCore/TextureMapperSparseBackingStore.h>
#include <wtf/TZoneMallocInlines.h>
#if OS(WINDOWS)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#endif

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WCScene);

#if OS(WINDOWS)
// A layer's contents drawn from a GL texture: a video frame the web process left on the GPU (see importVideoTexture).
class WCVideoTextureLayer final : public WebCore::TextureMapperPlatformLayer {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(WCVideoTextureLayer);
public:
    void setTexture(GLuint texture) { m_texture = texture; }
    void paintToTextureMapper(WebCore::TextureMapper& textureMapper, const WebCore::FloatRect& targetRect, const WebCore::TransformationMatrix& modelViewMatrix, float opacity) final
    {
        if (m_texture)
            textureMapper.drawTexture(m_texture, { }, targetRect, modelViewMatrix, opacity);
    }

private:
    GLuint m_texture { 0 };
};

// One of the web process's shared textures, opened on ANGLE's Direct3D 11 device and bound to a GL texture.
struct WCScene::VideoTexture {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(WCScene::VideoTexture);
public:
    explicit VideoTexture(WebCore::PlatformLayerIdentifier layer)
        : layer(layer)
    {
    }

    ~VideoTexture()
    {
        if (held)
            mutex->ReleaseSync(0);
        if (glTexture)
            glDeleteTextures(1, &glTexture);
        if (image != EGL_NO_IMAGE_KHR) {
            static auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
            if (destroyImage)
                destroyImage(display, image);
        }
    }

    WebCore::PlatformLayerIdentifier layer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    EGLDisplay display { EGL_NO_DISPLAY };
    EGLImageKHR image { EGL_NO_IMAGE_KHR };
    GLuint glTexture { 0 };
    bool held { false }; // acquired with key 1: shown by its layer
};

// ANGLE's Direct3D 11 device for the current EGL display.
static ID3D11Device* angleD3D11Device()
{
    static auto queryDisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglGetProcAddress("eglQueryDisplayAttribEXT"));
    static auto queryDevice = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(eglGetProcAddress("eglQueryDeviceAttribEXT"));
    if (!queryDisplay || !queryDevice)
        return nullptr;
    EGLAttrib device = 0;
    EGLAttrib d3dDevice = 0;
    if (!queryDisplay(eglGetCurrentDisplay(), EGL_DEVICE_EXT, &device) || !queryDevice(reinterpret_cast<EGLDeviceEXT>(device), EGL_D3D11_DEVICE_ANGLE, &d3dDevice))
        return nullptr;
    return reinterpret_cast<ID3D11Device*>(d3dDevice);
}
#endif

struct WCScene::Layer final : public WCContentBuffer::Client {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(WCScene::Layer);
public:
    Layer() = default;
    ~Layer()
    {
        if (contentBuffer)
            contentBuffer->setClient(nullptr);
        if (hostIdentifier)
            WCRemoteFrameHostLayerManager::singleton().releaseRemoteFrameHostLayer(*hostIdentifier);
    }

    // WCContentBuffer::Client
    void platformLayerWillBeDestroyed() override
    {
        contentBuffer = nullptr;
        texmapLayer.setContentsLayer(nullptr);
    }

    WebCore::TextureMapperLayer texmapLayer;
    std::unique_ptr<WebCore::TextureMapperSparseBackingStore> backingStore;
    std::unique_ptr<WebCore::TextureMapperLayer> backdropLayer;
    // The current frame of a video the web process hands over frame by frame, drawn into the contents rect.
    std::unique_ptr<WebCore::TextureMapperSparseBackingStore> videoFrame;
#if OS(WINDOWS)
    // ...or, for frames left on the GPU, the shared texture it shows (an id in m_videoTextures) and the layer drawing it.
    std::unique_ptr<WCVideoTextureLayer> videoTextureLayer;
    uint64_t shownTexture { 0 };
#endif
    WCContentBuffer* contentBuffer { nullptr };
    Markable<WebCore::LayerHostingContextIdentifier> hostIdentifier;
};

void WCScene::initialize(WCSceneContext& context)
{
    // The creation of the TextureMapper needs an active OpenGL context.
    m_context = &context;
    if (!m_context->makeContextCurrent())
        return;
    m_textureMapper = m_context->createTextureMapper();
}

WCScene::WCScene(WebCore::ProcessIdentifier webProcessIdentifier, bool usesOffscreenRendering)
    : m_webProcessIdentifier(webProcessIdentifier)
    , m_usesOffscreenRendering(usesOffscreenRendering)
{
}

WCScene::~WCScene()
{
    if (!m_context->makeContextCurrent())
        return;
    m_textureMapper = nullptr;
}

std::optional<UpdateInfo> WCScene::update(WCUpdateInfo&& update)
{
#if OS(WINDOWS)
    auto* stats = GPURenderingStats::singletonIfEnabled();
    GPURenderingStats::ScopedTimer compositeTimer(stats ? &stats->composite : nullptr);
#endif
    if (!m_context->makeContextCurrent())
        return std::nullopt;
    m_textureMapper->releaseUnusedTexturesNow();

    for (auto id : update.addedLayers) {
        auto layer = makeUnique<Layer>();
        m_layers.add(id, WTF::move(layer));
    }

    for (auto& layerUpdate : update.changedLayers) {
        auto layer = m_layers.get(layerUpdate.id);
        if (layerUpdate.changes & WCLayerChange::Children) {
            layer->texmapLayer.setChildren(WTF::map(layerUpdate.children, [&](auto id) {
                return &m_layers.get(id)->texmapLayer;
            }));
        }
        if (layerUpdate.changes & WCLayerChange::MaskLayer) {
            WebCore::TextureMapperLayer* maskLayer = nullptr;
            if (layerUpdate.maskLayer)
                maskLayer = &m_layers.get(*layerUpdate.maskLayer)->texmapLayer;
            layer->texmapLayer.setMaskLayer(maskLayer);
        }
        if (layerUpdate.changes & WCLayerChange::ReplicaLayer) {
            WebCore::TextureMapperLayer* replicaLayer = nullptr;
            if (layerUpdate.replicaLayer)
                replicaLayer = &m_layers.get(*layerUpdate.replicaLayer)->texmapLayer;
            layer->texmapLayer.setReplicaLayer(replicaLayer);
        }
        if (layerUpdate.changes & WCLayerChange::Position)
            layer->texmapLayer.setPosition(layerUpdate.position);
        if (layerUpdate.changes & WCLayerChange::AnchorPoint)
            layer->texmapLayer.setAnchorPoint(layerUpdate.anchorPoint);
        if (layerUpdate.changes & WCLayerChange::Size)
            layer->texmapLayer.setSize(layerUpdate.size);
        if (layerUpdate.changes & WCLayerChange::BoundsOrigin)
            layer->texmapLayer.setBoundsOrigin(layerUpdate.boundsOrigin);
        if (layerUpdate.changes & WCLayerChange::Preserves3D)
            layer->texmapLayer.setPreserves3D(layerUpdate.preserves3D);
        if (layerUpdate.changes & WCLayerChange::ContentsRect)
            layer->texmapLayer.setContentsRect(layerUpdate.contentsRect);
        if (layerUpdate.changes & WCLayerChange::ContentsClippingRect)
            layer->texmapLayer.setContentsClippingRect(layerUpdate.contentsClippingRect);
        if (layerUpdate.changes & WCLayerChange::ContentsRectClipsDescendants)
            layer->texmapLayer.setContentsRectClipsDescendants(layerUpdate.contentsRectClipsDescendants);
        if (layerUpdate.changes & WCLayerChange::ContentsVisible)
            layer->texmapLayer.setContentsVisible(layerUpdate.contentsVisible);
        if (layerUpdate.changes & WCLayerChange::BackfaceVisibility)
            layer->texmapLayer.setBackfaceVisibility(layerUpdate.backfaceVisibility);
        if (layerUpdate.changes & WCLayerChange::MasksToBounds)
            layer->texmapLayer.setMasksToBounds(layerUpdate.masksToBounds);
        if (layerUpdate.changes & WCLayerChange::Background) {
            if (layerUpdate.background.hasBackingStore) {
                if (!layer->backingStore) {
                    layer->backingStore = makeUnique<WebCore::TextureMapperSparseBackingStore>();
                    auto& backingStore = *layer->backingStore;
                    layer->texmapLayer.setBackgroundColor({ });
                    layer->texmapLayer.setBackingStore(&backingStore);
                }
                auto& backingStore = *layer->backingStore;
                backingStore.setSize(layerUpdate.background.backingStoreSize);
                for (auto& tileUpdate : layerUpdate.background.tileUpdates) {
                    if (tileUpdate.willRemove)
                        backingStore.removeTile(tileUpdate.index);
                    else {
                        auto bitmap = tileUpdate.backingStore.bitmap();
                        if (bitmap) {
                            if (RefPtr image = WebCore::NativeImage::create(bitmap->createPlatformImage(WebCore::BackingStoreCopy::DontCopyBackingStore)))
                                backingStore.updateContents(tileUpdate.index, *image, tileUpdate.dirtyRect);
                        }
                    }
                }
            } else {
                layer->texmapLayer.setBackgroundColor(layerUpdate.background.color);
                layer->texmapLayer.setBackingStore(nullptr);
                layer->backingStore = nullptr;
            }
        }
        if (layerUpdate.changes & WCLayerChange::SolidColor)
            layer->texmapLayer.setSolidColor(layerUpdate.solidColor);
        if (layerUpdate.changes & WCLayerChange::ShowDebugBorder)
            layer->texmapLayer.setShowDebugBorder(layerUpdate.showDebugBorder);
        if (layerUpdate.changes & WCLayerChange::DebugBorderColor)
            layer->texmapLayer.setDebugBorderColor(layerUpdate.debugBorderColor);
        if (layerUpdate.changes & WCLayerChange::DebugBorderWidth)
            layer->texmapLayer.setDebugBorderWidth(layerUpdate.debugBorderWidth);
        if (layerUpdate.changes & WCLayerChange::ShowRepaintCounter)
            layer->texmapLayer.setShowRepaintCounter(layerUpdate.showRepaintCounter);
        if (layerUpdate.changes & WCLayerChange::RepaintCount)
            layer->texmapLayer.setRepaintCount(layerUpdate.repaintCount);
        if (layerUpdate.changes & WCLayerChange::Opacity)
            layer->texmapLayer.setOpacity(layerUpdate.opacity);
        if (layerUpdate.changes & WCLayerChange::Transform)
            layer->texmapLayer.setTransform(layerUpdate.transform);
        if (layerUpdate.changes & WCLayerChange::ChildrenTransform)
            layer->texmapLayer.setChildrenTransform(layerUpdate.childrenTransform);
        if (layerUpdate.changes & WCLayerChange::Filters)
            layer->texmapLayer.setFilters(layerUpdate.filters);
        if (layerUpdate.changes & WCLayerChange::BackdropFilters || layerUpdate.changes & WCLayerChange::BackdropFiltersRect) {
            if (!layer->backdropLayer) {
                layer->backdropLayer = makeUnique<WebCore::TextureMapperLayer>();
                layer->backdropLayer->setAnchorPoint({ });
                layer->backdropLayer->setContentsVisible(true);
                layer->backdropLayer->setMasksToBounds(true);
            }
        }
        if (layerUpdate.changes & WCLayerChange::BackdropFilters) {
            if (layerUpdate.backdropFilters.isEmpty())
                layer->texmapLayer.setBackdropLayer(nullptr);
            else {
                layer->backdropLayer->setFilters(layerUpdate.backdropFilters);
                layer->texmapLayer.setBackdropLayer(layer->backdropLayer.get());
            }
        }
        if (layerUpdate.changes & WCLayerChange::BackdropFiltersRect) {
            layer->backdropLayer->setSize(layerUpdate.backdropFiltersRect.rect().size());
            layer->backdropLayer->setPosition(layerUpdate.backdropFiltersRect.rect().location());
            layer->texmapLayer.setBackdropFiltersRect(layerUpdate.backdropFiltersRect);
        }
        if (layerUpdate.changes & WCLayerChange::PlatformLayer) {
            if (!layerUpdate.platformLayer.hasLayer) {
                if (layer->contentBuffer) {
                    layer->contentBuffer->setClient(nullptr);
                    layer->contentBuffer = nullptr;
                }
                layer->texmapLayer.setContentsLayer(nullptr);
                layer->videoFrame = nullptr;
#if OS(WINDOWS)
                layer->videoTextureLayer = nullptr;
                layer->shownTexture = 0;
                releaseVideoTexturesOfLayer(layerUpdate.id);
#endif
            } else if (layerUpdate.platformLayer.videoFrame) {
                if (RefPtr bitmap = layerUpdate.platformLayer.videoFrame->bitmap())
                    uploadVideoFrame(*layer, *bitmap);
            } else if (!layer->videoFrame) {
                WCContentBuffer* contentBuffer = nullptr;
                for (auto identifier : layerUpdate.platformLayer.identifiers)
                    contentBuffer = WCContentBufferManager::singleton().releaseContentBufferIdentifier(m_webProcessIdentifier, identifier);
                if (contentBuffer) {
                    if (layer->contentBuffer)
                        layer->contentBuffer->setClient(nullptr);
                    contentBuffer->setClient(layer);
                    layer->contentBuffer = contentBuffer;
                    layer->texmapLayer.setContentsLayer(contentBuffer->platformLayer());
                }
            }
        }
        if (layerUpdate.changes & WCLayerChange::RemoteFrame) {
            if (layerUpdate.hostIdentifier) {
                auto platformLayer = WCRemoteFrameHostLayerManager::singleton().acquireRemoteFrameHostLayer(*layerUpdate.hostIdentifier, m_webProcessIdentifier);
                layer->texmapLayer.setContentsLayer(platformLayer);
            } else {
                ASSERT(layer->hostIdentifier);
                WCRemoteFrameHostLayerManager::singleton().releaseRemoteFrameHostLayer(*layer->hostIdentifier);
            }
            layer->hostIdentifier = layerUpdate.hostIdentifier;
        }
    }

    for (auto id : update.removedLayers) {
        m_layers.remove(id);
#if OS(WINDOWS)
        m_pendingVideoFrames.remove(id);
        m_pendingVideoTextures.remove(id);
        releaseVideoTexturesOfLayer(id);
#endif
    }
#if OS(WINDOWS)
    if (!m_pendingVideoFrames.isEmpty()) {
        Vector<WebCore::PlatformLayerIdentifier> applied;
        for (auto& [id, bitmap] : m_pendingVideoFrames) {
            if (auto* layer = m_layers.get(id)) {
                uploadVideoFrame(*layer, *bitmap);
                applied.append(id);
            }
        }
        for (auto id : applied)
            m_pendingVideoFrames.remove(id);
    }
#endif

    auto rootLayer = &m_layers.get(*update.rootLayer)->texmapLayer;
    rootLayer->applyAnimationsRecursively(MonotonicTime::now());
#if OS(WINDOWS)
    m_lastRootLayer = update.rootLayer;
    m_lastViewport = update.viewport;
    m_lastUpdateWasOnScreen = !update.remoteContextHostedIdentifier && !m_usesOffscreenRendering;
    if (!m_pendingVideoTextures.isEmpty() && m_lastUpdateWasOnScreen) {
        Vector<WebCore::PlatformLayerIdentifier> applied;
        for (auto& [id, textureIdentifier] : m_pendingVideoTextures) {
            auto* layer = m_layers.get(id);
            if (!layer)
                continue;
            if (auto* texture = m_videoTextures.get(textureIdentifier))
                showVideoTexture(*layer, textureIdentifier, *texture);
            applied.append(id);
        }
        for (auto id : applied)
            m_pendingVideoTextures.remove(id);
    }
#endif

    WebCore::BitmapTexture* surface = nullptr;
    RefPtr<WebCore::BitmapTexture> texture;
    bool showFPS = true;
    bool readPixel = false;
    RefPtr<WebCore::ShareableBitmap> bitmap;

    if (update.remoteContextHostedIdentifier) {
        showFPS = false;
        texture = WebCore::BitmapTexturePool::singleton().acquireTexture(update.viewport, { WebCore::BitmapTexture::Flags::SupportsAlpha });
        surface = texture.get();
    } else if (m_usesOffscreenRendering) {
        readPixel = true;
        texture = WebCore::BitmapTexturePool::singleton().acquireTexture(update.viewport, { WebCore::BitmapTexture::Flags::SupportsAlpha });
        surface = texture.get();
    } else
        glViewport(0, 0, update.viewport.width(), update.viewport.height());

#if OS(WINDOWS)
    std::optional<GPURenderingStats::ScopedTimer> paintTimer;
    if (stats)
        paintTimer.emplace(&stats->paint);
#endif
    m_textureMapper->beginPainting(WebCore::TextureMapper::FlipY::No, surface);
    rootLayer->paint(*m_textureMapper);
    if (showFPS)
        m_fpsCounter.updateFPSAndDisplay(*m_textureMapper);
    if (readPixel) {
        bitmap = WebCore::ShareableBitmap::create({ update.viewport });
        glReadPixels(0, 0, update.viewport.width(), update.viewport.height(), GL_BGRA, GL_UNSIGNED_BYTE, bitmap->mutableSpan().data());
    }
    m_textureMapper->endPainting();
#if OS(WINDOWS)
    paintTimer.reset();
#endif

    std::optional<UpdateInfo> result;
    if (update.remoteContextHostedIdentifier)
        WCRemoteFrameHostLayerManager::singleton().updateTexture(*update.remoteContextHostedIdentifier, m_webProcessIdentifier, WTF::move(texture));
    else if (m_usesOffscreenRendering) {
        if (auto handle = bitmap->createHandle()) {
            result.emplace();
            result->viewSize = update.viewport;
            result->deviceScaleFactor = 1;
            result->updateScaleFactor = 1;
            WebCore::IntRect viewport = { { }, update.viewport };
            result->updateRectBounds = viewport;
            result->updateRects.append(viewport);
            result->bitmapHandle = WTF::move(*handle);
        }
    } else
        m_needsPresent = true;
    return result;
}

void WCScene::uploadVideoFrame(Layer& layer, const WebCore::ShareableBitmap& bitmap)
{
    RefPtr image = WebCore::NativeImage::create(const_cast<WebCore::ShareableBitmap&>(bitmap).createPlatformImage(WebCore::BackingStoreCopy::DontCopyBackingStore));
    if (!image)
        return;
#if OS(WINDOWS)
    auto* stats = GPURenderingStats::singletonIfEnabled();
    GPURenderingStats::ScopedTimer timer(stats ? &stats->videoUpload : nullptr);
#endif
    if (!layer.videoFrame)
        layer.videoFrame = makeUnique<WebCore::TextureMapperSparseBackingStore>();
    auto size = bitmap.size();
    layer.videoFrame->setSize(size);
    layer.videoFrame->updateContents({ }, *image, { { }, size });
    layer.texmapLayer.setContentsLayer(layer.videoFrame.get());
#if OS(WINDOWS)
    // Back from GPU textures to frames in memory: give the texture shown until now back to the web process.
    if (layer.shownTexture) {
        if (auto* texture = m_videoTextures.get(layer.shownTexture); texture && texture->held) {
            texture->mutex->ReleaseSync(0);
            texture->held = false;
        }
        layer.shownTexture = 0;
    }
#endif
}

#if OS(WINDOWS)
bool WCScene::updateVideoFrame(WebCore::PlatformLayerIdentifier identifier, const WebCore::ShareableBitmap& bitmap)
{
    auto* layer = m_layers.get(identifier);
    if (!layer) {
        if (m_pendingVideoFrames.size() >= 8)
            m_pendingVideoFrames.clear();
        m_pendingVideoFrames.set(identifier, RefPtr { const_cast<WebCore::ShareableBitmap*>(&bitmap) });
        return false;
    }
    if (!m_lastUpdateWasOnScreen || !m_lastRootLayer || !m_context->makeContextCurrent())
        return false;
    {
        auto* stats = GPURenderingStats::singletonIfEnabled();
        GPURenderingStats::ScopedTimer compositeTimer(stats ? &stats->composite : nullptr);
        m_textureMapper->releaseUnusedTexturesNow();
        uploadVideoFrame(*layer, bitmap);
    }
    return repaintForVideo();
}

bool WCScene::importVideoTexture(WebCore::PlatformLayerIdentifier layerIdentifier, uint64_t textureIdentifier, const WebCore::IntSize&, WTF::Win32Handle&& handle)
{
    if (!m_context->makeContextCurrent())
        return false;
    auto* device = angleD3D11Device();
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
        return false;
    auto videoTexture = makeUnique<VideoTexture>(layerIdentifier);
    if (FAILED(device1->OpenSharedResource1(handle.get(), IID_PPV_ARGS(&videoTexture->texture))) || FAILED(videoTexture->texture.As(&videoTexture->mutex)))
        return false;

    static auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    static auto imageTargetTexture = reinterpret_cast<void (*)(GLenum, void*)>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage || !imageTargetTexture)
        return false;
    videoTexture->display = eglGetCurrentDisplay();
    const EGLint attributes[] = { EGL_NONE };
    videoTexture->image = createImage(videoTexture->display, EGL_NO_CONTEXT, EGL_D3D11_TEXTURE_ANGLE, static_cast<EGLClientBuffer>(videoTexture->texture.Get()), attributes);
    if (videoTexture->image == EGL_NO_IMAGE_KHR)
        return false;
    glGenTextures(1, &videoTexture->glTexture);
    glBindTexture(GL_TEXTURE_2D, videoTexture->glTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    imageTargetTexture(GL_TEXTURE_2D, videoTexture->image);
    glBindTexture(GL_TEXTURE_2D, 0);

    // A new set (a new size, say) replaces this layer's older ones, except the one it still shows.
    uint64_t generation = textureIdentifier >> 2;
    m_videoTextures.removeIf([&](auto& entry) {
        return entry.value->layer == layerIdentifier && (entry.key >> 2) < generation && !entry.value->held;
    });
    m_videoTextures.set(textureIdentifier, WTF::move(videoTexture));
    return true;
}

bool WCScene::updateVideoTexture(WebCore::PlatformLayerIdentifier layerIdentifier, uint64_t textureIdentifier)
{
    auto* stats = GPURenderingStats::singletonIfEnabled();
    auto* texture = m_videoTextures.get(textureIdentifier);
    if (!texture) {
        if (stats)
            ++stats->texturesMissing;
        return false;
    }
    auto* layer = m_layers.get(layerIdentifier);
    if (!layer || !m_lastUpdateWasOnScreen || !m_lastRootLayer || !m_context->makeContextCurrent()) {
        if (stats)
            ++stats->texturesNotShown;
        // A paused video's frame can come before the rendering update that adds its new layer (a page's layers rebuilt on
        // returning to its tab, full screen): handing it back left the layer empty, white, as a paused video sends no
        // other. Keep the newest such frame per layer and show it once the layer is there (see update()).
        if (!layer) {
            auto previous = m_pendingVideoTextures.get(layerIdentifier);
            if (previous && previous != textureIdentifier)
                skipVideoTexture(previous);
            m_pendingVideoTextures.set(layerIdentifier, textureIdentifier);
            return false;
        }
        skipVideoTexture(textureIdentifier);
        return false;
    }
    if (!showVideoTexture(*layer, textureIdentifier, *texture))
        return false;
    return repaintForVideo();
}

bool WCScene::showVideoTexture(Layer& layer, uint64_t textureIdentifier, VideoTexture& videoTexture)
{
    auto* stats = GPURenderingStats::singletonIfEnabled();
    auto* texture = &videoTexture;
    if (texture->mutex->AcquireSync(1, 50) != S_OK) {
        if (stats)
            ++stats->textureAcquireTimeouts;
        return false;
    }
    if (stats)
        ++stats->texturesShown;
    texture->held = true;
    // Give back the texture shown until now: the web process can write into it again.
    if (layer.shownTexture && layer.shownTexture != textureIdentifier) {
        if (auto* previous = m_videoTextures.get(layer.shownTexture); previous && previous->held) {
            previous->mutex->ReleaseSync(0);
            previous->held = false;
        }
    }
    layer.shownTexture = textureIdentifier;
    layer.videoFrame = nullptr;
    if (!layer.videoTextureLayer)
        layer.videoTextureLayer = makeUnique<WCVideoTextureLayer>();
    layer.videoTextureLayer->setTexture(texture->glTexture);
    layer.texmapLayer.setContentsLayer(layer.videoTextureLayer.get());
    return true;
}

void WCScene::skipVideoTexture(uint64_t textureIdentifier)
{
    auto* texture = m_videoTextures.get(textureIdentifier);
    if (!texture || texture->held)
        return;
    if (auto* stats = GPURenderingStats::singletonIfEnabled())
        ++stats->texturesSkipped;
    if (texture->mutex->AcquireSync(1, 0) == S_OK)
        texture->mutex->ReleaseSync(0);
}

void WCScene::releaseVideoTexturesOfLayer(WebCore::PlatformLayerIdentifier layerIdentifier)
{
    m_pendingVideoTextures.remove(layerIdentifier);
    if (m_videoTextures.isEmpty() || !m_context->makeContextCurrent())
        return;
    m_videoTextures.removeIf([&](auto& entry) {
        if (entry.value->layer != layerIdentifier)
            return false;
        if (auto* stats = GPURenderingStats::singletonIfEnabled())
            ++stats->texturesReleasedWithLayer;
        return true;
    });
}

// Repaints the scene as the last update left it (with a layer's new video frame, or into resized buffers), for
// present() to show.
bool WCScene::repaintForVideo()
{
    if (!m_lastUpdateWasOnScreen || !m_lastRootLayer)
        return false;
    auto* root = m_layers.get(*m_lastRootLayer);
    if (!root || !m_context->makeContextCurrent())
        return false;
    auto* stats = GPURenderingStats::singletonIfEnabled();
    GPURenderingStats::ScopedTimer compositeTimer(stats ? &stats->composite : nullptr);

    auto& rootLayer = root->texmapLayer;
    rootLayer.applyAnimationsRecursively(MonotonicTime::now());
    glViewport(0, 0, m_lastViewport.width(), m_lastViewport.height());
    {
        std::optional<GPURenderingStats::ScopedTimer> paintTimer;
        if (stats)
            paintTimer.emplace(&stats->paint);
        m_textureMapper->beginPainting(WebCore::TextureMapper::FlipY::No, nullptr);
        rootLayer.paint(*m_textureMapper);
        m_fpsCounter.updateFPSAndDisplay(*m_textureMapper);
        m_textureMapper->endPainting();
    }
    m_needsPresent = true;
    return true;
}
#endif

void WCScene::present()
{
    if (!m_needsPresent)
        return;
    m_needsPresent = false;
#if OS(WINDOWS)
    auto* stats = GPURenderingStats::singletonIfEnabled();
    GPURenderingStats::ScopedTimer presentTimer(stats ? &stats->present : nullptr);
#endif
    if (!m_context->makeContextCurrent())
        return;
#if OS(WINDOWS)
    auto sizeBefore = m_context->surfaceSize();
#endif
    m_context->swapBuffers();
#if OS(WINDOWS)
    // ANGLE resizes the window's buffers to the window only after presenting, so the first frame after a resize
    // went to buffers of the old size, and the window shows nothing (or that frame stretched) until the next present.
    // A static page doesn't present again: its view stayed gray when shown again (the browser sizes a hidden view to
    // 0x0) or after a resize (the find bar). Paint the scene again into the resized buffers and present it.
    auto sizeAfter = m_context->surfaceSize();
    if (sizeAfter != sizeBefore && !sizeAfter.isEmpty() && repaintForVideo()) {
        m_needsPresent = false;
        m_context->swapBuffers();
        if (stats)
            ++stats->resizePresents;
    }
    if (stats)
        stats->reportIfDue(); // Video frames are shown without rendering updates, which report otherwise.
#endif
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
