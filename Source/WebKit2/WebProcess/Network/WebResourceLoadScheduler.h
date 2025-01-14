/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
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

#ifndef WebResourceLoadScheduler_h
#define WebResourceLoadScheduler_h

#include "WebResourceLoader.h"
#include <WebCore/LoaderStrategy.h>
#include <WebCore/ResourceLoader.h>
#include <wtf/HashSet.h>
#include <wtf/RunLoop.h>

namespace WebKit {

class NetworkProcessConnection;
class WebResourceLoadScheduler;
typedef uint64_t ResourceLoadIdentifier;

class WebResourceLoadScheduler : public WebCore::LoaderStrategy {
    WTF_MAKE_NONCOPYABLE(WebResourceLoadScheduler); WTF_MAKE_FAST_ALLOCATED;
public:
    WebResourceLoadScheduler();
    virtual ~WebResourceLoadScheduler();
    
    virtual RefPtr<WebCore::SubresourceLoader> loadResource(WebCore::Frame*, WebCore::CachedResource*, const WebCore::ResourceRequest&, const WebCore::ResourceLoaderOptions&) override;
    virtual void loadResourceSynchronously(WebCore::NetworkingContext*, unsigned long resourceLoadIdentifier, const WebCore::ResourceRequest&, WebCore::StoredCredentials, WebCore::ClientCredentialPolicy, WebCore::ResourceError&, WebCore::ResourceResponse&, Vector<char>& data) override;

    virtual void remove(WebCore::ResourceLoader*) override;
    virtual void setDefersLoading(WebCore::ResourceLoader*, bool) override;
    virtual void crossOriginRedirectReceived(WebCore::ResourceLoader*, const WebCore::URL& redirectURL) override;
    
    virtual void servePendingRequests(WebCore::ResourceLoadPriority minimumPriority) override;

    virtual void suspendPendingRequests() override;
    virtual void resumePendingRequests() override;

    virtual void createPingHandle(WebCore::NetworkingContext*, WebCore::ResourceRequest&, bool shouldUseCredentialStorage) override;

    WebResourceLoader* webResourceLoaderForIdentifier(ResourceLoadIdentifier identifier) const { return m_webResourceLoaders.get(identifier); }
    RefPtr<WebCore::NetscapePlugInStreamLoader> schedulePluginStreamLoad(WebCore::Frame*, WebCore::NetscapePlugInStreamLoaderClient*, const WebCore::ResourceRequest&);

    void networkProcessCrashed();

private:
    void scheduleLoad(WebCore::ResourceLoader*, WebCore::CachedResource*, bool shouldClearReferrerOnHTTPSToHTTPRedirect);
    void scheduleInternallyFailedLoad(WebCore::ResourceLoader*);
    void internallyFailedLoadTimerFired();
    void startLocalLoad(WebCore::ResourceLoader&);

    HashSet<RefPtr<WebCore::ResourceLoader>> m_internallyFailedResourceLoaders;
    RunLoop::Timer<WebResourceLoadScheduler> m_internallyFailedLoadTimer;
    
    HashMap<unsigned long, RefPtr<WebResourceLoader>> m_webResourceLoaders;
};

} // namespace WebKit

#endif // WebResourceLoadScheduler_h
