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

#pragma once

#if USE(GRAPHICS_LAYER_WC)

#include "UpdateInfo.h"
#include <WebCore/GraphicsLayer.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/TextureMapperFPSCounter.h>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#if OS(WINDOWS)
#include <wtf/win/Win32Handle.h>
#endif

namespace WebCore {
class ShareableBitmap;
class TextureMapper;
class TextureMapperLayer;
class TextureMapperPlatformLayer;
class TextureMapperTiledBackingStore;
}

namespace WebKit {

class WCSceneContext;
struct WCUpdateInfo;

class WCScene {
    WTF_MAKE_TZONE_ALLOCATED(WCScene);
public:
    WCScene(WebCore::ProcessIdentifier, bool usesOffscreenRendering);
    ~WCScene();
    void initialize(WCSceneContext&);
    std::optional<UpdateInfo> update(WCUpdateInfo&&);
    // Presents what the last update() painted to the window, if anything. Called after the web process has
    // been answered, so a present that waits for the display doesn't hold up its next rendering update.
    void present();
#if OS(WINDOWS)
    // A video frame sent straight from the web process's video sink thread (not with a rendering update): uploads it
    // into the layer's video contents and repaints the scene as the last update left it, for present() to show.
    // Only for scenes painted to the window; returns false (the frame is dropped) otherwise.
    bool updateVideoFrame(WebCore::PlatformLayerIdentifier, const WebCore::ShareableBitmap&);
    // Video frames that stay on the GPU: the web process's shared Direct3D 11 textures, opened once each on ANGLE's
    // device (importVideoTexture), then shown frame by frame (updateVideoTexture: the layer draws the texture, repainted
    // like updateVideoFrame) or handed straight back unshown (skipVideoTexture). Each texture's keyed mutex passes it
    // between the processes: the scene holds the texture a layer shows (key 1) and gives it back (key 0) for the next.
    bool importVideoTexture(WebCore::PlatformLayerIdentifier, uint64_t texture, const WebCore::IntSize&, WTF::Win32Handle&&);
    bool updateVideoTexture(WebCore::PlatformLayerIdentifier, uint64_t texture);
    void skipVideoTexture(uint64_t texture);
#endif

private:
    struct Layer;
    using LayerMap = HashMap<WebCore::PlatformLayerIdentifier, std::unique_ptr<Layer>>;
    void uploadVideoFrame(Layer&, const WebCore::ShareableBitmap&);
#if OS(WINDOWS)
    WebCore::IntSize m_lastViewport;
    Markable<WebCore::PlatformLayerIdentifier> m_lastRootLayer;
    bool m_lastUpdateWasOnScreen { false };
    // Frames for layers the scene doesn't have yet (a paused video's first frame can arrive before the rendering
    // update that adds its layer): applied when the layer arrives.
    HashMap<WebCore::PlatformLayerIdentifier, RefPtr<WebCore::ShareableBitmap>> m_pendingVideoFrames;
    struct VideoTexture;
    HashMap<uint64_t, std::unique_ptr<VideoTexture>> m_videoTextures;
    // Like m_pendingVideoFrames, for frames left on the GPU: the newest texture for each layer the scene doesn't have yet.
    HashMap<WebCore::PlatformLayerIdentifier, uint64_t> m_pendingVideoTextures;
    bool showVideoTexture(Layer&, uint64_t texture, VideoTexture&);
    bool repaintForVideo();
    void releaseVideoTexturesOfLayer(WebCore::PlatformLayerIdentifier);
#endif

    WebCore::ProcessIdentifier m_webProcessIdentifier;
    WCSceneContext* m_context { nullptr };
    std::unique_ptr<WebCore::TextureMapper> m_textureMapper;
    WebCore::TextureMapperFPSCounter m_fpsCounter;
    LayerMap m_layers;
    bool m_usesOffscreenRendering;
    bool m_needsPresent { false };
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
