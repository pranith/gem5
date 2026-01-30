/*
 * Copyright (c) 2003-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "sim/debug.hh"

#include <atomic>
#include <string>
#include <vector>

#include "base/debug.hh"
#include "sim/eventq.hh"
#include "sim/global_event.hh"
#include "sim/sim_events.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

//
// Debug event: place a breakpoint on the process function and
// schedule the event to break at a particular cycle
//
struct DebugBreakEvent : public GlobalEvent
{
    DebugBreakEvent(Tick when);
    void process();     // process event
    virtual const char *description() const;
};

//
// constructor: schedule at specified time
//
DebugBreakEvent::DebugBreakEvent(Tick when)
    : GlobalEvent(when, Debug_Break_Pri, AutoDelete)
{
}

//
// handle debug event: set debugger breakpoint on this function
//
void
DebugBreakEvent::process()
{
    debug::breakpoint();
}


const char *
DebugBreakEvent::description() const
{
    return "debug breakpoint";
}

//
// handy function to schedule DebugBreakEvent on main event queue
// (callable from debugger)
//
void
schedBreak(Tick when)
{
    new DebugBreakEvent(when);
    warn("need to stop all queues");
}

void
schedRelBreak(Tick delta)
{
    schedBreak(curTick() + delta);
}

///
/// Function to cause the simulator to take a checkpoint from the debugger
///
void
takeCheckpoint(Tick when)
{
    if (!when)
        when = curTick() + 1;
    exitSimLoop("checkpoint", 0, when, 0);
}

void
eventqDump()
{
    for (uint32_t i = 0; i < numMainEventQueues; ++i) {
        mainEventQueue[i]->dump();
    }
}

namespace
{
std::atomic<uint64_t> debugStartSeqNum{0};
std::atomic<int> debugStartCpu{-1};
} // namespace

void
setDebugStartSeqNum(uint64_t seq_num)
{
    debugStartSeqNum.store(seq_num, std::memory_order_relaxed);
}

uint64_t
getDebugStartSeqNum()
{
    return debugStartSeqNum.load(std::memory_order_relaxed);
}

void
setDebugStartCpu(int cpu_id)
{
    debugStartCpu.store(cpu_id, std::memory_order_relaxed);
}

int
getDebugStartCpu()
{
    return debugStartCpu.load(std::memory_order_relaxed);
}

bool
consumeDebugStartSeqNum(uint64_t seq_num, int cpu_id)
{
    const uint64_t target = debugStartSeqNum.load(std::memory_order_relaxed);
    const int target_cpu = debugStartCpu.load(std::memory_order_relaxed);
    if (target && seq_num == target &&
        (target_cpu < 0 || cpu_id == target_cpu)) {
        debugStartSeqNum.store(0, std::memory_order_relaxed);
        debugStartCpu.store(-1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

} // namespace gem5
