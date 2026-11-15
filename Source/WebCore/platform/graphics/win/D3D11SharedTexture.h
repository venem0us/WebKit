/*
 * Copyright (C) 2026 Glass contributors
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 */

#pragma once

#if USE(SKIA) && PLATFORM(WIN)

#include "ImageBufferBackend.h"
#include "IntSize.h"
#include <wrl/client.h>
#include <wtf/Function.h>
#include <wtf/TZoneMallocInlines.h>

struct ID3D11Texture2D;

namespace WebCore {

// PlatformDisplaySkia.cpp: why this thread's Skia GL context couldn't be made (a short phrase), or null.
const char* skiaGLContextFailure();

// Image buffers that Skia draws on the GPU, into Direct3D 11 textures on ANGLE's device. The compositor runs in the
// same process on the same device, so it reads a buffer's texture directly, found by the identifier its backend shares:
// no copy, no cross-process handle. Drawing and compositing use separate GL contexts on separate threads, but ANGLE
// submits both to the device's one immediate context, so a texture drawn and flushed before the compositor is told
// about it is complete when the compositor draws with it.
class D3D11SharedTextureSharing final : public ImageBufferBackendSharing {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(D3D11SharedTextureSharing);
public:
    D3D11SharedTextureSharing(uint64_t identifier, IntSize size)
        : m_identifier(identifier)
        , m_size(size)
    {
    }

    uint64_t identifier() const { return m_identifier; }
    IntSize size() const { return m_size; }

private:
    bool isD3D11SharedTextureSharing() const final { return true; }

    uint64_t m_identifier;
    IntSize m_size;
};

namespace D3D11SharedTextures {

// An accelerated Skia backend drawing into a new shared texture, or null if this thread has no Skia GPU context or the
// texture can't be made. Called on the drawing thread; the backend's sharing is a D3D11SharedTextureSharing.
WEBCORE_EXPORT std::unique_ptr<ImageBufferBackend> createSkiaBackend(const ImageBufferParameters&, const ImageBufferCreationContext&);

// The texture shared under an identifier, or null once its image buffer has gone. Any thread.
WEBCORE_EXPORT Microsoft::WRL::ComPtr<ID3D11Texture2D> texture(uint64_t identifier);

// A second name for a shared texture: the image buffer that draws into it, as another process knows it (its owner, a
// web process, and the buffer's identifier there). Dropped with the texture.
WEBCORE_EXPORT void setAlias(uint64_t owner, uint64_t buffer, uint64_t identifier);
struct AliasedTexture {
    uint64_t identifier { 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};
WEBCORE_EXPORT AliasedTexture textureForAlias(uint64_t owner, uint64_t buffer);
// The compositor has issued everything that reads a texture textureForAlias gave it (each call needs one of these):
// once its buffer has gone too, the texture may be drawn into for another buffer.
WEBCORE_EXPORT void finishedReading(uint64_t identifier);

// A drawing thread is finishing (its web process's rendering backend stopped, its buffers released): delete the
// textures it pooled, with their EGL images, and its Skia context, while the thread can still make GL calls. At thread
// exit they would be left behind, and an EGL image keeps its texture's video memory. Does nothing on a thread that
// never made a buffer.
WEBCORE_EXPORT void releaseThreadResources();

// Runs a task on the calling drawing thread (its rendering backend's work queue; dropped once that has stopped). Set by
// the backend on its thread when it starts, so that the device watch thread can have a drawing thread that has drawn
// nothing for a few seconds give back its Skia cache and pooled textures.
WEBCORE_EXPORT void setThreadDispatcher(Function<void(Function<void()>&&)>&&);

// Skia gets GPU contexts (PlatformDisplay::skiaGLContext) only once createSkiaBackend has been called in this process:
// until then accelerated image buffers stay in software on Windows, as they always have.
bool skiaGPUContextsEnabled();

// Has Direct3D destroy the textures released so far (it waits for the next flush), with a context of ANGLE's display
// current: after deleting a batch of them with nothing drawn next.
WEBCORE_EXPORT void flushReleasedTextures();

// Why Direct3D removed the current EGL context's device (a driver reset or update, the GPU hung, disabled or gone), or
// S_OK while it's usable. Everything made on a removed device is lost; only a new device can draw again.
WEBCORE_EXPORT long currentDeviceRemovedReason();
// Once per process: a thread that asks the current EGL context's device every second, calling `check` with its removed
// reason (S_OK while usable), so a removal is noticed on a page that isn't drawing anything too.
WEBCORE_EXPORT void watchCurrentDevice(void (*check)(long reason));

// Why the last createSkiaBackend on this thread returned null (a short phrase), for logs.
WEBCORE_EXPORT const char* lastFailure();
// Whether the last backend createSkiaBackend made on this thread reused a released buffer's texture, for stats.
WEBCORE_EXPORT bool lastWasReused();

// The bytes of every texture made here (in use, waiting for the compositor or pooled), the most since the last call,
// and the GPU memory budget past which new buffers fall back to bitmaps (0 until the first buffer). For stats. Also the
// drawing threads' EGL images still alive: each holds a texture, so a count that grows while bytes don't is a leak. And
// what the drawing threads' pools hold (part of bytes) and their Skia contexts' caches (not in bytes), as last seen.
struct MemoryUse {
    uint64_t bytes { 0 };
    uint64_t peakBytes { 0 };
    uint64_t budgetBytes { 0 };
    uint64_t images { 0 };
    uint64_t pooledBytes { 0 };
    uint64_t skiaCacheBytes { 0 };
    // Since the last call: Direct3D flushes after textures were released (drawing and compositor threads, the watch
    // thread) and driver trims (see flushReleasedTextures).
    uint64_t releaseFlushes { 0 };
    uint64_t watchFlushes { 0 };
    uint64_t trims { 0 };
    // Drawing threads that gave back their Skia cache and pooled textures after drawing nothing for a few seconds.
    uint64_t idlePurges { 0 };
};
WEBCORE_EXPORT MemoryUse takeMemoryUse();

} // namespace D3D11SharedTextures

} // namespace WebCore

#endif // USE(SKIA) && PLATFORM(WIN)
