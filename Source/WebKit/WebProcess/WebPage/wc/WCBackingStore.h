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

#include "ImageBufferBackendHandle.h"
#include <WebCore/ImageBuffer.h>
#include <WebCore/RenderingResourceIdentifier.h>
#include <wtf/TZoneMalloc.h>

namespace WebKit {

class WCBackingStore {
    WTF_MAKE_TZONE_ALLOCATED(WCBackingStore);
public:
    WCBackingStore() = default;
    WebCore::ImageBuffer* imageBuffer() { return m_imageBuffer.get(); }
    void setImageBuffer(RefPtr<WebCore::ImageBuffer>&& image) { m_imageBuffer = WTF::move(image); }
    // Sends pixels that are already in shared memory (a video frame) instead of an image buffer's.
    void setBitmap(RefPtr<WebCore::ShareableBitmap>&& bitmap) { m_bitmap = WTF::move(bitmap); }
    WebCore::ShareableBitmap* bitmap() const { return m_bitmap.get(); }
    // A buffer drawn on the GPU (an accelerated image buffer in the GPU process, see GLASS_GPU_RASTER) travels as its
    // name, not its pixels: the compositor, in the same process, reads its texture.
    std::optional<WebCore::RenderingResourceIdentifier> acceleratedBufferIdentifier() const;

private:
    friend struct IPC::ArgumentCoder<WCBackingStore>;

    WCBackingStore(std::optional<ImageBufferBackendHandle>&&, std::optional<WebCore::RenderingResourceIdentifier>&&);
    std::optional<ImageBufferBackendHandle> handle() const;

    RefPtr<WebCore::ImageBuffer> m_imageBuffer;
    RefPtr<WebCore::ShareableBitmap> m_bitmap;
    std::optional<WebCore::RenderingResourceIdentifier> m_acceleratedBufferIdentifier;
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
