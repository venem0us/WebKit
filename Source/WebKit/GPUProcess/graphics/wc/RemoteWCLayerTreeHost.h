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

#include "Connection.h"
#include "MessageReceiver.h"
#include "MessageSender.h"
#include "WCBackingStore.h"
#include "WCLayerTreeHostIdentifier.h"
#include "WCSharedSceneContextHolder.h"
#include <WebCore/PlatformLayerIdentifier.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/RefCounted.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#if OS(WINDOWS)
#include <wtf/win/Win32Handle.h>
#endif
#include <wtf/TZoneMalloc.h>

namespace IPC {
class StreamConnectionWorkQueue;
}

namespace WebKit {
class GPUConnectionToWebProcess;
class WCScene;
struct UpdateInfo;
struct WCUpdateInfo;

class RemoteWCLayerTreeHost : public IPC::MessageReceiver, private IPC::MessageSender, public RefCounted<RemoteWCLayerTreeHost> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteWCLayerTreeHost);
public:
    static Ref<RemoteWCLayerTreeHost> create(GPUConnectionToWebProcess&, WebKit::WCLayerTreeHostIdentifier, uint64_t nativeWindow, bool usesOffscreenRendering);
    ~RemoteWCLayerTreeHost();
    // message handlers
    void update(WCUpdateInfo&&, CompletionHandler<void(std::optional<WebKit::UpdateInfo>)>&&);
#if OS(WINDOWS)
    void updateVideoFrame(WebCore::PlatformLayerIdentifier, WCBackingStore&&);
    void importVideoTexture(WebCore::PlatformLayerIdentifier, uint64_t texture, WebCore::IntSize, WTF::Win32Handle&&, CompletionHandler<void(bool)>&&);
    void updateVideoTexture(WebCore::PlatformLayerIdentifier, uint64_t texture);
    void setFrozen(bool);
#endif

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    RemoteWCLayerTreeHost(GPUConnectionToWebProcess&, WebKit::WCLayerTreeHostIdentifier, uint64_t nativeWindow, bool usesOffscreenRendering);

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;
    // IPC::MessageSender
    IPC::Connection* messageSenderConnection() const override;
    uint64_t messageSenderDestinationID() const override;

    WeakPtr<GPUConnectionToWebProcess> m_connectionToWebProcess;
    WebCore::ProcessIdentifier m_webProcessIdentifier;
    WCLayerTreeHostIdentifier m_identifier;
    RefPtr<WCSharedSceneContextHolder::Holder> m_sharedSceneContextHolder;
    std::unique_ptr<WCScene> m_scene;
#if OS(WINDOWS)
    // Video frames sent from the web process's video sink threads, latest per layer: the work queue shows the newest
    // and drops any it didn't get to. Shared with the work queue, which outlives a message.
    // GPU textures (see WCScene::importVideoTexture) work the same way: imports always go first; of a layer's texture
    // frames only the newest is shown, the others are handed straight back to the web process.
    struct TextureImport {
        WebCore::PlatformLayerIdentifier layer;
        uint64_t texture;
        WebCore::IntSize size;
        WTF::Win32Handle handle;
        CompletionHandler<void(bool)> completionHandler;
    };
    struct VideoFrameMailbox : ThreadSafeRefCounted<VideoFrameMailbox> {
        Lock lock;
        HashMap<WebCore::PlatformLayerIdentifier, RefPtr<WebCore::ShareableBitmap>> frames WTF_GUARDED_BY_LOCK(lock);
        Vector<TextureImport> textureImports WTF_GUARDED_BY_LOCK(lock);
        HashMap<WebCore::PlatformLayerIdentifier, Vector<uint64_t>> textureFrames WTF_GUARDED_BY_LOCK(lock);
        bool drainScheduled WTF_GUARDED_BY_LOCK(lock) { false };
    };
    void scheduleVideoFrameDrain() WTF_REQUIRES_LOCK(m_videoFrames->lock);
    Ref<VideoFrameMailbox> m_videoFrames { adoptRef(*new VideoFrameMailbox) };

    // Presents the scene again shortly after an update sent while the page's window was hidden or just shown (see
    // update()). The timers hold this state, not the host: the host clears it when destroyed.
    struct ShownPresents : RefCounted<ShownPresents> {
        RemoteWCLayerTreeHost* host { nullptr };
        bool scheduled { false };
        // The page's layer tree is frozen (swapped out or suspended): its window may show another page now.
        bool frozen { false };
    };
    void scheduleShownPresents();
    uint64_t m_nativeWindow { 0 };
    bool m_windowWasVisible { false };
    MonotonicTime m_lastUpdateTime;
    Ref<ShownPresents> m_shownPresents { adoptRef(*new ShownPresents) };
#endif
};

IPC::StreamConnectionWorkQueue& remoteGraphicsStreamWorkQueue();

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
