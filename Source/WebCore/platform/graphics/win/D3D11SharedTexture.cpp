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

#include "config.h"
#include "D3D11SharedTexture.h"

#if USE(SKIA) && PLATFORM(WIN)

#include "FontRenderOptions.h"
#include "GLContext.h"
#include "ImageBufferSkiaAcceleratedBackend.h"
#include "PlatformDisplay.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Threading.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Vector.h>
#include <wtf/NeverDestroyed.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLTypes.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {
namespace D3D11SharedTextures {

static std::atomic<bool> gpuContextsEnabled;
static thread_local const char* failure = "";
static thread_local bool reusedLast;

static Lock registryLock;
static uint64_t nextIdentifier WTF_GUARDED_BY_LOCK(registryLock) { 1 };

static HashMap<uint64_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>>& registry() WTF_REQUIRES_LOCK(registryLock)
{
    static NeverDestroyed<HashMap<uint64_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>>> textures;
    return textures;
}

using AliasKey = std::pair<uint64_t, uint64_t>;
static HashMap<AliasKey, uint64_t>& aliases() WTF_REQUIRES_LOCK(registryLock)
{
    static NeverDestroyed<HashMap<AliasKey, uint64_t>> map;
    return map;
}
static HashMap<uint64_t, AliasKey>& aliasOfTexture() WTF_REQUIRES_LOCK(registryLock)
{
    static NeverDestroyed<HashMap<uint64_t, AliasKey>> map;
    return map;
}

// A buffer's texture outlives the buffer while it has an alias nobody has read yet: a web process sends a layer tile's
// buffer once and lets it go, and the GPU process may drop the buffer before the compositor, on another thread, reads
// the update that names it. The compositor's read takes it; a texture nobody reads goes after a few seconds.
static HashMap<uint64_t, MonotonicTime>& releasedUnread() WTF_REQUIRES_LOCK(registryLock)
{
    static NeverDestroyed<HashMap<uint64_t, MonotonicTime>> map;
    return map;
}

// Textures the compositor has read (textureForAlias): those it is still copying from (a count per texture, until
// finishedReading), and those it has finished with at least once, which can be reused as soon as their buffer goes.
static HashMap<uint64_t, unsigned>& readers() WTF_REQUIRES_LOCK(registryLock)
{
    static NeverDestroyed<HashMap<uint64_t, unsigned>> map;
    return map;
}
static HashSet<uint64_t>& read() WTF_REQUIRES_LOCK(registryLock)
{
    static NeverDestroyed<HashSet<uint64_t>> set;
    return set;
}

// Forgetting a texture makes it free: its drawing thread may reuse it for a new buffer (see TexturePool).
static void forget(uint64_t identifier) WTF_REQUIRES_LOCK(registryLock)
{
    registry().remove(identifier);
    releasedUnread().remove(identifier);
    read().remove(identifier);
    if (auto alias = aliasOfTexture().take(identifier); alias.first)
        aliases().remove(alias);
}

static void forgetStaleTextures() WTF_REQUIRES_LOCK(registryLock)
{
    auto now = MonotonicTime::now();
    Vector<uint64_t> stale;
    for (auto& [identifier, releasedAt] : releasedUnread()) {
        if (now - releasedAt > 10_s && !readers().contains(identifier))
            stale.append(identifier);
    }
    for (auto identifier : stale)
        forget(identifier);
}

static ID3D11Device* angleDevice(EGLDisplay display)
{
    static auto queryDisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglGetProcAddress("eglQueryDisplayAttribEXT"));
    static auto queryDevice = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(eglGetProcAddress("eglQueryDeviceAttribEXT"));
    if (!queryDisplay || !queryDevice)
        return nullptr;
    EGLAttrib device = 0;
    EGLAttrib d3dDevice = 0;
    if (!queryDisplay(display, EGL_DEVICE_EXT, &device) || !queryDevice(reinterpret_cast<EGLDeviceEXT>(device), EGL_D3D11_DEVICE_ANGLE, &d3dDevice))
        return nullptr;
    return reinterpret_cast<ID3D11Device*>(d3dDevice);
}

long currentDeviceRemovedReason()
{
    EGLDisplay display = eglGetCurrentDisplay();
    auto* device = display != EGL_NO_DISPLAY ? angleDevice(display) : nullptr;
    return device ? device->GetDeviceRemovedReason() : S_OK;
}

void watchCurrentDevice(void (*check)(long reason))
{
    static std::once_flag once;
    std::call_once(once, [check] {
        EGLDisplay display = eglGetCurrentDisplay();
        Microsoft::WRL::ComPtr<ID3D11Device> device = display != EGL_NO_DISPLAY ? angleDevice(display) : nullptr;
        if (!device)
            return;
        Thread::create("D3D11 device watch"_s, [device = WTF::move(device), check] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                check(device->GetDeviceRemovedReason());
            }
        })->detach();
    });
}

// ANGLE on Windows' software Direct3D (WARP, a PC or virtual machine without a usable GPU) would draw "on the GPU" much
// slower than Skia does on the CPU, so it isn't used unless GLASS_GPU_RASTER_ALLOW_WARP=1 (testing).
static bool isSoftwareAdapter(ID3D11Device* device)
{
    static const bool software = [&] {
        const char* allow = std::getenv("GLASS_GPU_RASTER_ALLOW_WARP");
        if (allow && !std::strcmp(allow, "1"))
            return false;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description { };
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description)))
            return false;
        // Microsoft's Basic Render Driver.
        return description.VendorId == 0x1414 && description.DeviceId == 0x8c;
    }();
    return software;
}

// GPU memory budget. Every texture made here (drawn, waiting for the compositor or pooled) counts, and once they reach
// the budget, or Windows says this process is over its video memory budget (the driver would start evicting), new
// buffers fall back to bitmaps: a tile is drawn on the processor and uploaded, a canvas draws on the processor. Pages
// with many or huge canvases otherwise take video memory without limit (WebKit's own limits count buffers, not bytes).
// A quarter of the adapter's dedicated memory, between 256 MB and 2 GB; GLASS_GPU_MEMORY_BUDGET_MB overrides it.
static std::atomic<uint64_t> textureBytes;
static std::atomic<uint64_t> peakTextureBytes;
static std::atomic<uint64_t> knownBudget;
static std::atomic<uint64_t> liveImages;
// For stats: the drawing threads' pools, and their Skia contexts' caches (each thread adds what changed since it last
// looked, when it makes a buffer).
static std::atomic<int64_t> pooledBytes;
static std::atomic<int64_t> skiaCacheBytes;
static thread_local int64_t threadSkiaCacheBytes;
// Whether this thread has made a buffer (and so has a Skia context, maybe a pool): see releaseThreadResources.
static thread_local bool threadMadeBuffers;

static uint64_t bytesForSize(IntSize size)
{
    return static_cast<uint64_t>(size.width()) * size.height() * 4;
}

static Microsoft::WRL::ComPtr<IDXGIAdapter> adapterFor(ID3D11Device* device)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)))
        return nullptr;
    return adapter;
}

static uint64_t memoryBudget(ID3D11Device* device)
{
    static const uint64_t budget = [&]() -> uint64_t {
        constexpr uint64_t megabyte = 1024 * 1024;
        if (const char* value = std::getenv("GLASS_GPU_MEMORY_BUDGET_MB"); value && *value)
            return static_cast<uint64_t>(std::strtoull(value, nullptr, 10)) * megabyte;
        DXGI_ADAPTER_DESC description { };
        auto adapter = adapterFor(device);
        uint64_t dedicated = adapter && SUCCEEDED(adapter->GetDesc(&description)) ? description.DedicatedVideoMemory : 0;
        return std::clamp<uint64_t>(dedicated / 4, 256 * megabyte, 2048 * megabyte);
    }();
    knownBudget = budget;
    return budget;
}

// Whether Windows reports this process over its local video memory budget, asked at most twice a second.
static bool overSystemBudget(ID3D11Device* device)
{
    static Lock lock;
    static MonotonicTime lastQuery WTF_GUARDED_BY_LOCK(lock);
    static bool over WTF_GUARDED_BY_LOCK(lock) { false };
    Locker locker { lock };
    auto now = MonotonicTime::now();
    if (now - lastQuery < 500_ms)
        return over;
    lastQuery = now;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO info { };
    auto adapter = adapterFor(device);
    over = adapter && SUCCEEDED(adapter.As(&adapter3)) && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))
        && info.Budget && info.CurrentUsage > info.Budget;
    return over;
}

// What Skia's surface draws into: the GL texture made from the shared texture's EGL image. Released by Skia, on the
// drawing thread with its context current, when the surface lets go of the texture.
struct WrappedTexture {
    uint64_t identifier;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    IntSize size;
    EGLDisplay display;
    EGLImageKHR image;
    GLuint glTexture;
    Thread* thread;
    // Whether it goes back to the pool when released: only buffers that are replaced all the time (layer tiles). A
    // canvas's texture is deleted, since the pool is trimmed only when a buffer is made, which an idle page never does.
    bool poolable;
};

static void destroyWrappedTexture(const WrappedTexture& wrapped)
{
    static auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    textureBytes -= bytesForSize(wrapped.size);
    if (wrapped.glTexture)
        glDeleteTextures(1, &wrapped.glTexture);
    if (wrapped.image != EGL_NO_IMAGE_KHR && destroyImage) {
        destroyImage(wrapped.display, wrapped.image);
        --liveImages;
    }
}

// Each drawing thread keeps the textures of its released buffers (with their EGL images and GL textures, which belong
// to its Skia context) and gives them to new buffers of the same size once the compositor is done with them: page tiles
// are redrawn into new buffers all the time, and making a texture, an EGL image and a Skia surface for each was most of
// what drawing a tile on the GPU cost. Textures unused for a few seconds, or past the pool's size, are deleted. A
// finishing thread deletes the rest (releaseThreadResources).
class TexturePool {
public:
    ~TexturePool()
    {
        // The thread is ending without releaseThreadResources; its GL context may already be gone, and EGL calls at
        // thread exit run under the loader lock, so the GL objects and EGL images are left (and their textures with them).
        for (auto& entry : m_entries) {
            entry.wrapped.glTexture = 0;
            textureBytes -= bytesForSize(entry.wrapped.size);
        }
        pooledBytes -= m_bytes;
    }

    void add(WrappedTexture&& wrapped)
    {
        m_bytes += bytesForSize(wrapped.size);
        pooledBytes += bytesForSize(wrapped.size);
        m_entries.append({ WTF::move(wrapped), MonotonicTime::now() });
    }

    // With the thread's Skia context current: every entry, whether or not the compositor still has its texture (the
    // registry keeps that alive on its own).
    void destroyAll()
    {
        auto entries = std::exchange(m_entries, { });
        pooledBytes -= m_bytes;
        m_bytes = 0;
        for (auto& entry : entries)
            destroyWrappedTexture(entry.wrapped);
    }

    std::optional<WrappedTexture> take(IntSize size) WTF_REQUIRES_LOCK(registryLock)
    {
        for (size_t i = m_entries.size(); i--;) {
            auto& entry = m_entries[i];
            if (entry.wrapped.size == size && !registry().contains(entry.wrapped.identifier)) {
                auto wrapped = WTF::move(entry.wrapped);
                m_entries.removeAt(i);
                m_bytes -= bytesForSize(size);
                pooledBytes -= bytesForSize(size);
                return wrapped;
            }
        }
        return std::nullopt;
    }

    // Called with the thread's Skia context current. Whether it deleted any. Over the memory budget, every free entry goes.
    // A thread whose page stops drawing never trims again, so the pool is kept small in bytes too (the oldest go first).
    bool trim(bool everythingFree)
    {
        static constexpr size_t maximumEntries = 64;
        static constexpr uint64_t maximumBytes = 32 * 1024 * 1024;
        auto now = MonotonicTime::now();
        Vector<WrappedTexture> unused;
        {
            Locker locker { registryLock };
            for (size_t i = 0; i < m_entries.size();) {
                auto& entry = m_entries[i];
                bool free = !registry().contains(entry.wrapped.identifier);
                if (free && (everythingFree || now - entry.releasedAt > 3_s || m_entries.size() > maximumEntries || m_bytes > maximumBytes)) {
                    m_bytes -= bytesForSize(entry.wrapped.size);
                    pooledBytes -= bytesForSize(entry.wrapped.size);
                    unused.append(WTF::move(entry.wrapped));
                    m_entries.removeAt(i);
                } else
                    ++i;
            }
        }
        for (auto& wrapped : unused)
            destroyWrappedTexture(wrapped);
        return !unused.isEmpty();
    }

    bool isEmpty() const { return m_entries.isEmpty(); }

private:
    struct Entry {
        WrappedTexture wrapped;
        MonotonicTime releasedAt;
    };
    Vector<Entry> m_entries;
    uint64_t m_bytes { 0 };
};

static TexturePool& texturePool()
{
    static thread_local TexturePool pool;
    return pool;
}

static void releaseWrappedTexture(void* context)
{
    std::unique_ptr<WrappedTexture> wrapped { static_cast<WrappedTexture*>(context) };
    {
        Locker locker { registryLock };
        if (readers().contains(wrapped->identifier) || (aliasOfTexture().contains(wrapped->identifier) && !read().contains(wrapped->identifier)))
            releasedUnread().set(wrapped->identifier, MonotonicTime::now());
        else
            forget(wrapped->identifier);
        forgetStaleTextures();
    }
    // Skia releases on the thread that drew, with its context current; anywhere else, the GL objects can't be kept.
    if (wrapped->thread != &Thread::currentSingleton()) {
        destroyWrappedTexture(*wrapped);
        return;
    }
    if (!wrapped->poolable) {
        destroyWrappedTexture(*wrapped);
        // Deleting a texture Skia last bound unbinds it under Skia (see createSkiaBackend's trim).
        if (auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext())
            grContext->resetContext(kTextureBinding_GrGLBackendState);
        return;
    }
    auto& pool = texturePool();
    pool.add(WTF::move(*wrapped));
    // A page that stops drawing makes no more buffers, so trim here too, or its last tiles' textures stay pooled.
    if (pool.trim(false)) {
        if (auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext())
            grContext->resetContext(kTextureBinding_GrGLBackendState);
    }
}

static std::unique_ptr<ImageBufferBackend> wrapTexture(const ImageBufferParameters&, const ImageBufferCreationContext&, GrDirectContext*, WrappedTexture*, IntSize);

std::unique_ptr<ImageBufferBackend> createSkiaBackend(const ImageBufferParameters& parameters, const ImageBufferCreationContext& creationContext)
{
    static auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    static auto imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    gpuContextsEnabled = true;
    auto fail = [](const char* reason) -> std::unique_ptr<ImageBufferBackend> {
        failure = reason;
        return nullptr;
    };
    if (!createImage || !imageTargetTexture)
        return fail("no EGL image functions");

    auto size = ImageBufferBackend::calculateSafeBackendSize(parameters);
    if (size.isEmpty())
        return fail("empty size");

    auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!glContext) {
        auto* reason = skiaGLContextFailure();
        return fail(reason ? reason : "no Skia GL context");
    }
    if (!glContext->makeContextCurrent())
        return fail("Skia GL context not made current");
    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    if (!grContext)
        return fail("no Skia GrDirectContext");
    EGLDisplay display = eglGetCurrentDisplay();
    auto* device = angleDevice(display);
    if (!device)
        return fail("no D3D11 device from ANGLE");
    // With drawing on the GPU, threads other than ANGLE's callers reach the device's one immediate context: the
    // compositor's video textures (their keyed mutexes) while a drawing thread is inside ANGLE. ANGLE serialises only
    // its own calls, so have Direct3D lock every immediate-context call too.
    static std::once_flag protectOnce;
    std::call_once(protectOnce, [device] {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (context && SUCCEEDED(context.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);
    });
    if (isSoftwareAdapter(device))
        return fail("software adapter (WARP); GLASS_GPU_RASTER_ALLOW_WARP=1 to use it anyway");
    threadMadeBuffers = true;

    auto& pool = texturePool();
    std::optional<WrappedTexture> reused;
    uint64_t identifier;
    {
        Locker locker { registryLock };
        reused = pool.take(size);
        if (reused) {
            identifier = nextIdentifier++;
            reused->identifier = identifier;
            registry().set(identifier, reused->texture);
        }
    }
    // Canvases keep their buffer for as long as the page keeps them, so they may fill only three quarters of the budget:
    // the rest stays for layer tiles, whose buffers are replaced every update and would otherwise all end up on the
    // processor once canvases had taken everything.
    auto budget = memoryBudget(device);
    if (parameters.purpose == RenderingPurpose::Canvas)
        budget -= budget / 4;
    auto overBudget = [&] {
        return textureBytes + bytesForSize(size) > budget || overSystemBudget(device);
    };
    // Deleting a texture Skia last bound unbinds it under Skia, as in the creation below.
    if (!pool.isEmpty() && pool.trim(overBudget()))
        grContext->resetContext(kTextureBinding_GrGLBackendState);
    size_t cacheBytes = 0;
    grContext->getResourceCacheUsage(nullptr, &cacheBytes);
    skiaCacheBytes += static_cast<int64_t>(cacheBytes) - std::exchange(threadSkiaCacheBytes, static_cast<int64_t>(cacheBytes));
    reusedLast = !!reused;
    if (reused) {
        reused->poolable = parameters.purpose == RenderingPurpose::LayerBacking;
        return wrapTexture(parameters, creationContext, grContext, new WrappedTexture(WTF::move(*reused)), size);
    }
    if (overBudget())
        return fail("over the GPU memory budget");

    D3D11_TEXTURE2D_DESC description { };
    description.Width = size.width();
    description.Height = size.height();
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &texture)))
        return fail("CreateTexture2D failed");

    const EGLint attributes[] = { EGL_NONE };
    EGLImageKHR image = createImage(display, EGL_NO_CONTEXT, EGL_D3D11_TEXTURE_ANGLE, static_cast<EGLClientBuffer>(texture.Get()), attributes);
    if (image == EGL_NO_IMAGE_KHR)
        return fail("eglCreateImageKHR failed");
    ++liveImages;
    GLuint glTexture = 0;
    glGenTextures(1, &glTexture);
    glBindTexture(GL_TEXTURE_2D, glTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    imageTargetTexture(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);
    // Skia keeps its own record of the GL state it set; tell it the texture binding changed under it.
    grContext->resetContext(kTextureBinding_GrGLBackendState);

    {
        Locker locker { registryLock };
        identifier = nextIdentifier++;
        registry().set(identifier, texture);
    }
    auto bytes = textureBytes += bytesForSize(size);
    for (auto peak = peakTextureBytes.load(); bytes > peak && !peakTextureBytes.compare_exchange_weak(peak, bytes); ) { }
    auto* wrapped = new WrappedTexture { identifier, texture, size, display, image, glTexture, &Thread::currentSingleton(), parameters.purpose == RenderingPurpose::LayerBacking };
    return wrapTexture(parameters, creationContext, grContext, wrapped, size);
}

static std::unique_ptr<ImageBufferBackend> wrapTexture(const ImageBufferParameters& parameters, const ImageBufferCreationContext& creationContext, GrDirectContext* grContext, WrappedTexture* wrapped, IntSize size)
{
    auto fail = [](const char* reason) -> std::unique_ptr<ImageBufferBackend> {
        failure = reason;
        return nullptr;
    };
    uint64_t identifier = wrapped->identifier;
    GrGLTextureInfo textureInfo;
    textureInfo.fTarget = GL_TEXTURE_2D;
    textureInfo.fID = wrapped->glTexture;
    textureInfo.fFormat = GL_RGBA8_OES;
    auto backendTexture = GrBackendTextures::MakeGL(size.width(), size.height(), skgpu::Mipmapped::kNo, textureInfo);
    SkSurfaceProps properties = FontRenderOptions::singleton().createSurfaceProps();
    // Skia calls releaseWrappedTexture when the surface is done with the texture, or straight away if it can't make one.
    auto surface = SkSurfaces::WrapBackendTexture(grContext, backendTexture, kTopLeft_GrSurfaceOrigin, 0, kRGBA_8888_SkColorType,
        parameters.colorSpace.platformColorSpace(), &properties, releaseWrappedTexture, wrapped);
    if (!surface || !surface->getCanvas())
        return fail("Skia couldn't wrap the texture");
    // Every buffer starts transparent. A reused texture still holds its last buffer's pixels, and a new one's memory is
    // undefined: the driver can hand back memory of a texture this process just released, i.e. another tile's old paint.
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);

    auto backend = ImageBufferSkiaAcceleratedBackend::create(parameters, creationContext, WTF::move(surface));
    if (!backend)
        return fail("no accelerated backend");
    failure = "";
    backend->setBackendSharing(makeUnique<D3D11SharedTextureSharing>(identifier, size));
    return backend;
}

void setAlias(uint64_t owner, uint64_t buffer, uint64_t identifier)
{
    // Owners and buffers are identifiers counted from 1, so the hash tables' empty keys (zeros) never occur.
    if (!owner || !buffer)
        return;
    Locker locker { registryLock };
    if (!registry().contains(identifier))
        return;
    aliases().set({ owner, buffer }, identifier);
    aliasOfTexture().set(identifier, AliasKey { owner, buffer });
}

AliasedTexture textureForAlias(uint64_t owner, uint64_t buffer)
{
    if (!owner || !buffer)
        return { };
    Locker locker { registryLock };
    auto identifier = aliases().get({ owner, buffer });
    if (!identifier)
        return { };
    AliasedTexture result { identifier, registry().get(identifier) };
    readers().add(identifier, 0).iterator->value++;
    return result;
}

void finishedReading(uint64_t identifier)
{
    Locker locker { registryLock };
    auto reader = readers().find(identifier);
    if (reader == readers().end())
        return;
    if (--reader->value)
        return;
    readers().remove(reader);
    // Released while the compositor read it: free now. Otherwise free as soon as its buffer goes.
    if (releasedUnread().contains(identifier))
        forget(identifier);
    else if (registry().contains(identifier))
        read().add(identifier);
}

bool skiaGPUContextsEnabled()
{
    return gpuContextsEnabled;
}

const char* lastFailure()
{
    return failure;
}

bool lastWasReused()
{
    return reusedLast;
}

MemoryUse takeMemoryUse()
{
    uint64_t current = textureBytes;
    return { current, std::max(peakTextureBytes.exchange(current), current), knownBudget, liveImages,
        static_cast<uint64_t>(std::max<int64_t>(pooledBytes, 0)), static_cast<uint64_t>(std::max<int64_t>(skiaCacheBytes, 0)) };
}

void releaseThreadResources()
{
    if (!threadMadeBuffers)
        return;
    threadMadeBuffers = false;
    skiaCacheBytes -= std::exchange(threadSkiaCacheBytes, 0);
    auto& display = PlatformDisplay::sharedDisplay();
    if (auto* glContext = display.skiaGLContext(); glContext && glContext->makeContextCurrent()) {
        texturePool().destroyAll();
        if (auto* grContext = display.skiaGrContext())
            grContext->freeGpuResources();
    }
    // No context: drops this thread's, here rather than at thread exit.
    display.setSkiaGLContextForCurrentThread(nullptr);
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> texture(uint64_t identifier)
{
    Locker locker { registryLock };
    return registry().get(identifier);
}

} // namespace D3D11SharedTextures
} // namespace WebCore

#endif // USE(SKIA) && PLATFORM(WIN)
