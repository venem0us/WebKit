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
#if ENABLE(VIDEO) && OS(WINDOWS)
#include <array>
#include <atomic>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <memory>
#include <wrl/client.h>
#include <wtf/win/Win32Handle.h>
#endif

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
#if OS(WINDOWS)
    if (m_gpuProcessDidCloseHandler)
        m_gpuProcessDidCloseHandler(previousConnection.identifier());
#endif
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

    // Frames on the GPU: a GPU copy into one of three shared textures, which the GPU process opens once each (their
    // NT handles go over the first time) and draws from. Each texture's keyed mutex hands it between the two devices:
    // key 0 when it's ours to write, key 1 when the GPU process may read it. If the GPU process can't open them (another
    // adapter, say) frames go back to pushFrame() for good.
    bool pushD3D11Frame(void* texture, unsigned subresource, const WebCore::IntSize& size) final
    {
        if (m_texturesUnusable || size.isEmpty())
            return false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
        if (FAILED(static_cast<IUnknown*>(texture)->QueryInterface(IID_PPV_ARGS(&source))))
            return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        source->GetDevice(&device);
        if (device != m_device || size != m_size) {
            if (!createSharedTextures(device.Get(), size))
                return false;
        }
        auto& slot = m_textures[m_nextSlot];
        // The GPU process hands a texture back (key 0) when it shows a newer one; wait a little for that, or skip.
        if (slot.mutex->AcquireSync(0, 20) != S_OK)
            return false;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        D3D11_BOX box { 0, 0, 0, static_cast<UINT>(size.width()), static_cast<UINT>(size.height()), 1 };
        context->CopySubresourceRegion(slot.texture.Get(), 0, 0, 0, 0, source.Get(), subresource, &box);
        slot.mutex->ReleaseSync(1);
        m_nextSlot = (m_nextSlot + 1) % m_textures.size();

        if (!slot.imported) {
            Microsoft::WRL::ComPtr<IDXGIResource1> resource;
            HANDLE handle = nullptr;
            if (FAILED(slot.texture.As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
                m_texturesUnusable = true;
                return false;
            }
            slot.imported = true;
            m_connection->sendWithAsyncReply(Messages::RemoteWCLayerTreeHost::ImportVideoTexture(m_layer, slot.id, size, WTF::Win32Handle::adopt(handle)), [weakUnusable = m_unusableFlag](bool imported) {
                if (!imported)
                    *weakUnusable = true;
            }, m_destinationID);
        }
        if (*m_unusableFlag) {
            m_texturesUnusable = true;
            return false;
        }
        return m_connection->send(Messages::RemoteWCLayerTreeHost::UpdateVideoTexture(m_layer, slot.id), m_destinationID) == IPC::Error::NoError;
    }

private:
    struct SharedTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
        uint64_t id { 0 };
        bool imported { false };
    };

    bool createSharedTextures(ID3D11Device* device, const WebCore::IntSize& size)
    {
        m_device = device;
        m_size = size;
        m_nextSlot = 0;
        // Ids are unique in the process: the GPU process keeps a page's textures by id, and a page can have several
        // senders (one per video, and a new one whenever a video moves to another layer).
        static std::atomic<uint64_t> lastGeneration { 0 };
        m_generation = ++lastGeneration;
        D3D11_TEXTURE2D_DESC desc { };
        desc.Width = size.width();
        desc.Height = size.height();
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        for (unsigned i = 0; i < m_textures.size(); ++i) {
            auto& slot = m_textures[i];
            slot = { };
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &slot.texture)) || FAILED(slot.texture.As(&slot.mutex))) {
                m_texturesUnusable = true;
                return false;
            }
            // Texture ids: the generation (a new set for each device or size) above the slot.
            slot.id = (m_generation << 2) | i;
        }
        return true;
    }

    const Ref<IPC::Connection> m_connection;
    const uint64_t m_destinationID;
    const WebCore::PlatformLayerIdentifier m_layer;
    // Used on the player's sink thread only, except the flag the GPU process's import replies set (main thread).
    std::array<SharedTexture, 3> m_textures;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    WebCore::IntSize m_size;
    unsigned m_nextSlot { 0 };
    uint64_t m_generation { 0 };
    bool m_texturesUnusable { false };
    std::shared_ptr<std::atomic<bool>> m_unusableFlag { std::make_shared<std::atomic<bool>>(false) };
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
