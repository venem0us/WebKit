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
#include "RemoteWCLayerTreeHost.h"

#if USE(GRAPHICS_LAYER_WC)

#include "GPUConnectionToWebProcess.h"
#include "GPUProcess.h"
#include "GPURenderingStats.h"
#include "RemoteGraphicsContextGL.h"
#include "RemoteWCLayerTreeHostMessages.h"
#include "StreamConnectionWorkQueue.h"
#include "WCScene.h"
#include "WCUpdateInfo.h"
#include <WebCore/ProcessIdentifier.h>
#include <wtf/TZoneMallocInlines.h>
#if OS(WINDOWS)
#include <windows.h>
#endif

namespace WebKit {

IPC::StreamConnectionWorkQueue& remoteGraphicsStreamWorkQueue()
{
#if ENABLE(WEBGL)
    return remoteGraphicsContextGLStreamWorkQueueSingleton();
#else
    // LazyNeverDestroyed owns the initial ref.
    static NeverDestroyed<IPC::StreamConnectionWorkQueue> instance { "RemoteWCLayerTreeHost work queue"_s };
    return instance.get();
#endif
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteWCLayerTreeHost);

Ref<RemoteWCLayerTreeHost> RemoteWCLayerTreeHost::create(GPUConnectionToWebProcess& connectionToWebProcess, WebKit::WCLayerTreeHostIdentifier identifier, uint64_t nativeWindow, bool usesOffscreenRendering)
{
    return adoptRef(*new RemoteWCLayerTreeHost(connectionToWebProcess, identifier, nativeWindow, usesOffscreenRendering));
}

RemoteWCLayerTreeHost::RemoteWCLayerTreeHost(GPUConnectionToWebProcess& connectionToWebProcess, WebKit::WCLayerTreeHostIdentifier identifier, uint64_t nativeWindow, bool usesOffscreenRendering)
    : m_connectionToWebProcess(connectionToWebProcess)
    , m_webProcessIdentifier(connectionToWebProcess.webProcessIdentifier())
    , m_identifier(identifier)
    , m_sharedSceneContextHolder(connectionToWebProcess.gpuProcess().sharedSceneContext().ensureHolderForWindow(nativeWindow))
{
    m_connectionToWebProcess->messageReceiverMap().addMessageReceiver(Messages::RemoteWCLayerTreeHost::messageReceiverName(), m_identifier.toUInt64(), *this);
    m_scene = makeUnique<WCScene>(m_webProcessIdentifier, usesOffscreenRendering);
#if OS(WINDOWS)
    m_nativeWindow = nativeWindow;
    m_shownPresents->host = this;
#endif

    remoteGraphicsStreamWorkQueue().dispatch([scene = m_scene.get(), sceneContextHolder = m_sharedSceneContextHolder.get(), nativeWindow] {
        if (!sceneContextHolder->context)
            sceneContextHolder->context.emplace(nativeWindow);
        scene->initialize(*sceneContextHolder->context);
    });
}

RemoteWCLayerTreeHost::~RemoteWCLayerTreeHost()
{
    ASSERT(m_connectionToWebProcess);
#if OS(WINDOWS)
    m_shownPresents->host = nullptr;
#endif
    m_connectionToWebProcess->messageReceiverMap().removeMessageReceiver(Messages::RemoteWCLayerTreeHost::messageReceiverName(), m_identifier.toUInt64());
    auto sceneContextHolder = m_connectionToWebProcess->gpuProcess().sharedSceneContext().removeHolder(m_sharedSceneContextHolder.releaseNonNull());

    remoteGraphicsStreamWorkQueue().dispatch([sceneContextHolder = WTF::move(sceneContextHolder), scene = WTF::move(m_scene)]() mutable {
        // Destroy scene on the StreamWorkQueue thread.
        scene = nullptr;
        // sceneContextHolder can be destroyed on the StreamWorkQueue thread because it hasOneRef.
    });
}

IPC::Connection* RemoteWCLayerTreeHost::messageSenderConnection() const
{
    return &m_connectionToWebProcess->connection();
}

uint64_t RemoteWCLayerTreeHost::messageSenderDestinationID() const
{
    return m_identifier.toUInt64();
}

void RemoteWCLayerTreeHost::update(WCUpdateInfo&& update, CompletionHandler<void(std::optional<WebKit::UpdateInfo>)>&& completionHandler)
{
#if OS(WINDOWS)
    // ANGLE presents into the page's window with a blt-model swap chain: the frame is copied into the browser window's
    // surface, which the browser (another process) redraws right after showing a page (a tab switch, a split pane). A
    // frame presented just before can be painted over, and a page that doesn't change presents nothing more: it stayed
    // blank until clicked. So after an update sent while the window was hidden or just shown, or the first after a quiet
    // spell (a page shown again may have sent nothing while hidden), present again a little later, once the browser has
    // drawn.
    bool visible = IsWindowVisible(reinterpret_cast<HWND>(m_nativeWindow));
    auto now = MonotonicTime::now();
    if (!visible || !m_windowWasVisible || now - m_lastUpdateTime > 250_ms)
        scheduleShownPresents();
    m_windowWasVisible = visible;
    m_lastUpdateTime = now;
#endif
    remoteGraphicsStreamWorkQueue().dispatch([scene = m_scene.get(), update = WTF::move(update), completionHandler = WTF::move(completionHandler)]() mutable {
        auto updateInfo = scene->update(WTF::move(update));
        RunLoop::mainSingleton().dispatch([updateInfo = WTF::move(updateInfo), completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler(WTF::move(updateInfo));
        });
        // Present after answering: the swap can wait for the display, and the web process can prepare its
        // next update meanwhile. That update queues behind the present on this work queue.
        scene->present();
    });
}

#if OS(WINDOWS)
void RemoteWCLayerTreeHost::updateVideoFrame(WebCore::PlatformLayerIdentifier layer, WCBackingStore&& frame)
{
    RefPtr bitmap = frame.bitmap();
    if (!bitmap)
        return;
    Locker locker { m_videoFrames->lock };
    m_videoFrames->frames.set(layer, WTF::move(bitmap));
    scheduleVideoFrameDrain();
}

void RemoteWCLayerTreeHost::importVideoTexture(WebCore::PlatformLayerIdentifier layer, uint64_t texture, WebCore::IntSize size, WTF::Win32Handle&& handle, CompletionHandler<void(bool)>&& completionHandler)
{
    Locker locker { m_videoFrames->lock };
    m_videoFrames->textureImports.append({ layer, texture, size, WTF::move(handle), WTF::move(completionHandler) });
    scheduleVideoFrameDrain();
}

void RemoteWCLayerTreeHost::updateVideoTexture(WebCore::PlatformLayerIdentifier layer, uint64_t texture)
{
    Locker locker { m_videoFrames->lock };
    m_videoFrames->textureFrames.ensure(layer, [] { return Vector<uint64_t> { }; }).iterator->value.append(texture);
    scheduleVideoFrameDrain();
}

void RemoteWCLayerTreeHost::scheduleVideoFrameDrain()
{
    if (m_videoFrames->drainScheduled)
        return;
    m_videoFrames->drainScheduled = true;
    // Queued behind any layer tree update already on the work queue, so a frame never lands on a layer the scene
    // doesn't have yet. The scene outlives this task: it's destroyed by a task queued after it.
    remoteGraphicsStreamWorkQueue().dispatch([scene = m_scene.get(), mailbox = m_videoFrames] {
        HashMap<WebCore::PlatformLayerIdentifier, RefPtr<WebCore::ShareableBitmap>> frames;
        Vector<TextureImport> imports;
        HashMap<WebCore::PlatformLayerIdentifier, Vector<uint64_t>> textureFrames;
        {
            Locker locker { mailbox->lock };
            frames = std::exchange(mailbox->frames, { });
            imports = std::exchange(mailbox->textureImports, { });
            textureFrames = std::exchange(mailbox->textureFrames, { });
            mailbox->drainScheduled = false;
        }
        for (auto& import : imports) {
            bool imported = scene->importVideoTexture(import.layer, import.texture, import.size, WTF::move(import.handle));
            if (auto* stats = GPURenderingStats::singletonIfEnabled())
                ++(imported ? stats->textureImports : stats->textureImportFailures);
            RunLoop::mainSingleton().dispatch([completionHandler = WTF::move(import.completionHandler), imported]() mutable {
                completionHandler(imported);
            });
        }
        bool painted = false;
        for (auto& [layer, bitmap] : frames)
            painted |= scene->updateVideoFrame(layer, *bitmap);
        for (auto& [layer, textures] : textureFrames) {
            // Only the newest is shown; the others go straight back to the web process.
            for (size_t i = 0; i + 1 < textures.size(); ++i)
                scene->skipVideoTexture(textures[i]);
            painted |= scene->updateVideoTexture(layer, textures.last());
        }
        if (painted)
            scene->present();
    });
}

void RemoteWCLayerTreeHost::setFrozen(bool frozen)
{
    // After a process swap the window shows the next page, painted by the browser if it isn't composited. A frame this
    // page presented again there (up to a second after its last update) covered it, and nothing repainted it: the next
    // page showed this one's pixels until a scroll (1.19.67, New Tab -> Wikipedia, 26 of 30 on the GPU PC).
    m_shownPresents->frozen = frozen;
}

void RemoteWCLayerTreeHost::scheduleShownPresents()
{
    // GLASS_NO_SHOWN_PRESENTS=1 turns these off (comparison, rollback).
    static const bool disabled = [] {
        const char* value = std::getenv("GLASS_NO_SHOWN_PRESENTS");
        return value && !std::strcmp(value, "1");
    }();
    if (disabled || m_shownPresents->scheduled)
        return;
    m_shownPresents->scheduled = true;
    for (auto delay : { 50_ms, 250_ms, 1_s }) {
        RunLoop::mainSingleton().dispatchAfter(delay, [state = m_shownPresents, last = delay == 1_s] {
            if (last)
                state->scheduled = false;
            auto* host = state->host;
            if (!host || state->frozen || !host->m_scene || !IsWindowVisible(reinterpret_cast<HWND>(host->m_nativeWindow)))
                return;
            // Queued ahead of the task that destroys the scene, like the video frames.
            remoteGraphicsStreamWorkQueue().dispatch([scene = host->m_scene.get()] {
                scene->presentAgain();
            });
        });
    }
}
#endif

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
