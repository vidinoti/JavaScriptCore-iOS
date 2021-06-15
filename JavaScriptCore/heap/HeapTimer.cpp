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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
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
#include "HeapTimer.h"

#include "APIShims.h"
#include "JSObject.h"
#include "JSString.h"

#include <wtf/MainThread.h>
#include <wtf/Threading.h>

#if PLATFORM(EFL)
#include <Ecore.h>
#endif

 #if PLATFORM(ANDROID)

#include "VidiModules/Vision/VDARObject.h"

namespace VDAR {

    class RenderingEngine : public VDARObject {
        
    public:

        void addTimer(const Timer t) ;

        void removeTimer(unsigned int tID);
       
        static RenderingEngine* getInstance();

    };
    
}
#endif

namespace JSC {

#if USE(CF) || PLATFORM(ANDROID)

static const void* retainAPILock(const void* info)
{
    static_cast<JSLock*>(const_cast<void*>(info))->ref();
    return info;
}

static void releaseAPILock(const void* info)
{
    static_cast<JSLock*>(const_cast<void*>(info))->deref();
}

#endif

#if USE(CF)
    
const CFTimeInterval HeapTimer::s_decade = 60 * 60 * 24 * 365 * 10;


HeapTimer::HeapTimer(VM* vm, CFRunLoopRef runLoop)
    : m_vm(vm)
    , m_runLoop(runLoop)
{
    memset(&m_context, 0, sizeof(CFRunLoopTimerContext));
    m_context.info = &vm->apiLock();
    m_context.retain = retainAPILock;
    m_context.release = releaseAPILock;
    CFRunLoopAddTimer(m_runLoop.get(), m_timer.get(), kCFRunLoopCommonModes);
    m_timer = adoptCF(CFRunLoopTimerCreate(0, s_decade, s_decade, 0, 0, HeapTimer::timerDidFire, &m_context));
}

HeapTimer::~HeapTimer()
{
    CFRunLoopRemoveTimer(m_runLoop.get(), m_timer.get(), kCFRunLoopCommonModes);
    CFRunLoopTimerInvalidate(m_timer.get());
}

void HeapTimer::timerDidFire(CFRunLoopTimerRef timer, void* context)
{
    dataLog("Heap timer fireing...\n");
    JSLock* apiLock = static_cast<JSLock*>(context);
    apiLock->lock();

    VM* vm = apiLock->vm();
    // The VM has been destroyed, so we should just give up.
    if (!vm) {
        apiLock->unlock();
        return;
    }

    HeapTimer* heapTimer = 0;
    if (vm->heap.activityCallback() && vm->heap.activityCallback()->m_timer.get() == timer)
        heapTimer = vm->heap.activityCallback();
    else if (vm->heap.sweeper()->m_timer.get() == timer)
        heapTimer = vm->heap.sweeper();
    else
        RELEASE_ASSERT_NOT_REACHED();

    {
        APIEntryShim shim(vm);
        dataLog("Heap timer do work...\n");
        heapTimer->doWork();
    }

    apiLock->unlock();
}

#elif PLATFORM(BLACKBERRY)

HeapTimer::HeapTimer(VM* vm)
    : m_vm(vm)
    , m_timer(this, &HeapTimer::timerDidFire)
{
    // FIXME: Implement HeapTimer for other threads.
    if (WTF::isMainThread() && !m_timer.tryCreateClient())
        CRASH();
}

HeapTimer::~HeapTimer()
{
}

void HeapTimer::timerDidFire()
{
    doWork();
}

void HeapTimer::invalidate()
{
}

#elif PLATFORM(ANDROID)



HeapTimer::HeapTimer(VM* vm)
    : m_vm(vm)
{

    JSLock *ptrLock = &vm->apiLock();

    retainAPILock(ptrLock);

    m_timer = new VDAR::Timer( 60 * 60 * 24 * 365 * 10, true, [ptrLock](VDAR::Timer * t) {
        timerDidFire(t,ptrLock);
    }, [ptrLock](VDAR::Timer * t) {
        releaseAPILock(ptrLock);
    });

}

HeapTimer::~HeapTimer()
{
    if(m_timer) {
        if(m_timer->isValid()) {
             VDAR::RenderingEngine::getInstance()->removeTimer(m_timer->getID());
        }
        delete m_timer;
    }
}

void HeapTimer::timerDidFire(VDAR::Timer * timer, void *lock)
{
    JSLock* apiLock = static_cast<JSLock*>(lock);

    apiLock->lock();

    VM* vm = apiLock->vm();
    // The VM has been destroyed, so we should just give up.
    if (!vm) {
        apiLock->unlock();
        return;
    }

    HeapTimer* heapTimer = 0;
    if (vm->heap.activityCallback() && vm->heap.activityCallback()->m_timer->getID() == timer->getID())
        heapTimer = vm->heap.activityCallback();
    else if (vm->heap.sweeper()->m_timer->getID() == timer->getID())
        heapTimer = vm->heap.sweeper();
    else
        RELEASE_ASSERT_NOT_REACHED();

    {
        APIEntryShim shim(vm);
        heapTimer->doWork();
    }

    apiLock->unlock();
}

void HeapTimer::invalidate()
{
}

#elif PLATFORM(EFL)

HeapTimer::HeapTimer(VM* vm)
    : m_vm(vm)
    , m_timer(0)
{
}

HeapTimer::~HeapTimer()
{
    stop();
}

Ecore_Timer* HeapTimer::add(double delay, void* agent)
{
    return ecore_timer_add(delay, reinterpret_cast<Ecore_Task_Cb>(timerEvent), agent);
}
    
void HeapTimer::stop()
{
    if (!m_timer)
        return;

    ecore_timer_del(m_timer);
    m_timer = 0;
}

bool HeapTimer::timerEvent(void* info)
{
    HeapTimer* agent = static_cast<HeapTimer*>(info);
    
    APIEntryShim shim(agent->m_vm);
    agent->doWork();
    agent->m_timer = 0;
    
    return ECORE_CALLBACK_CANCEL;
}
#else
HeapTimer::HeapTimer(VM* vm)
    : m_vm(vm)
{
}

HeapTimer::~HeapTimer()
{
}

void HeapTimer::invalidate()
{
}

#endif
    

} // namespace JSC
