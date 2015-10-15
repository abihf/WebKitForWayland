/*
 * Copyright (C) 2014 Igalia S.L.
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

#ifndef ThreadedCompositor_h
#define ThreadedCompositor_h

#if USE(COORDINATED_GRAPHICS_THREADED)

#include "CompositingManager.h"
#include "CoordinatedGraphicsScene.h"
#include "SimpleViewportController.h"
#include <WebCore/GLContext.h>
#include <WebCore/IntSize.h>
#include <WebCore/TransformationMatrix.h>
#include <wtf/Atomics.h>
#include <wtf/Condition.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ThreadSafeRefCounted.h>

#if PLATFORM(GBM)
#include <WebCore/GBMSurface.h>
#include <WebCore/PlatformDisplayGBM.h>
#endif

#if PLATFORM(BCM_RPI)
#include <WebCore/BCMRPiSurface.h>
#include <WebCore/PlatformDisplayBCMRPi.h>
#endif

#if USE(REQUEST_ANIMATION_FRAME_DISPLAY_MONITOR)
#include <WebCore/DisplayRefreshMonitor.h>
#endif

namespace WebCore {
struct CoordinatedGraphicsState;
}

namespace WebKit {

class CompositingRunLoop;
class CoordinatedGraphicsScene;
class CoordinatedGraphicsSceneClient;
class WebPage;

class ThreadedCompositor : public ThreadSafeRefCounted<ThreadedCompositor>, public SimpleViewportController::Client, public CoordinatedGraphicsSceneClient, 
#if PLATFORM(GBM)
    public WebCore::GBMSurface::Client,
#endif
    public CompositingManager::Client {
    WTF_MAKE_NONCOPYABLE(ThreadedCompositor);
    WTF_MAKE_FAST_ALLOCATED;
public:
    class Client {
    public:
        virtual void setVisibleContentsRect(const WebCore::FloatRect&, const WebCore::FloatPoint&, float) = 0;
        virtual void purgeBackingStores() = 0;
        virtual void renderNextFrame() = 0;
        virtual void commitScrollOffset(uint32_t layerID, const WebCore::IntSize& offset) = 0;
    };

    static Ref<ThreadedCompositor> create(Client*, WebPage&);
    virtual ~ThreadedCompositor();

    void setNeedsDisplay();

    void setNativeSurfaceHandleForCompositing(uint64_t);

    void updateSceneState(const WebCore::CoordinatedGraphicsState&);

    void didChangeViewportSize(const WebCore::IntSize&);
    void didChangeViewportAttribute(const WebCore::ViewportAttributes&);
    void didChangeContentsSize(const WebCore::IntSize&);
    void scrollTo(const WebCore::IntPoint&);
    void scrollBy(const WebCore::IntSize&);

    RefPtr<WebCore::DisplayRefreshMonitor> createDisplayRefreshMonitor(PlatformDisplayID);

private:
    ThreadedCompositor(Client*, WebPage&);

    // CoordinatedGraphicsSceneClient
    virtual void purgeBackingStores() override;
    virtual void renderNextFrame() override;
    virtual void updateViewport() override;
    virtual void commitScrollOffset(uint32_t layerID, const WebCore::IntSize& offset) override;

#if PLATFORM(GBM)
    // GBMSurface::Client
    virtual void destroyBuffer(uint32_t) override;
#endif

    // CompositingManager::Client
    virtual void releaseBuffer(uint32_t) override;
    virtual void frameComplete() override;

    void renderLayerTree();
    void scheduleDisplayImmediately();
    virtual void didChangeVisibleRect() override;

    bool ensureGLContext();
    WebCore::GLContext* glContext();
    SimpleViewportController* viewportController() { return m_viewportController.get(); }

    void callOnCompositingThread(std::function<void()>);
    void createCompositingThread();
    void runCompositingThread();
    void terminateCompositingThread();
    static void compositingThreadEntry(void*);

    Client* m_client;
    RefPtr<CoordinatedGraphicsScene> m_scene;
    std::unique_ptr<SimpleViewportController> m_viewportController;

#if PLATFORM(GBM)
    std::unique_ptr<WebCore::GBMSurface> m_gbmSurface;
#endif
#if PLATFORM(BCM_RPI)
    std::unique_ptr<WebCore::BCMRPiSurface> m_surface;
#endif
    std::unique_ptr<WebCore::GLContext> m_context;

    WebCore::IntSize m_viewportSize;
    uint64_t m_nativeSurfaceHandle;

    std::unique_ptr<CompositingRunLoop> m_compositingRunLoop;

    ThreadIdentifier m_threadIdentifier;
    Condition m_initializeRunLoopCondition;
    Lock m_initializeRunLoopConditionLock;
    Condition m_terminateRunLoopCondition;
    Lock m_terminateRunLoopConditionLock;

    CompositingManager m_compositingManager;

#if USE(REQUEST_ANIMATION_FRAME_DISPLAY_MONITOR)
    class DisplayRefreshMonitor : public WebCore::DisplayRefreshMonitor {
    public:
        DisplayRefreshMonitor(ThreadedCompositor&);

        virtual bool requestRefreshCallback() override;

        bool requiresDisplayRefreshCallback();
        void dispatchDisplayRefreshCallback();
        void invalidate();

    private:
        void displayRefreshCallback();
        RunLoop::Timer<DisplayRefreshMonitor> m_displayRefreshTimer;
        ThreadedCompositor* m_compositor;
    };
    RefPtr<DisplayRefreshMonitor> m_displayRefreshMonitor;
#endif

    Atomic<bool> m_clientRendersNextFrame;
    Atomic<bool> m_coordinateUpdateCompletionWithClient;
};

} // namespace WebKit

#endif

#endif // ThreadedCompositor_h
