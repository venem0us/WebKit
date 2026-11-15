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
#include "WCSceneContext.h"

#if USE(GRAPHICS_LAYER_WC)

#include <WebCore/GLContext.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/TextureMapper.h>
#include <cstdlib>

#if OS(WINDOWS)
#include <EGL/egl.h>
#endif
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WCSceneContext);

WCSceneContext::WCSceneContext(uint64_t nativeWindow)
    : m_glContext(WebCore::GLContext::create(WebCore::PlatformDisplay::sharedDisplay(), reinterpret_cast<GLNativeWindowType>(nativeWindow)))
{
#if OS(WINDOWS)
    // With the default swap interval of 1, presenting a frame waits until the display can take another; at a
    // steady 60 frames a second the present queue stays full, so each present blocked for most of a frame
    // (11-14 ms full screen) and held up the next composite. The window is composed by DWM, which shows the
    // newest frame at each refresh, so presenting without waiting doesn't tear. WEBKIT_WC_SWAP_INTERVAL
    // overrides the interval (1 restores the waiting present).
    if (m_glContext && m_glContext->makeContextCurrent()) {
        int interval = 0;
        if (auto* value = std::getenv("WEBKIT_WC_SWAP_INTERVAL"); value && *value)
            interval = std::atoi(value);
        eglSwapInterval(eglGetCurrentDisplay(), interval);
    }
#endif
}

WCSceneContext::~WCSceneContext() = default;

bool WCSceneContext::makeContextCurrent()
{
    if (!m_glContext)
        return false;
    return m_glContext->makeContextCurrent();
}

std::unique_ptr<WebCore::TextureMapper> WCSceneContext::createTextureMapper()
{
    return WebCore::TextureMapper::create();
}

void WCSceneContext::swapBuffers()
{
    m_glContext->swapBuffers();
}

#if OS(WINDOWS)
WebCore::IntSize WCSceneContext::surfaceSize() const
{
    EGLint width = 0, height = 0;
    EGLDisplay display = eglGetCurrentDisplay();
    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (surface == EGL_NO_SURFACE || !eglQuerySurface(display, surface, EGL_WIDTH, &width) || !eglQuerySurface(display, surface, EGL_HEIGHT, &height))
        return { };
    return { width, height };
}
#endif

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
