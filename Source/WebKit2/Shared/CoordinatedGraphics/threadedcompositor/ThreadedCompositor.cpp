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

#include "config.h"

#if USE(COORDINATED_GRAPHICS_THREADED)
#include "ThreadedCompositor.h"

#include <WebCore/GLContextEGL.h>
#include <WebCore/PlatformDisplayGBM.h>
#include <WebCore/TransformationMatrix.h>
#include <cstdio>
#include <cstdlib>
#include <wtf/Atomics.h>
#include <wtf/CurrentTime.h>
#include <wtf/RunLoop.h>
#include <wtf/StdLibExtras.h>

#if USE(OPENGL_ES_2)
#include <GLES2/gl2.h>
#else
#include <GL/gl.h>
#endif

using namespace WebCore;

namespace WebKit {

class CompositingRunLoop {
    WTF_MAKE_NONCOPYABLE(CompositingRunLoop);
    WTF_MAKE_FAST_ALLOCATED;
public:
    CompositingRunLoop(std::function<void()> updateFunction)
        : m_runLoop(RunLoop::current())
        , m_updateTimer(m_runLoop, this, &CompositingRunLoop::updateTimerFired)
        , m_updateFunction(WTF::move(updateFunction))
    {
        m_updateState.store(UpdateState::Completed);
    }

    void callOnCompositingRunLoop(std::function<void()> function)
    {
        if (&m_runLoop == &RunLoop::current()) {
            function();
            return;
        }

        m_runLoop.dispatch(WTF::move(function));
    }

    void scheduleUpdate()
    {
        if (m_updateState.compareExchangeStrong(UpdateState::Completed, UpdateState::InProgress)) {
            m_updateTimer.startOneShot(0);
            return;
        }

        if (m_updateState.compareExchangeStrong(UpdateState::InProgress, UpdateState::PendingAfterCompletion))
            return;
    }

    void stopUpdates()
    {
        m_updateTimer.stop();
        m_updateState.store(UpdateState::Completed);
    }

    void updateCompleted()
    {
        if (m_updateState.compareExchangeStrong(UpdateState::InProgress, UpdateState::Completed))
            return;

        if (m_updateState.compareExchangeStrong(UpdateState::PendingAfterCompletion, UpdateState::InProgress)) {
            m_updateTimer.startOneShot(0);
            return;
        }

        ASSERT_NOT_REACHED();
    }

    RunLoop& runLoop()
    {
        return m_runLoop;
    }

private:
    enum class UpdateState {
        Completed,
        InProgress,
        PendingAfterCompletion,
    };

    void updateTimerFired()
    {
        m_updateFunction();
    }

    RunLoop& m_runLoop;
    RunLoop::Timer<CompositingRunLoop> m_updateTimer;
    std::function<void()> m_updateFunction;
    Atomic<UpdateState> m_updateState;
};

Ref<ThreadedCompositor> ThreadedCompositor::create(Client* client, WebPage& webPage)
{
    return adoptRef(*new ThreadedCompositor(client, webPage));
}

ThreadedCompositor::ThreadedCompositor(Client* client, WebPage& webPage)
    : m_client(client)
    , m_threadIdentifier(0)
    , m_compositingManager(*this)
#if USE(REQUEST_ANIMATION_FRAME_DISPLAY_MONITOR)
    , m_displayRefreshMonitor(adoptRef(new DisplayRefreshMonitor(*this)))
#endif
{
    m_clientRendersNextFrame.store(false);
    m_coordinateUpdateCompletionWithClient.store(false);
    createCompositingThread();
    m_compositingManager.establishConnection(webPage, m_compositingRunLoop->runLoop());
}

ThreadedCompositor::~ThreadedCompositor()
{
    m_displayRefreshMonitor->invalidate();
    terminateCompositingThread();
}

void ThreadedCompositor::setNeedsDisplay()
{
    m_compositingRunLoop->scheduleUpdate();
}

void ThreadedCompositor::setNativeSurfaceHandleForCompositing(uint64_t handle)
{
    RefPtr<ThreadedCompositor> protector(this);
    callOnCompositingThread([=] {
        protector->m_nativeSurfaceHandle = handle;
        protector->m_scene->setActive(true);
    });
}

void ThreadedCompositor::didChangeViewportSize(const IntSize& newSize)
{
    RefPtr<ThreadedCompositor> protector(this);
    callOnCompositingThread([=] {
        if (protector->m_surface)
            protector->m_surface->resize(newSize);
        protector->viewportController()->didChangeViewportSize(newSize);
    });
}

void ThreadedCompositor::didChangeViewportAttribute(const ViewportAttributes& attr)
{
    RefPtr<ThreadedCompositor> protector(this);
    callOnCompositingThread([=] {
        protector->viewportController()->didChangeViewportAttribute(attr);
    });
}

void ThreadedCompositor::didChangeContentsSize(const IntSize& size)
{
    RefPtr<ThreadedCompositor> protector(this);
    callOnCompositingThread([=] {
        protector->viewportController()->didChangeContentsSize(size);
    });
}

void ThreadedCompositor::scrollTo(const IntPoint& position)
{
    RefPtr<ThreadedCompositor> protector(this);
    callOnCompositingThread([=] {
        protector->viewportController()->scrollTo(position);
    });
}

void ThreadedCompositor::scrollBy(const IntSize& delta)
{
    RefPtr<ThreadedCompositor> protector(this);
    callOnCompositingThread([=] {
        protector->viewportController()->scrollBy(delta);
    });
}

void ThreadedCompositor::purgeBackingStores()
{
    m_client->purgeBackingStores();
}

void ThreadedCompositor::renderNextFrame()
{
    m_client->renderNextFrame();
}

void ThreadedCompositor::updateViewport()
{
    m_compositingRunLoop->scheduleUpdate();
}

void ThreadedCompositor::commitScrollOffset(uint32_t layerID, const IntSize& offset)
{
    m_client->commitScrollOffset(layerID, offset);
}

#if PLATFORM(GBM)
void ThreadedCompositor::destroyBuffer(uint32_t handle)
{
    RELEASE_ASSERT(&RunLoop::current() == &m_compositingRunLoop->runLoop());
    m_compositingManager.destroyPrimeBuffer(handle);
}
#endif

bool ThreadedCompositor::ensureGLContext()
{
    if (!glContext())
        return false;

    glContext()->makeContextCurrent();
    // The window size may be out of sync with the page size at this point, and getting
    // the viewport parameters incorrect, means that the content will be misplaced. Thus
    // we set the viewport parameters directly from the window size.
    IntSize contextSize = glContext()->defaultFrameBufferSize();
    if (m_viewportSize != contextSize) {
        glViewport(0, 0, contextSize.width(), contextSize.height());
        m_viewportSize = contextSize;
    }

    return true;
}

GLContext* ThreadedCompositor::glContext()
{
    if (m_context)
        return m_context.get();

#if PLATFORM(GBM)
    RELEASE_ASSERT(is<PlatformDisplayGBM>(PlatformDisplay::sharedDisplay()));
    m_gbmSurface = downcast<PlatformDisplayGBM>(PlatformDisplay::sharedDisplay())
        .createSurface(IntSize(viewportController()->visibleContentsRect().size()), *this);
    if (!m_gbmSurface)
        return 0;

    setNativeSurfaceHandleForCompositing(0);
    m_context = m_gbmSurface->createGLContext();
#endif

#if PLATFORM(BCM_RPI)
    RELEASE_ASSERT(is<PlatformDisplayBCMRPi>(PlatformDisplay::sharedDisplay()));

    IntSize size(viewportController()->visibleContentsRect().size());
    m_surface = downcast<PlatformDisplayBCMRPi>(PlatformDisplay::sharedDisplay()).createSurface(size,
        m_compositingManager.createBCMElement(size.width(), size.height()));
    if (!m_surface)
        return nullptr;

    setNativeSurfaceHandleForCompositing(0);
    m_context = m_surface->createGLContext();
#endif

    return m_context.get();
}

void ThreadedCompositor::scheduleDisplayImmediately()
{
    m_compositingRunLoop->scheduleUpdate();
}

void ThreadedCompositor::didChangeVisibleRect()
{
    FloatRect visibleRect = viewportController()->visibleContentsRect();
    float scale = viewportController()->pageScaleFactor();
    RefPtr<ThreadedCompositor> protector(this);
    RunLoop::main().dispatch([protector, visibleRect, scale] {
        protector->m_client->setVisibleContentsRect(visibleRect, FloatPoint::zero(), scale);
    });

    scheduleDisplayImmediately();
}

void ThreadedCompositor::renderLayerTree()
{
    if (!m_scene)
        return;

    if (!ensureGLContext())
        return;

#if 0
    if (!downcast<PlatformDisplayGBM>(PlatformDisplay::sharedDisplay()).hasFreeBuffers(*m_gbmSurface))
        return;
#endif

    FloatRect clipRect(0, 0, m_viewportSize.width(), m_viewportSize.height());

    TransformationMatrix viewportTransform;
    FloatPoint scrollPostion = viewportController()->visibleContentsRect().location();
    viewportTransform.scale(viewportController()->pageScaleFactor());
    viewportTransform.translate(-scrollPostion.x(), -scrollPostion.y());

    m_scene->paintToCurrentGLContext(viewportTransform, 1, clipRect, Color::white, false, scrollPostion);

    auto bufferExport = m_surface->lockFrontBuffer();
    m_compositingManager.commitBCMBuffer(bufferExport);

    glContext()->swapBuffers();

    // auto bufferExport = downcast<PlatformDisplayGBM>(PlatformDisplay::sharedDisplay()).lockFrontBuffer(*m_gbmSurface);
    // m_compositingManager.commitPrimeBuffer(bufferExport);
}

void ThreadedCompositor::updateSceneState(const CoordinatedGraphicsState& state)
{
    RefPtr<ThreadedCompositor> protector(this);
    RefPtr<CoordinatedGraphicsScene> scene = m_scene;
    m_scene->appendUpdate([protector, scene, state] {
        scene->commitSceneState(state);

        protector->m_clientRendersNextFrame.store(true);
        bool coordinateUpdate = std::any_of(state.layersToUpdate.begin(), state.layersToUpdate.end(),
            [](const std::pair<CoordinatedLayerID, CoordinatedGraphicsLayerState>& it) {
                return it.second.platformLayerChanged;
            });
        protector->m_coordinateUpdateCompletionWithClient.store(coordinateUpdate);
    });

    setNeedsDisplay();
}

void ThreadedCompositor::callOnCompositingThread(std::function<void()> function)
{
    m_compositingRunLoop->callOnCompositingRunLoop(WTF::move(function));
}

void ThreadedCompositor::compositingThreadEntry(void* coordinatedCompositor)
{
    static_cast<ThreadedCompositor*>(coordinatedCompositor)->runCompositingThread();
}

void ThreadedCompositor::createCompositingThread()
{
    if (m_threadIdentifier)
        return;

    LockHolder locker(m_initializeRunLoopConditionLock);
    m_threadIdentifier = createThread(compositingThreadEntry, this, "WebCore: ThreadedCompositor");

    m_initializeRunLoopCondition.wait(m_initializeRunLoopConditionLock);
}

void ThreadedCompositor::runCompositingThread()
{
    {
        LockHolder locker(m_initializeRunLoopConditionLock);

        m_compositingRunLoop = std::make_unique<CompositingRunLoop>([&] {
            renderLayerTree();
        });
        m_scene = adoptRef(new CoordinatedGraphicsScene(this));
        m_viewportController = std::make_unique<SimpleViewportController>(this);

        m_initializeRunLoopCondition.notifyOne();
    }

    m_compositingRunLoop->runLoop().run();

    m_compositingRunLoop->stopUpdates();
    m_scene->purgeGLResources();

    {
        LockHolder locker(m_terminateRunLoopConditionLock);
        m_compositingRunLoop = nullptr;
        m_context = nullptr;
        m_scene = nullptr;
        m_terminateRunLoopCondition.notifyOne();
    }

    detachThread(m_threadIdentifier);
}

void ThreadedCompositor::terminateCompositingThread()
{
    LockHolder locker(m_terminateRunLoopConditionLock);

    m_scene->detach();
    m_compositingRunLoop->runLoop().stop();

    m_terminateRunLoopCondition.wait(m_terminateRunLoopConditionLock);
}

static void debugThreadedCompositorFPS()
{
    static double lastTime = currentTime();
    static unsigned frameCount = 0;

    double ct = currentTime();
    frameCount++;

    if (ct - lastTime >= 5.0) {
        fprintf(stderr, "ThreadedCompositor: frame callbacks %.2f FPS\n", frameCount / (ct - lastTime));
        lastTime = ct;
        frameCount = 0;
    }
}

#if USE(REQUEST_ANIMATION_FRAME_DISPLAY_MONITOR)
RefPtr<WebCore::DisplayRefreshMonitor> ThreadedCompositor::createDisplayRefreshMonitor(PlatformDisplayID)
{
    return m_displayRefreshMonitor;
}

ThreadedCompositor::DisplayRefreshMonitor::DisplayRefreshMonitor(ThreadedCompositor& compositor)
    : WebCore::DisplayRefreshMonitor(0)
    , m_displayRefreshTimer(RunLoop::main(), this, &ThreadedCompositor::DisplayRefreshMonitor::displayRefreshCallback)
    , m_compositor(&compositor)
{
}

bool ThreadedCompositor::DisplayRefreshMonitor::requestRefreshCallback()
{
    LockHolder locker(mutex());
    setIsScheduled(true);
    return true;
}

bool ThreadedCompositor::DisplayRefreshMonitor::requiresDisplayRefreshCallback()
{
    LockHolder locker(mutex());
    return isScheduled() && isPreviousFrameDone();
}

void ThreadedCompositor::DisplayRefreshMonitor::dispatchDisplayRefreshCallback()
{
    m_displayRefreshTimer.startOneShot(0);
}

void ThreadedCompositor::DisplayRefreshMonitor::invalidate()
{
    m_compositor = nullptr;
}

void ThreadedCompositor::DisplayRefreshMonitor::displayRefreshCallback()
{
    bool shouldHandleDisplayRefreshNotification = false;
    {
        LockHolder locker(mutex());
        shouldHandleDisplayRefreshNotification = isScheduled() && isPreviousFrameDone();
        if (shouldHandleDisplayRefreshNotification) {
            setIsPreviousFrameDone(false);
            setMonotonicAnimationStartTime(monotonicallyIncreasingTime());
        }
    }

    if (shouldHandleDisplayRefreshNotification)
        DisplayRefreshMonitor::handleDisplayRefreshedNotificationOnMainThread(this);

    if (m_compositor) {
        if (m_compositor->m_clientRendersNextFrame.compareExchangeStrong(true, false))
            m_compositor->m_scene->renderNextFrame();
        if (m_compositor->m_coordinateUpdateCompletionWithClient.compareExchangeStrong(true, false))
            m_compositor->m_compositingRunLoop->updateCompleted();
    }
}
#endif

#if PLATFORM(WPE)
void ThreadedCompositor::releaseBuffer(uint32_t handle)
{
    RELEASE_ASSERT(&RunLoop::current() == &m_compositingRunLoop->runLoop());
#if PLATFORM(GBM)
    downcast<PlatformDisplayGBM>(PlatformDisplay::sharedDisplay()).releaseBuffer(*m_gbmSurface, handle);
#endif
#if PLATFORM(BCM_RPI)
    m_surface->releaseBuffer(handle);
#endif
}

void ThreadedCompositor::frameComplete()
{
    RELEASE_ASSERT(&RunLoop::current() == &m_compositingRunLoop->runLoop());
    static bool reportFPS = !!std::getenv("WPE_THREADED_COMPOSITOR_FPS");
    if (reportFPS)
        debugThreadedCompositorFPS();

    bool shouldDispatchDisplayRefreshCallback = m_clientRendersNextFrame.load()
        || m_displayRefreshMonitor->requiresDisplayRefreshCallback();
    bool shouldCoordinateUpdateCompletionWithClient = m_coordinateUpdateCompletionWithClient.load();

    if (shouldDispatchDisplayRefreshCallback)
        m_displayRefreshMonitor->dispatchDisplayRefreshCallback();
    if (!shouldCoordinateUpdateCompletionWithClient)
        m_compositingRunLoop->updateCompleted();
}
#endif

}
#endif // USE(COORDINATED_GRAPHICS_THREADED)
