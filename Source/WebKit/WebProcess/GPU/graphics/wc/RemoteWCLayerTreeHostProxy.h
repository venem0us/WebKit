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

#include "GPUProcessConnection.h"
#include "MessageReceiver.h"
#include "MessageSender.h"
#include "UpdateInfo.h"
#include "WCLayerTreeHostIdentifier.h"
#include <WebCore/MediaPlayerCompositorFrameSink.h>
#include <WebCore/PlatformLayerIdentifier.h>
#if OS(WINDOWS)
#include <wtf/win/Win32Handle.h>
#endif
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakRef.h>

namespace WebKit {

struct WCUpdateInfo;

class RemoteWCLayerTreeHostProxy
    : private IPC::MessageSender
    , private GPUProcessConnection::Client
    , public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<RemoteWCLayerTreeHostProxy> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteWCLayerTreeHostProxy);
public:
    RemoteWCLayerTreeHostProxy(WebPage&, bool usesOffscreenRendering);
    ~RemoteWCLayerTreeHostProxy();

    void update(WCUpdateInfo&&, CompletionHandler<void(std::optional<WebKit::UpdateInfo>)>&&);
#if ENABLE(VIDEO) && OS(WINDOWS)
    // A sink a video player's thread sends the frames of the video in `layer` to, straight to the GPU process
    // (UpdateVideoFrame), or null where that's not possible (offscreen rendering, or turned off).
    RefPtr<WebCore::MediaPlayerCompositorFrameSink> createVideoFrameSink(WebCore::PlatformLayerIdentifier layer);
#endif
#if OS(WINDOWS)
    // Glass: called (on the main thread) when the GPU process this layer tree was in has gone. The next update starts a
    // new one, with an empty scene. With the identifier of the connection that closed.
    void setGPUProcessDidCloseHandler(Function<void(GPUProcessConnectionIdentifier)>&& handler) { m_gpuProcessDidCloseHandler = WTF::move(handler); }
#endif

    WTF_ABSTRACT_THREAD_SAFE_REF_COUNTED_AND_CAN_MAKE_WEAK_PTR_IMPL;

private:
    WCLayerTreeHostIdentifier wcLayerTreeHostIdentifier() const { return m_wcLayerTreeHostIdentifier; };
    GPUProcessConnection& ensureGPUProcessConnection();
    void disconnectGpuProcessIfNeeded();

    // GPUProcessConnection::Client
    void gpuProcessConnectionDidClose(GPUProcessConnection&) final;

    // IPC::MessageSender
    IPC::Connection* messageSenderConnection() const override;
    uint64_t messageSenderDestinationID() const override;

    ThreadSafeWeakPtr<GPUProcessConnection> m_gpuProcessConnection;
    WCLayerTreeHostIdentifier m_wcLayerTreeHostIdentifier { WCLayerTreeHostIdentifier::generate() };
    WeakRef<WebPage> m_page;
    bool m_usesOffscreenRendering { false };
#if OS(WINDOWS)
    Function<void(GPUProcessConnectionIdentifier)> m_gpuProcessDidCloseHandler;
#endif
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
