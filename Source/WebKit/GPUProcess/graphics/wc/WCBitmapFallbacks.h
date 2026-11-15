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

#if USE(GRAPHICS_LAYER_WC) && OS(WINDOWS)

#include <WebCore/ShareableBitmap.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>

namespace WebKit {

// With GLASS_GPU_RASTER, a web process sends an accelerated layer tile by name, expecting a GPU texture. Where the GPU
// process couldn't make one (no GPU, Windows' software Direct3D, an allocation failure), the buffer is a bitmap: kept
// here under the same name until the compositor takes it, so the tile still arrives. Unclaimed bitmaps go after 10 s.
class WCBitmapFallbacks {
public:
    using Key = std::pair<uint64_t, uint64_t>; // web process, buffer

    static void set(Key key, RefPtr<WebCore::ShareableBitmap>&& bitmap)
    {
        if (!key.first || !key.second || !bitmap)
            return;
        Locker locker { s_lock };
        forgetStale();
        map().set(key, Entry { WTF::move(bitmap), MonotonicTime::now() });
    }

    static RefPtr<WebCore::ShareableBitmap> take(Key key)
    {
        if (!key.first || !key.second)
            return nullptr;
        Locker locker { s_lock };
        return map().take(key).bitmap;
    }

private:
    struct Entry {
        RefPtr<WebCore::ShareableBitmap> bitmap;
        MonotonicTime added;
    };

    static inline Lock s_lock;

    static HashMap<Key, Entry>& map() WTF_REQUIRES_LOCK(s_lock)
    {
        static NeverDestroyed<HashMap<Key, Entry>> map;
        return map;
    }

    static void forgetStale() WTF_REQUIRES_LOCK(s_lock)
    {
        auto now = MonotonicTime::now();
        Vector<Key> stale;
        for (auto& [key, entry] : map()) {
            if (now - entry.added > 10_s)
                stale.append(key);
        }
        for (auto& key : stale)
            map().remove(key);
    }
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC) && OS(WINDOWS)
