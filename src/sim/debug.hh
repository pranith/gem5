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

#ifndef __SIM_DEBUG_HH__
#define __SIM_DEBUG_HH__

#include "base/types.hh"

/** @file This file provides the definitions for some useful debugging
 * functions. These are intended to be called from a debugger such as
 * gdb.
 */

namespace gem5
{

/** Cause the simulator to execute a breakpoint
 * @param when the tick to break
 */
void schedBreak(Tick when);

/**
 * Cause the simulator to execute a breakpoint
 * relative to the current tick.
 * @param delta the number of ticks to execute until breaking
 */
void schedRelBreak(Tick delta);

/** Cause the simulator to return to python to create a checkpoint
 * @param when the cycle to break
 */
void takeCheckpoint(Tick when);

/** Dump all the events currently on the event queue
 */
void eventqDump();

/** Arm a decode-triggered trace enable at the given instruction seqNum */
void setDebugStartSeqNum(uint64_t seq_num);

/** Return the current decode-triggered seqNum (0 when disabled) */
uint64_t getDebugStartSeqNum();

/** Set the CPU id to match for decode-triggered tracing (-1 means any CPU). */
void setDebugStartCpu(int cpu_id);

/** Return the CPU id for decode-triggered tracing (-1 means any CPU). */
int getDebugStartCpu();

/**
 * Check whether a given seqNum should enable tracing. Returns true once,
 * clearing the trigger after it fires.
 */
bool consumeDebugStartSeqNum(uint64_t seq_num, int cpu_id);

} // namespace gem5

#endif // __SIM_DEBUG_HH__
