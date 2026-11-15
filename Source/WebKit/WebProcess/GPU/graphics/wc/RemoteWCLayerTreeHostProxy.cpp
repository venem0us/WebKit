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
#include "RemoteWCLayerTreeHostProxy.h"

#if USE(GRAPHICS_LAYER_WC)

#include "GPUConnectionToWebProcess.h"
#include "MessageSenderInlines.h"
#include "RemoteWCLayerTreeHostMessages.h"
#include "WCUpdateInfo.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteWCLayerTreeHostProxy);

RemoteWCLayerTreeHostProxy::RemoteWCLayerTreeHostProxy(WebPage& page, bool usesOffscreenRendering)
    : m_page(page)
    , m_usesOffscreenRendering(usesOffscreenRendering)
{
}

RemoteWCLayerTreeHostProxy::~RemoteWCLayerTreeHostProxy()
{
    disconnectGpuProcessIfNeeded();
}

IPC::Connection* RemoteWCLayerTreeHostProxy::messageSenderConnection() const
{
    return &const_cast<RemoteWCLayerTreeHostProxy&>(*this).ensureGPUProcessConnection().connection();
}

GPUProcessConnection& RemoteWCLayerTreeHostProxy::ensureGPUProcessConnection()
{
    if (!m_gpuProcessConnection.get()) {
        Ref gpuProcessConnection = WebProcess::singleton().ensureGPUProcessConnection();
        m_gpuProcessConnection = gpuProcessConnection.get();
        gpuProcessConnection->addClient(*this);
        gpuProcessConnection->connection().send(
            Messages::GPUConnectionToWebProcess::CreateWCLayerTreeHost(wcLayerTreeHostIdentifier(), m_page->nativeWindowHandle(), m_usesOffscreenRendering),
            0, IPC::SendOption::DispatchMessageEvenWhenWaitingForSyncReply);
    }
    ASSERT(m_gpuProcessConnection.get() == &WebProcess::singleton().ensureGPUProcessConnection());
    return WebProcess::singleton().ensureGPUProcessConnection();
}

void RemoteWCLayerTreeHostProxy::disconnectGpuProcessIfNeeded()
{
    if (auto gpuProcessConnection = std::exchange(m_gpuProcessConnection, nullptr).get()) {
        gpuProcessConnection->connection().send(Messages::GPUConnectionToWebProcess::ReleaseWCLayerTreeHost(wcLayerTreeHostIdentifier()), 0, IPC::SendOption::DispatchMessageEvenWhenWaitingForSyncReply);
    }
}

void RemoteWCLayerTreeHostProxy::gpuProcessConnectionDidClose(GPUProcessConnection& previousConnection)
{
    m_gpuProcessConnection = nullptr;
}

uint64_t RemoteWCLayerTreeHostProxy::messageSenderDestinationID() const
{
    return wcLayerTreeHostIdentifier().toUInt64();
}

void RemoteWCLayerTreeHostProxy::update(WCUpdateInfo&& updateInfo, CompletionHandler<void(std::optional<WebKit::UpdateInfo>)>&& completionHandler)
{
    sendWithAsyncReply(Messages::RemoteWCLayerTreeHost::Update(updateInfo), WTF::move(completionHandler));
}

#if ENABLE(VIDEO) && OS(WINDOWS)
namespace {

// Copies each frame into shared memory and sends it to the GPU process's scene for this layer tree, on the video
// player's thread. A frame the GPU process isn't ready for (or a lost connection) is dropped there or fails here, and
// the player falls back to showing frames through rendering updates.
class WCVideoFrameSender final : public WebCore::MediaPlayerCompositorFrameSink {
public:
    WCVideoFrameSender(Ref<IPC::Connection>&& connection, uint64_t destinationID, WebCore::PlatformLayerIdentifier layer)
        : m_connection(WTF::move(connection))
        , m_destinationID(destinationID)
        , m_layer(layer)
    {
    }

    bool pushFrame(std::span<const uint8_t> pixels, const WebCore::IntSize& size, unsigned stride, bool opaque) final
    {
        if (size.isEmpty() || stride < static_cast<unsigned>(size.width()) * 4 || pixels.size() < static_cast<size_t>(stride) * size.height())
            return false;
        // N32 premultiplied, which is BGRA in memory on Windows: the frame's rows copy straight in.
        RefPtr bitmap = WebCore::ShareableBitmap::create({ size, WebCore::ColorSpace::SRGB(), WebCore::PixelFormat::BGRA8, WebCore::Headroom::None, opaque });
        if (!bitmap)
            return false;
        auto destination = bitmap->mutableSpan();
        size_t rowBytes = static_cast<size_t>(size.width()) * 4;
        size_t destinationStride = bitmap->bytesPerRow();
        for (int y = 0; y < size.height(); ++y) {
            auto source = pixels.subspan(static_cast<size_t>(y) * stride, rowBytes);
            auto row = destination.subspan(static_cast<size_t>(y) * destinationStride, rowBytes);
            memcpySpan(row, source);
            if (opaque) {
                // BGRx: the fourth byte is padding, not alpha.
                for (size_t x = 3; x < rowBytes; x += 4)
                    row[x] = 0xff;
            }
        }
        WCBackingStore store;
        store.setBitmap(WTF::move(bitmap));
        return m_connection->send(Messages::RemoteWCLayerTreeHost::UpdateVideoFrame(m_layer, store), m_destinationID) == IPC::Error::NoError;
    }

private:
    const Ref<IPC::Connection> m_connection;
    const uint64_t m_destinationID;
    const WebCore::PlatformLayerIdentifier m_layer;
};

} // namespace

RefPtr<WebCore::MediaPlayerCompositorFrameSink> RemoteWCLayerTreeHostProxy::createVideoFrameSink(WebCore::PlatformLayerIdentifier layer)
{
    // WEBKIT_GST_DIRECT_VIDEO_FRAMES=0: frames go with rendering updates, as before.
    static const bool disabled = [] {
        auto* value = std::getenv("WEBKIT_GST_DIRECT_VIDEO_FRAMES");
        return value && *value == '0';
    }();
    if (disabled || m_usesOffscreenRendering)
        return nullptr;
    return adoptRef(*new WCVideoFrameSender(Ref { ensureGPUProcessConnection().connection() }, wcLayerTreeHostIdentifier().toUInt64(), layer));
}
#endif

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
