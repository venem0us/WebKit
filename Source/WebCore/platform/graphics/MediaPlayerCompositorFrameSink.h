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

#if ENABLE(VIDEO) && OS(WINDOWS)

#include <WebCore/IntSize.h>
#include <span>
#include <wtf/RefPtr.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebCore {

class MediaPlayer;

// Receives a video's frames on the thread that produces them, for a compositor that shows them without the main
// thread's rendering updates (the WC compositor with the GStreamer port). Thread-safe.
class MediaPlayerCompositorFrameSink : public ThreadSafeRefCounted<MediaPlayerCompositorFrameSink> {
public:
    virtual ~MediaPlayerCompositorFrameSink() = default;
    // BGRA pixels with premultiplied alpha, rows `stride` bytes apart; `opaque` means the fourth byte isn't alpha
    // (BGRx) and the frame is fully opaque. False if the frame couldn't be sent: the player then shows it through a
    // main-thread repaint as before.
    virtual bool pushFrame(std::span<const uint8_t> pixels, const IntSize&, unsigned stride, bool opaque) = 0;
};

// Where the player sends frames straight from its decoding thread (null: back to repaints through the main thread).
WEBCORE_EXPORT void setCompositorFrameSink(MediaPlayer&, RefPtr<MediaPlayerCompositorFrameSink>&&);

} // namespace WebCore

#endif // ENABLE(VIDEO) && OS(WINDOWS)
