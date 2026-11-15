/*
 * Copyright (C) 2008 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "DNSResolveQueueCurl.h"

#if USE(CURL)

#include "NotImplemented.h"
#include <wtf/CompletionHandler.h>
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/CString.h>

#if OS(WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
using AddressInfo = ADDRINFOA;   // the narrow-string form, whatever UNICODE says
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
using AddressInfo = struct addrinfo;
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DNSResolveQueueCurl);

// Glass M4c: name resolution for callers that need the addresses (WebRTC's STUN/TURN server names, through the
// network process). getaddrinfo() runs on a background queue; the answer comes back on the main thread. Cancelled
// identifiers are kept on the main thread only.
static HashSet<uint64_t>& cancelledResolves()
{
    static NeverDestroyed<HashSet<uint64_t>> identifiers;
    return identifiers.get();
}

static WorkQueue& resolveQueue()
{
    static NeverDestroyed<Ref<WorkQueue>> queue(WorkQueue::create("DNSResolveQueueCurl"_s));
    return queue.get();
}

void DNSResolveQueueCurl::updateIsUsingProxy()
{
    notImplemented();
}

void DNSResolveQueueCurl::platformResolve(const String& /* hostname */)
{
    notImplemented();
}

void DNSResolveQueueCurl::resolve(const String& hostname, uint64_t identifier, DNSCompletionHandler&& completionHandler)
{
    ASSERT(isMainThread());
    cancelledResolves().remove(identifier);
    resolveQueue().dispatch([hostname = hostname.utf8(), identifier, completionHandler = WTF::move(completionHandler)]() mutable {
        Vector<IPAddress> addresses;
        AddressInfo hints { };
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        AddressInfo* list = nullptr;
        if (!::getaddrinfo(reinterpret_cast<const char*>(hostname.data()), nullptr, &hints, &list)) {
            for (auto* entry = list; entry; entry = entry->ai_next) {
                if (entry->ai_family == AF_INET)
                    addresses.append(IPAddress { reinterpret_cast<const struct sockaddr_in*>(entry->ai_addr)->sin_addr });
                else if (entry->ai_family == AF_INET6)
                    addresses.append(IPAddress { reinterpret_cast<const struct sockaddr_in6*>(entry->ai_addr)->sin6_addr });
            }
            freeaddrinfo(list);
        }
        callOnMainThread([identifier, addresses = WTF::move(addresses), completionHandler = WTF::move(completionHandler)]() mutable {
            if (cancelledResolves().remove(identifier)) {
                completionHandler(makeUnexpected(DNSError::Cancelled));
                return;
            }
            if (addresses.isEmpty()) {
                completionHandler(makeUnexpected(DNSError::CannotResolve));
                return;
            }
            completionHandler(WTF::move(addresses));
        });
    });
}

void DNSResolveQueueCurl::stopResolve(uint64_t identifier)
{
    ASSERT(isMainThread());
    cancelledResolves().add(identifier);
}

}

#endif
