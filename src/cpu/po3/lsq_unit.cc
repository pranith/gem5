/*
 * Copyright (c) 2010-2014, 2017-2021, 2025 Arm Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "cpu/po3/lsq_unit.hh"

#include "arch/generic/debugfaults.hh"
#include "arch/generic/mmu.hh"
#include "base/cprintf.hh"
#include "base/logging.hh"
#include "base/stats/group.hh"
#include "base/stats/info.hh"
#include "base/stats/units.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/inst_seq.hh"
#include "cpu/po3/cpu.hh"
#include "cpu/po3/dyn_inst.hh"
#include "cpu/po3/dyn_inst_ptr.hh"
#include "cpu/po3/limits.hh"
#include "cpu/po3/lsq.hh"
#include "debug/HtmCpu.hh"
#include "debug/LSQUnit.hh"
#include "mem/htm.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/faults.hh"

#include <vector>

namespace gem5
{

namespace po3
{

namespace
{

uint64_t
extractValue(const uint8_t *data, size_t size)
{
    uint64_t value = 0;
    const size_t limit = size < sizeof(value) ? size : sizeof(value);

    for (size_t i = 0; i < limit; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }

    return value;
}

CachePort
readPortFor(MemPipe pipe)
{
    switch (pipe) {
      case MemPipe::Load:
        return CachePort::LoadPipeRead;
      case MemPipe::LoadStore0:
        return CachePort::LoadStorePipe0Read;
      case MemPipe::LoadStore1:
        return CachePort::LoadStorePipe1Read;
      case MemPipe::Unassigned:
        panic("Memory request reached the cache without a memory pipe");
    }
    panic("Unknown PO3 memory pipe");
}

unsigned
storePipeIndex(MemPipe pipe)
{
    switch (pipe) {
      case MemPipe::LoadStore0:
        return 0;
      case MemPipe::LoadStore1:
        return 1;
      case MemPipe::Load:
        panic("Load-only pipe cannot issue a store");
      case MemPipe::Unassigned:
        panic("Store reached the merge buffer without a memory pipe");
    }
    panic("Unknown PO3 memory pipe");
}

} // namespace

LSQUnit::WritebackEvent::WritebackEvent(const DynInstPtr &_inst,
                                        PacketPtr _pkt, LSQUnit *lsq_ptr,
                                        bool delete_pkt)
    : Event(Default_Pri, AutoDelete),
      inst(_inst),
      pkt(_pkt),
      lsqPtr(lsq_ptr),
      deletePkt(delete_pkt)
{
    assert(_inst->savedRequest);
    _inst->savedRequest->writebackScheduled();
}

void
LSQUnit::WritebackEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    lsqPtr->writeback(inst, pkt);

    assert(inst->savedRequest);
    inst->savedRequest->writebackDone();
    if (deletePkt) {
        delete pkt;
    }
}

const char *
LSQUnit::WritebackEvent::description() const
{
    return "Store writeback";
}

bool
LSQUnit::recvTimingResp(PacketPtr pkt)
{
    if (auto *pf_state =
            dynamic_cast<MergeBufferPrefetchSenderState *>(pkt->senderState)) {
        assert(mergeBufferPfInFlight > 0);
        --mergeBufferPfInFlight;
        delete pf_state;
        delete pkt;
        return true;
    } else if (auto *mb_state = dynamic_cast<MergeBufferDrainSenderState *>(
                   pkt->senderState)) {
        if (mb_state->entry && mb_state->entry->isAtomic) {
            LSQRequest *req = mb_state->entry->atomicReq;
            if (req && !req->isReleased()) {
                DynInstPtr inst = req->instruction();
                if (pkt && pkt->getSize() > 0) {
                    const uint64_t val = extractValue(
                        pkt->getConstPtr<uint8_t>(), pkt->getSize());
                    DPRINTF(LSQUnit,
                            "Atomic drain resp writeback [sn:%llu] PC %s "
                            "val:%#x size:%u addr:%#x\n",
                            inst->seqNum, inst->pcState(), val, pkt->getSize(),
                            pkt->getAddr());
                }
                writeback(inst, pkt);
                req->writebackDone();
                completeStore(inst->sqIt);
                if (storeQueue.empty()) {
                    storeWBIt = storeQueue.end();
                } else {
                    storeWBIt = storeQueue.begin();
                }
            }
        }
        ++stats.mbDrainHitMiss[pkt->req->isL1DCacheHit() ? 0 : 1];
        mergeBuffer.handleDrainResp(mb_state->entry, this);
        delete mb_state;
        delete pkt;
        return true;
    } else {
        LSQRequest *request = dynamic_cast<LSQRequest *>(pkt->senderState);
        assert(request != nullptr);
        bool ret = true;
        /* Check that the request is still alive before any further action. */
        if (!request->isReleased()) {
            ret = request->recvTimingResp(pkt);
        }
        return ret;
    }
}

void
LSQUnit::handleMBDrain(MergeBuffer::MergeBufferEntry *entry)
{
    if (!entry) {
        return;
    }

    if (isStalled() && entry->blockAddr == stallingMBAddr) {
        DPRINTF(LSQUnit,
                "Unstalling, stalling load [sn:%lli] "
                "load idx:%li MB addr:%#x\n",
                loadQueue[stallingLoadIdx].instruction()->seqNum,
                stallingLoadIdx, stallingMBAddr);
        stalled = false;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }
}

void
LSQUnit::completeDataAccess(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest *>(pkt->senderState);
    DynInstPtr inst = request->instruction();

    // hardware transactional memory
    // sanity check
    if (pkt->isHtmTransactional() && !inst->isSquashed()) {
        assert(inst->getHtmTransactionUid() == pkt->getHtmTransactionUid());
    }

    // if in a HTM transaction, it's possible
    // to abort within the cache hierarchy.
    // This is signalled back to the processor
    // through responses to memory requests.
    if (pkt->htmTransactionFailedInCache()) {
        // cannot do this for write requests because
        // they cannot tolerate faults
        const HtmCacheFailure htm_rc = pkt->getHtmTransactionFailedInCacheRC();
        if (pkt->isWrite()) {
            DPRINTF(HtmCpu,
                    "store notification (ignored) of HTM transaction failure "
                    "in cache - addr=0x%lx - rc=%s - htmUid=%d\n",
                    pkt->getAddr(), htmFailureToStr(htm_rc),
                    pkt->getHtmTransactionUid());
        } else {
            HtmFailureFaultCause fail_reason = HtmFailureFaultCause::INVALID;

            if (htm_rc == HtmCacheFailure::FAIL_SELF) {
                fail_reason = HtmFailureFaultCause::SIZE;
            } else if (htm_rc == HtmCacheFailure::FAIL_REMOTE) {
                fail_reason = HtmFailureFaultCause::MEMORY;
            } else if (htm_rc == HtmCacheFailure::FAIL_OTHER) {
                // these are likely loads that were issued out of order
                // they are faulted here, but it's unlikely that these will
                // ever reach the commit head.
                fail_reason = HtmFailureFaultCause::OTHER;
            } else {
                panic("HTM error - unhandled return code from cache (%s)",
                      htmFailureToStr(htm_rc));
            }

            inst->fault = std::make_shared<GenericHtmFailureFault>(
                inst->getHtmTransactionUid(), fail_reason);

            DPRINTF(HtmCpu,
                    "load notification of HTM transaction failure "
                    "in cache - pc=%s - addr=0x%lx - "
                    "rc=%u - htmUid=%d\n",
                    inst->pcState(), pkt->getAddr(), htmFailureToStr(htm_rc),
                    pkt->getHtmTransactionUid());
        }
    }

    cpu->ppDataAccessComplete->notify(std::make_pair(inst, pkt));

    assert(!cpu->switchedOut());
    if (!inst->isSquashed()) {
        if (request->needWBToRegister()) {
            // Only loads, store conditionals and atomics perform the writeback
            // after receving the response from the memory
            assert(inst->isLoad() || inst->isStoreConditional() ||
                   inst->isAtomic());

            // hardware transactional memory
            if (pkt->htmTransactionFailedInCache()) {
                request->mainPacket()->setHtmTransactionFailedInCache(
                    pkt->getHtmTransactionFailedInCacheRC());
            }

            if (po3MemPipeline && inst->isLoad()) {
                schedulePO3Writeback(inst, request->mainPacket());
            } else {
                writeback(inst, request->mainPacket());
            }
            if (inst->isStore() || inst->isAtomic()) {
                request->writebackDone();
                completeStore(request->instruction()->sqIt);
            }
        } else if (inst->isStore()) {
            // This is a regular store (i.e., not store conditionals and
            // atomics), so it can complete without writing back
            completeStore(request->instruction()->sqIt);
        }
    }
}

LSQUnit::LSQUnit(uint32_t lqEntries, uint32_t sqEntries)
    : lsqID(-1),
      storeQueue(sqEntries),
      loadQueue(lqEntries),
      storesToWB(0),
      htmStarts(0),
      htmStops(0),
      lastRetiredHtmUid(0),
      cacheBlockMask(0),
      stalled(false),
      isStoreBlocked(false),
      storeInFlight(false),
      stats(nullptr)
{}

void
LSQUnit::init(CPU *cpu_ptr, IEW *iew_ptr, const BasePO3CPUParams &params,
              LSQ *lsq_ptr, unsigned id)
{
    lsqID = id;

    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    cpu->addStatGroup(csprintf("lsq%i", lsqID).c_str(), &stats);

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n", lsqID);

    depCheckShift = params.LSQDepCheckShift;
    checkLoads = params.LSQCheckLoads;
    needsTSO = params.needsTSO;
    po3MemPipeline = params.po3MemPipeline;
    po3MemAddrGenLatency = params.po3MemAddrGenLatency;
    po3MemTLBLookupLatency = params.po3MemTLBLookupLatency;
    po3MemCacheAccessLatency = params.po3MemCacheAccessLatency;
    po3MemWritebackLatency = params.po3MemWritebackLatency;
    po3MemAddrGenWidth = params.po3MemAddrGenWidth;
    po3MemTLBLookupWidth = params.po3MemTLBLookupWidth;
    po3MemCacheAccessWidth = params.po3MemCacheAccessWidth;

    mergeBufferEnabled = params.useMergeBuffer;
    mergeBufferSqPressureThreshold = params.mergeBufferSqPressureThreshold;
    fatal_if(mergeBufferSqPressureThreshold > 100,
             "mergeBufferSqPressureThreshold must be in [0, 100]");
    mergeBufferFreeEntryPressureThreshold =
        params.mergeBufferFreeEntryPressureThreshold;
    mergeBufferPrefetchEnabled = params.mergeBufferPrefetch;
    mergeBufferPfInFlight = 0;
    mbRetireWhenFullValid = params.mbRetireWhenFullValid;
    optimizeStoreRelease = params.optimizeStoreRelease;

    storeDeallocateWidth = params.storeDeallocateWidth;
    storeDeallocsThisCycle = 0;
    lastStoreDeallocCycle = cpu->curCycle();
    mbStorePipe0Used = false;
    mbStorePipe1Used = false;

    if (mergeBufferEnabled) {
        mergeBuffer.init(this, params.mergeBufferEntries, cacheLineSize(),
                         params.mergeBufferRetireCycles,
                         params.mergeBufferResetRetireOnMerge,
                         params.mergeBufferRetireResetCycles,
                         params.mergeBufferMaxUnretire);
    }

    resetState();
}

void
LSQUnit::resetState()
{
    storesToWB = 0;

    if (mergeBufferEnabled) {
        mergeBuffer.reset();
    }

    // hardware transactional memory
    // nesting depth
    htmStarts = htmStops = 0;

    storeWBIt = storeQueue.begin();

    retryPkt = NULL;
    memDepViolator = NULL;
    memOrderViolator = NULL;
    po3MemStageCycles.clear();
    po3AddrGenStage.clear();
    po3TLBLookupStage.clear();
    po3CacheAccessStage.clear();
    po3MemPipelineInsts.clear();
    partialWriteRMWs.clear();

    stalled = false;

    cacheBlockMask = ~(cpu->cacheLineSize() - 1);
}

std::string
LSQUnit::name() const
{
    if (MaxThreads == 1) {
        return iewStage->name() + ".lsq";
    } else {
        return iewStage->name() + ".lsq.thread" + std::to_string(lsqID);
    }
}

LSQUnit::LSQUnitStats::LSQUnitStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(forwLoads, statistics::units::Count::get(),
               "Number of loads that had data forwarded from stores"),
      ADD_STAT(squashedLoads, statistics::units::Count::get(),
               "Number of loads squashed"),
      ADD_STAT(ignoredResponses, statistics::units::Count::get(),
               "Number of memory responses ignored because the instruction is "
               "squashed"),
      ADD_STAT(memOrderViolation, statistics::units::Count::get(),
               "Number of memory ordering violations"),
      ADD_STAT(tsoLoadCompletionReschedules, statistics::units::Count::get(),
               "TSO loads replayed when an older load completed after an "
               "L1 eviction or coherence hazard"),
      ADD_STAT(tsoL1EvictionHazards, statistics::units::Count::get(),
               "Executed TSO loads marked hazardous by an L1D replacement"),
      ADD_STAT(possibleConsistencyViolation, statistics::units::Count::get(),
               "Number of possible consistency violations detected"),
      ADD_STAT(squashedStores, statistics::units::Count::get(),
               "Number of stores squashed"),
      ADD_STAT(rescheduledLoads, statistics::units::Count::get(),
               "Number of loads that were rescheduled"),
      ADD_STAT(sqPartialFwdRescheduledLoads, statistics::units::Count::get(),
               "Number of loads rescheduled due to partial store queue "
               "forwarding"),
      ADD_STAT(mbPartialFwdRescheduledLoads, statistics::units::Count::get(),
               "Number of loads rescheduled due to partial merge buffer "
               "forwarding"),
      ADD_STAT(blockedByCache, statistics::units::Count::get(),
               "Number of times an access to memory failed due to the cache "
               "being blocked"),
      ADD_STAT(loadToUse, statistics::units::Cycle::get(),
               "Distribution of cycle latency between the "
               "first time a load is issued and its completion"),
      ADD_STAT(addedLoadsAndStores, statistics::units::Count::get(),
               "Number of loads and stores written to the Load Store Queue"),
      ADD_STAT(lqAvgOccupancy, statistics::units::Ratio::get(),
               "Average LQ Occupancy (UsedSlots/TotalSlots)"),
      ADD_STAT(sqAvgOccupancy, statistics::units::Ratio::get(),
               "Average SQ Occupancy (UsedSlots/TotalSlots)"),
      ADD_STAT(mbAllocations, statistics::units::Count::get(),
               "Number of merge buffer entries allocated"),
      ADD_STAT(mbMerges, statistics::units::Count::get(),
               "Number of stores merged into existing merge buffer entries"),
      ADD_STAT(mbTsoBlockedMergeOpportunities, statistics::units::Count::get(),
               "TSO merge opportunities blocked without version reasoning"),
      ADD_STAT(mbTsoMergeBlockedByAllocationOrder,
               statistics::units::Count::get(),
               "TSO merges blocked by the newest-allocation constraint"),
      ADD_STAT(mbTsoBlockedMergesUnderSqPressure,
               statistics::units::Count::get(),
               "Blocked TSO merges while SQ occupancy met its threshold"),
      ADD_STAT(mbTsoBlockedMergesUnderMbPressure,
               statistics::units::Count::get(),
               "Blocked TSO merges while MB free entries were below their "
               "threshold"),
      ADD_STAT(mbRetired, statistics::units::Count::get(),
               "Number of merge buffer entries retired"),
      ADD_STAT(mbDrains, statistics::units::Count::get(),
               "Number of merge buffer entries drained to cache"),
      ADD_STAT(mbDrainLatency, statistics::units::Cycle::get(),
               "Cycles from merge-buffer drain issue to response"),
      ADD_STAT(mbDrainHitMiss, statistics::units::Count::get(),
               "Merge-buffer drain L1D hit/miss distribution"),
      ADD_STAT(mbTsoStoreInFlightDrainStallCycles,
               statistics::units::Cycle::get(),
               "Cycles a ready TSO drain waited for storeInFlight"),
      ADD_STAT(mbCacheWritePortUses, statistics::units::Count::get(),
               "Merge buffer drains issued through its cache write port"),
      ADD_STAT(mbLoadStorePipe0Writes, statistics::units::Count::get(),
               "Stores written into the merge buffer by load/store pipe 0"),
      ADD_STAT(mbLoadStorePipe1Writes, statistics::units::Count::get(),
               "Stores written into the merge buffer by load/store pipe 1"),
      ADD_STAT(mbDualPipeWriteCycles, statistics::units::Count::get(),
               "Cycles in which both load/store pipes wrote into the merge "
               "buffer"),
      ADD_STAT(mbUnretire, statistics::units::Count::get(),
               "Number of merge buffer entries unretired from RETIRED state"),
      ADD_STAT(mbForwards, statistics::units::Count::get(),
               "Number of loads forwarded from merge buffer"),
      ADD_STAT(mbFullStoreDeallocStalls, statistics::units::Count::get(),
               "Stores blocked from dealloc because merge buffer is full"),
      ADD_STAT(mbSqHeadBlockedCycles, statistics::units::Cycle::get(),
               "Cycles the SQ head could not enter the merge buffer"),
      ADD_STAT(mbFullEvents, statistics::units::Count::get(),
               "MB-full rejection events at the SQ head"),
      ADD_STAT(mbFullFrontendStalledEvents, statistics::units::Count::get(),
               "MB-full events when fetch or rename was already stalled"),
      ADD_STAT(mbFullFrontendStallFraction, statistics::units::Ratio::get(),
               "Fraction of MB-full events overlapping a frontend stall"),
      ADD_STAT(mbForceRetiresOlderVersion, statistics::units::Count::get(),
               "MB entries force retired due to lower version on same block"),
      ADD_STAT(mbVersionAdvanceForceRetires, statistics::units::Count::get(),
               "MB entries force retired by version advancement"),
      ADD_STAT(mbAvgOccupancy, statistics::units::Ratio::get(),
               "Average merge buffer occupancy (UsedEntries/TotalEntries)"),
      ADD_STAT(mbResidencyCycles, statistics::units::Count::get(),
               "Total cycles entries reside in the merge buffer"),
      ADD_STAT(mbPrefetchToDrainLeadTime, statistics::units::Cycle::get(),
               "Cycles from successful MB prefetch issue to drain issue"),
      ADD_STAT(barrierSqStallCycles, statistics::units::Count::get(),
               "Cycles store WB/dealloc stalled by a barrier at the head"),
      ADD_STAT(barrierSqStallOccupancy, statistics::units::Count::get(),
               "Sum of SQ occupancy during barrier stall cycles"),
      ADD_STAT(mbReleaseWaitCycles, statistics::units::Count::get(),
               "Cycles release MB entries waited for outstanding bytes"),
      ADD_STAT(mbVersionLoadStallCycles, statistics::units::Count::get(),
               "Cycles loads waited on older merge buffer versions"),
      ADD_STAT(barrierReschedulesLSQ, statistics::units::Count::get(),
               "Instructions rescheduled/replayed due to barrier in LSQ")
{
    loadToUse.init(0, 299, 10).flags(statistics::nozero);
    mbDrainLatency.init(0, 299, 10).flags(statistics::nozero);
    mbDrainHitMiss.init(2);
    mbDrainHitMiss.subname(0, "hit");
    mbDrainHitMiss.subname(1, "miss");
    mbPrefetchToDrainLeadTime.init(0, 299, 10).flags(statistics::nozero);

    mbFullFrontendStallFraction.precision(4);
    mbFullFrontendStallFraction = mbFullFrontendStalledEvents / mbFullEvents;

    lqAvgOccupancy.precision(2);

    sqAvgOccupancy.precision(2);

    mbAvgOccupancy.precision(2);
}

void
LSQUnit::setDcachePort(RequestPort *dcache_port)
{
    dcachePort = dcache_port;
}

void
LSQUnit::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.capacity(); ++i) {
        assert(!loadQueue[i].valid());
    }

    assert(storesToWB == 0);
    assert(!retryPkt);
    assert(po3MemPipelineInsts.empty());
    assert(po3MemStageCycles.empty());
    assert(po3AddrGenStage.empty());
    assert(po3TLBLookupStage.empty());
    assert(po3CacheAccessStage.empty());
    assert(partialWriteRMWs.empty());
    assert(!mergeBufferEnabled || mergeBuffer.isEmpty());
}

void
LSQUnit::takeOverFrom()
{
    resetState();
}

bool
LSQUnit::isInPO3MemPipeline(const DynInstPtr &inst) const
{
    return po3MemPipelineInsts.count(inst->seqNum) != 0;
}

void
LSQUnit::enqueuePO3MemInst(const DynInstPtr &inst)
{
    if (isInPO3MemPipeline(inst)) {
        return;
    }

    DPRINTF(LSQUnit, "PO3 enqueue addr-gen for [sn:%lli] PC %s\n",
            inst->seqNum, inst->pcState());

    po3MemPipelineInsts.insert(inst->seqNum);
    po3MemStageCycles[inst->seqNum] = po3MemAddrGenLatency;
    po3AddrGenStage.push_back(inst);
    cpu->activityThisCycle();
    cpu->wakeCPU();
}

void
LSQUnit::retirePO3MemStage(
    std::deque<DynInstPtr> &stage, unsigned width, const char *stage_name,
    const std::function<void(const DynInstPtr &)> &retire)
{
    std::vector<DynInstPtr> ready_insts;
    unsigned retired = 0;
    auto it = stage.begin();

    while (it != stage.end() && retired < width) {
        DynInstPtr inst = *it;

        if (inst->isSquashed()) {
            DPRINTF(LSQUnit, "PO3 dropping squashed %s [sn:%lli]\n",
                    stage_name, inst->seqNum);
            po3MemStageCycles.erase(inst->seqNum);
            po3MemPipelineInsts.erase(inst->seqNum);
            it = stage.erase(it);
            continue;
        }

        auto cycles_it = po3MemStageCycles.find(inst->seqNum);
        assert(cycles_it != po3MemStageCycles.end());

        if (cycles_it->second > Cycles(1)) {
            --cycles_it->second;
            ++it;
            continue;
        }

        DPRINTF(LSQUnit, "PO3 retire %s [sn:%lli] PC %s\n", stage_name,
                inst->seqNum, inst->pcState());

        po3MemStageCycles.erase(cycles_it);
        it = stage.erase(it);
        ready_insts.push_back(inst);
        retired++;
    }

    for (const auto &inst : ready_insts) {
        retire(inst);
    }
}

void
LSQUnit::advancePO3MemPipeline()
{
    retirePO3MemStage(
        po3CacheAccessStage, po3MemCacheAccessWidth, "cache-access",
        [this](const DynInstPtr &inst) { issuePO3CacheAccess(inst); });

    retirePO3MemStage(po3TLBLookupStage, po3MemTLBLookupWidth, "tlb-lookup",
                      [this](const DynInstPtr &inst) {
                          if (inst->translationCompleted() ||
                              !inst->readMemAccPredicate()) {
                              po3MemStageCycles[inst->seqNum] =
                                  po3MemCacheAccessLatency;
                              po3CacheAccessStage.push_back(inst);
                          } else {
                              requeuePO3MemInst(inst);
                          }
                      });

    retirePO3MemStage(
        po3AddrGenStage, po3MemAddrGenWidth, "addr-gen",
        [this](const DynInstPtr &inst) {
            Fault fault = inst->initiateAcc();
            if (fault != NoFault) {
                inst->getFault() = fault;
            }

            if (inst->getFault() != NoFault || !inst->readPredicate() ||
                !inst->readMemAccPredicate() || inst->translationCompleted()) {
                po3MemStageCycles[inst->seqNum] = po3MemTLBLookupLatency;
                po3TLBLookupStage.push_back(inst);
            } else {
                requeuePO3MemInst(inst);
            }
        });
}

void
LSQUnit::tick()
{
    if (po3MemPipeline) {
        advancePO3MemPipeline();
        if (!po3MemPipelineInsts.empty()) {
            cpu->activityThisCycle();
        }
    }
}

void
LSQUnit::reserveRMWWriteBank()
{
    if (!partialWriteRMWs.empty()) {
        cpu->activityThisCycle();
    }

    for (auto &[pkt, state] : partialWriteRMWs) {
        state.writeReserved = false;
        if (state.cyclesUntilWrite > Cycles(0)) {
            --state.cyclesUntilWrite;
        }
        if (state.cyclesUntilWrite > Cycles(0) || lsq->cacheBlocked()) {
            continue;
        }

        const CachePort port = state.writePort;
        const bool port_available = lsq->cachePortAvailable(port);
        const bool bank_available =
            port_available && lsq->cacheBankAvailable(port, pkt->getAddr());
        if (bank_available) {
            lsq->cachePortBusy(port, pkt->getAddr());
            state.writeReserved = true;
            DPRINTF(LSQUnit,
                    "Reserved merge-buffer-port RMW write at addr %#x\n",
                    pkt->getAddr());
        } else if (port_available) {
            lsq->cacheBankConflict();
            DPRINTF(LSQUnit,
                    "D-cache bank conflict for partial-write RMW write "
                    "at addr %#x\n",
                    pkt->getAddr());
        }
    }
}

void
LSQUnit::requeuePO3MemInst(const DynInstPtr &inst)
{
    po3MemPipelineInsts.insert(inst->seqNum);
    po3MemStageCycles[inst->seqNum] = Cycles(1);
    po3TLBLookupStage.push_back(inst);
    cpu->wakeCPU();
}

void
LSQUnit::issuePO3CacheAccess(const DynInstPtr &inst)
{
    po3MemPipelineInsts.erase(inst->seqNum);

    if (inst->isSquashed()) {
        return;
    }

    if ((inst->isDataPrefetch() || inst->isInstPrefetch()) &&
        inst->getFault() != NoFault) {
        DPRINTF(LSQUnit,
                "Suppressing fault on prefetch [sn:%lli] PC %s\n",
                inst->seqNum, inst->pcState());
        inst->getFault() = NoFault;
        inst->setExecuted();
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return;
    }

    if (inst->getFault() != NoFault || !inst->readPredicate()) {
        if (!inst->readPredicate()) {
            inst->forwardOldRegs();
        }
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return;
    }

    if (!inst->readMemAccPredicate()) {
        if (inst->isLoad()) {
            inst->setExecuted();
            iewStage->instToCommit(inst);
            iewStage->activityThisCycle();

            if (inst->lqIdx >= 0) {
                LSQRequest *request = loadQueue[inst->lqIdx].request();
                if (request) {
                    loadQueue[inst->lqIdx].setRequest(nullptr);
                    request->discard();
                    inst->clearRequest();
                }
            }
        } else if (inst->isStore() && !inst->isStoreConditional() &&
                   !inst->isAtomic()) {
            inst->setExecuted();
            iewStage->instToCommit(inst);
            iewStage->activityThisCycle();
        }
        return;
    }

    if (inst->isLoad()) {
        LSQRequest *request = inst->savedRequest;
        assert(request);
        Fault fault = read(request, inst->lqIdx);
        if (fault != NoFault) {
            inst->getFault() = fault;
        }
    } else {
        typename LoadQueue::iterator load_it = inst->lqIt;

        if (storeQueue[inst->sqIdx].size() == 0) {
            DPRINTF(LSQUnit, "PO3 fault/zero-size store PC %s [sn:%lli]\n",
                    inst->pcState(), inst->seqNum);

            if (inst->isAtomic()) {
                if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
                    inst->isAtCommit()) {
                    inst->setExecuted();
                }
                iewStage->instToCommit(inst);
                iewStage->activityThisCycle();
            }
            if (inst->isStore() && !inst->isStoreConditional() &&
                !inst->isAtomic()) {
                inst->setExecuted();
                iewStage->instToCommit(inst);
                iewStage->activityThisCycle();
            }
            return;
        }

        assert(inst->getFault() == NoFault);

        if (inst->isStoreConditional() || inst->isAtomic()) {
            storeQueue[inst->sqIdx].canWB() = true;
            ++storesToWB;
        }

        Fault fault = checkViolations(load_it, inst);
        if (fault != NoFault) {
            inst->getFault() = fault;
            iewStage->checkMemOrderViolation(inst);
            inst->getFault() = NoFault;
        }

        if (!inst->isStoreConditional() && !inst->isAtomic()) {
            inst->setExecuted();
            iewStage->instToCommit(inst);
            iewStage->activityThisCycle();
        }
    }
}

void
LSQUnit::schedulePO3Writeback(const DynInstPtr &inst, PacketPtr pkt)
{
    WritebackEvent *wb = new WritebackEvent(inst, pkt, this, false);
    cpu->schedule(wb, cpu->clockEdge(po3MemWritebackLatency));
}

void
LSQUnit::insert(const DynInstPtr &inst)
{
    assert(inst->isMemRef());

    assert(inst->isLoad() || inst->isStore() || inst->isAtomic());

    if (inst->isLoad()) {
        insertLoad(inst);
    } else {
        insertStore(inst);
    }

    inst->setInLSQ();
}

void
LSQUnit::insertLoad(const DynInstPtr &load_inst)
{
    assert(!loadQueue.full());
    assert(loadQueue.size() < loadQueue.capacity());
    ++stats.addedLoadsAndStores;

    DPRINTF(LSQUnit, "Inserting load PC %s, idx:%i [sn:%lli]\n",
            load_inst->pcState(), loadQueue.tail(), load_inst->seqNum);

    /* Grow the queue. */
    loadQueue.advance_tail();

    load_inst->sqIt = storeQueue.end();

    assert(!loadQueue.back().valid());
    loadQueue.back().set(load_inst);
    load_inst->lqIdx = loadQueue.tail();
    assert(load_inst->lqIdx > 0);
    load_inst->lqIt = loadQueue.getIterator(load_inst->lqIdx);

    stats.lqAvgOccupancy = queueOccupancy(loadQueue);

    // hardware transactional memory
    // transactional state and nesting depth must be tracked
    // in the in-order part of the core.
    if (load_inst->isHtmStart()) {
        htmStarts++;
        DPRINTF(HtmCpu, ">> htmStarts++ (%d) : htmStops (%d)\n", htmStarts,
                htmStops);

        const int htm_depth = htmStarts - htmStops;
        const auto &htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
        auto htm_uid = htm_cpt->getHtmUid();

        // for debugging purposes
        if (!load_inst->inHtmTransactionalState()) {
            htm_uid = htm_cpt->newHtmUid();
            DPRINTF(HtmCpu, "generating new htmUid=%u\n", htm_uid);
            if (htm_depth != 1) {
                DPRINTF(HtmCpu,
                        "unusual HTM transactional depth (%d)"
                        " possibly caused by mispeculation - htmUid=%u\n",
                        htm_depth, htm_uid);
            }
        }
        load_inst->setHtmTransactionalState(htm_uid, htm_depth);
    }

    if (load_inst->isHtmStop()) {
        htmStops++;
        DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops++ (%d)\n", htmStarts,
                htmStops);

        if (htmStops == 1 && htmStarts == 0) {
            DPRINTF(HtmCpu, "htmStops==1 && htmStarts==0. "
                            "This generally shouldn't happen "
                            "(unless due to misspeculation)\n");
        }
    }
}

void
LSQUnit::insertStore(const DynInstPtr &store_inst)
{
    // Make sure it is not full before inserting an instruction.
    assert(!storeQueue.full());
    assert(storeQueue.size() < storeQueue.capacity());
    ++stats.addedLoadsAndStores;

    DPRINTF(LSQUnit, "Inserting store PC %s, idx:%i [sn:%lli]\n",
            store_inst->pcState(), storeQueue.tail(), store_inst->seqNum);
    storeQueue.advance_tail();

    store_inst->sqIdx = storeQueue.tail();
    store_inst->sqIt = storeQueue.getIterator(store_inst->sqIdx);

    store_inst->lqIdx = loadQueue.tail() + 1;
    assert(store_inst->lqIdx > 0);
    store_inst->lqIt = loadQueue.end();

    storeQueue.back().set(store_inst);

    stats.sqAvgOccupancy = queueOccupancy(storeQueue);
}

DynInstPtr
LSQUnit::getMemDepViolator()
{
    DynInstPtr temp = memDepViolator;

    memDepViolator = NULL;

    return temp;
}

void
LSQUnit::setMemOrderViolatorIfOlder(const DynInstPtr &inst)
{
    if (!memOrderViolator || inst->seqNum < memOrderViolator->seqNum) {
        memOrderViolator = inst;
    }
}

void
LSQUnit::checkCompletedLoadSnoopHazards(const DynInstPtr &completed_load)
{
    if (!needsTSO || loadQueue.empty() || !completed_load ||
        completed_load->isSquashed()) {
        return;
    }

    auto younger_it = completed_load->lqIt;
    ++younger_it;

    for (; younger_it != loadQueue.end(); ++younger_it) {
        if (!younger_it->valid()) {
            continue;
        }

        const DynInstPtr &younger = younger_it->instruction();
        if (!younger || younger->isSquashed() || !younger->isExecuted() ||
            !younger->hitExternalSnoop()) {
            continue;
        }

        DPRINTF(LSQUnit,
                "TSO load/load violation: older load completed [sn:%lli], "
                "younger executed load observed an eviction/snoop "
                "[sn:%lli]\n",
                completed_load->seqNum, younger->seqNum);

        if (younger->fault == NoFault) {
            younger->fault = std::make_shared<ReExec>();
            if (younger_it->hasRequest()) {
                younger_it->request()->setStateToFault();
            }
            setMemOrderViolatorIfOlder(younger);
            ++stats.barrierReschedulesLSQ;
            ++stats.memOrderViolation;
            ++stats.tsoLoadCompletionReschedules;
        }

        // Replaying the oldest hazardous younger load also removes every
        // younger instruction.
        break;
    }
}

DynInstPtr
LSQUnit::getMemOrderViolator()
{
    DynInstPtr temp = memOrderViolator;
    memOrderViolator = NULL;
    return temp;
}

DynInstPtr
LSQUnit::peekMemOrderViolator() const
{
    return memOrderViolator;
}

unsigned
LSQUnit::numFreeLoadEntries()
{
    DPRINTF(LSQUnit, "LQ size: %d, #loads occupied: %d\n",
            loadQueue.capacity(), loadQueue.size());
    return loadQueue.capacity() - loadQueue.size();
}

unsigned
LSQUnit::numFreeStoreEntries()
{
    DPRINTF(LSQUnit, "SQ size: %d, #stores occupied: %d\n",
            storeQueue.capacity(), storeQueue.size());
    return storeQueue.capacity() - storeQueue.size();
}

void
LSQUnit::checkL1Eviction(PacketPtr pkt)
{
    assert(pkt->req && pkt->req->isL1DEvictionNotify());

    if (!needsTSO || loadQueue.empty()) {
        return;
    }

    const Addr evict_addr = pkt->getAddr() & cacheBlockMask;
    for (auto &entry : loadQueue) {
        if (!entry.valid() || !entry.hasRequest()) {
            continue;
        }

        const DynInstPtr &ld_inst = entry.instruction();
        assert(ld_inst);
        LSQRequest *request = entry.request();

        if (ld_inst->isSquashed() || !ld_inst->isExecuted() ||
            !ld_inst->effAddrValid() || ld_inst->strictlyOrdered() ||
            !request->isCacheBlockHit(evict_addr, cacheBlockMask)) {
            continue;
        }

        if (!ld_inst->hitExternalSnoop()) {
            DPRINTF(LSQUnit,
                    "Recording TSO L1 eviction hazard for addr %#x "
                    "[sn:%lli]\n",
                    evict_addr, ld_inst->seqNum);
            ld_inst->hitExternalSnoop(true);
            ++stats.tsoL1EvictionHazards;
        }
    }
}

void
LSQUnit::checkSnoop(PacketPtr pkt)
{
    // Should only ever get invalidations in here
    assert(pkt->isInvalidate());

    DPRINTF(LSQUnit, "Got snoop for address %#x\n", pkt->getAddr());

    for (int x = 0; x < cpu->numContexts(); x++) {
        gem5::ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        tc->getIsaPtr()->handleLockedSnoop(pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    if (loadQueue.empty()) {
        return;
    }

    auto iter = loadQueue.begin();

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;

    DynInstPtr ld_inst = iter->instruction();
    assert(ld_inst);
    LSQRequest *request = iter->request();

    // Check that this snoop didn't just invalidate our lock flag
    if (ld_inst->effAddrValid() && request &&
        request->isCacheBlockHit(invalidate_addr, cacheBlockMask) &&
        ld_inst->memReqFlags & Request::LLSC) {
        ld_inst->tcBase()->getIsaPtr()->handleLockedSnoopHit(ld_inst.get());
    }

    bool force_squash = false;

    while (++iter != loadQueue.end()) {
        ld_inst = iter->instruction();
        assert(ld_inst);
        request = iter->request();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered() ||
            !request) {
            continue;
        }

        DPRINTF(LSQUnit, "-- inst [sn:%lli] to pktAddr:%#x\n", ld_inst->seqNum,
                invalidate_addr);

        if (force_squash ||
            request->isCacheBlockHit(invalidate_addr, cacheBlockMask)) {
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;
            }
            if (false) {
                // if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
                request->setStateToFault();
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC) {
                    ld_inst->tcBase()->getIsaPtr()->handleLockedSnoopHit(
                        ld_inst.get());
                }

                // If a older load checks this and it's true
                // then we might have missed the snoop
                // in which case we need to invalidate to be sure
                ld_inst->hitExternalSnoop(true);
            }
        }
    }
    return;
}

unsigned
LSQUnit::markLoadsHitExternalSnoopAfter(const InstSeqNum &barrier_sn)
{
    if (loadQueue.empty()) {
        return 0;
    }

    unsigned marked = 0;
    for (auto &entry : loadQueue) {
        if (!entry.valid()) {
            continue;
        }

        const DynInstPtr &ld_inst = entry.instruction();
        assert(ld_inst);

        if (ld_inst->seqNum <= barrier_sn || ld_inst->isSquashed()) {
            continue;
        }

        // Unlike in checkViolations, we can use isExecuted() here because
        // the store actually completed updating the cache by this time.
        if (!ld_inst->isExecuted()) {
            continue;
        }

        if (!ld_inst->hitExternalSnoop()) {
            continue;
        }

        if (ld_inst->fault == NoFault) {
            DPRINTF(LSQUnit,
                    "Marking load for re-exec due to external snoop "
                    "[sn:%lli] barrier [sn:%lli]\n",
                    ld_inst->seqNum, barrier_sn);
            ld_inst->fault = std::make_shared<ReExec>();
            if (entry.hasRequest()) {
                entry.request()->setStateToFault();
            }
            ++stats.barrierReschedulesLSQ;
            ++marked;
        }
    }

    return marked;
}

unsigned
LSQUnit::markLoadsHitExternalSnoop(uint64_t version)
{
    if (loadQueue.empty()) {
        return 0;
    }

    unsigned marked = 0;
    for (auto &entry : loadQueue) {
        if (!entry.valid()) {
            continue;
        }

        const DynInstPtr &ld_inst = entry.instruction();
        assert(ld_inst);

        if (ld_inst->isSquashed()) {
            continue;
        }

        if (ld_inst->getMemOrderVersion() <= version) {
            continue;
        }

        // Unlike in checkViolations, we can use isExecuted() here because
        // the store actually completed updating the cache by this time.
        if (!ld_inst->isExecuted()) {
            continue;
        }

        if (!ld_inst->hitExternalSnoop()) {
            continue;
        }

        if (ld_inst->fault == NoFault) {
            DPRINTF(LSQUnit,
                    "Marking load for re-exec due to external snoop "
                    "[sn:%lli] ver:%llu > %llu\n",
                    ld_inst->seqNum, ld_inst->getMemOrderVersion(), version);
            ld_inst->fault = std::make_shared<ReExec>();
            if (entry.hasRequest()) {
                entry.request()->setStateToFault();
            }
            ++stats.barrierReschedulesLSQ;
            ++marked;
        }
    }

    return marked;
}

unsigned
LSQUnit::markAcquireLoadsHitExternalSnoopAfter(const InstSeqNum &barrier_sn)
{
    // Stub: speculativeBarrierIssueEnabled() is always false, so this is
    // never called.
    return 0;
}

Fault
LSQUnit::checkViolations(typename LoadQueue::iterator &loadIt,
                         const DynInstPtr &inst)
{
    Addr inst_eff_addr1 = inst->effAddr >> depCheckShift;
    Addr inst_eff_addr2 = (inst->effAddr + inst->effSize - 1) >> depCheckShift;

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    while (loadIt != loadQueue.end()) {
        DynInstPtr ld_inst = loadIt->instruction();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            ++loadIt;
            continue;
        }

        auto inst_mem_version = inst->getMemOrderVersion();
        auto ld_mem_version = ld_inst->getMemOrderVersion();
        // if a younger load bypassed an older store with older version,
        // mark this load as a potential violation on snoop
        bool possible_ordering_hazard = cpu->versioningEnabled() &&
                                        (inst_mem_version < ld_mem_version) &&
                                        !ld_inst->stlfForwarded();

        Addr ld_eff_addr1 = ld_inst->effAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> depCheckShift;

        bool addr_overlap = (inst_eff_addr2 >= ld_eff_addr1) &&
                            (inst_eff_addr1 <= ld_eff_addr2);

        if (addr_overlap) {
            if (inst->isLoad()) {
                // If this load is to the same block as an external snoop
                // invalidate that we've observed then the load needs to be
                // squashed as it could have newer data
                if (ld_inst->hitExternalSnoop()) {
                    if (!memDepViolator ||
                        ld_inst->seqNum < memDepViolator->seqNum) {
                        DPRINTF(LSQUnit,
                                "Detected fault with inst [sn:%lli] "
                                "and [sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        memDepViolator = ld_inst;

                        ++stats.memOrderViolation;
                        if (possible_ordering_hazard && !addr_overlap) {
                            ++stats.possibleConsistencyViolation;
                        }

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // If this load and a younger load have the same version and
                // the younger load did not see an invalidation snoop yet, we
                // don't need to mark the younger load as a possible violation
                // in a weak memory model
                // if (!needsTSO && cpu->versioningEnabled() &&
                //    inst_mem_version >= ld_mem_version) {
                //    ++loadIt;
                //    continue;
                //}
                // Otherwise, mark the load has a possible load violation and
                // if we see a snoop before it's commited, we need to squash
                // Do not mark a possible load violation here. A load which
                // has not actually overlapped this store must not trigger a
                // later read-after-read squash merely because it observed a
                // snoop.
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator &&
                    ld_inst->seqNum > memDepViolator->seqNum) {
                    break;
                }

                DPRINTF(LSQUnit,
                        "Detected fault with inst [sn:%lli] and "
                        "[sn:%lli] at address %#x\n",
                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                memDepViolator = ld_inst;

                ++stats.memOrderViolation;

                return std::make_shared<GenericISA::M5PanicFault>(
                    "Detected fault with "
                    "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
            }
        }
        ++loadIt;
    }

    return NoFault;
}

Fault
LSQUnit::executeLoad(const DynInstPtr &inst)
{
    // Execute a specific load.
    Fault load_fault = NoFault;

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli]\n", inst->pcState(),
            inst->seqNum);

    assert(!inst->isSquashed());

    if (inst->isExecuted()) {
        DPRINTF(LSQUnit, "Load [sn:%lli] already executed\n", inst->seqNum);
        return NoFault;
    }

    if (po3MemPipeline && !isInPO3MemPipeline(inst)) {
        enqueuePO3MemInst(inst);
        return NoFault;
    }

    load_fault = inst->initiateAcc();

    if (load_fault == NoFault && !inst->readMemAccPredicate()) {
        assert(inst->readPredicate());
        inst->setExecuted();
        inst->completeAcc(nullptr);
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        if (inst->lqIdx >= 0) {
            LSQRequest *request = loadQueue[inst->lqIdx].request();
            if (request) {
                // No packet or translation response will arrive for this
                // short-circuited access.  Drop the LSQ's request pointer here
                // after the instruction has completed its execute-stage work,
                // so the request no longer keeps the DynInst alive.
                loadQueue[inst->lqIdx].setRequest(nullptr);
                request->discard();
                inst->clearRequest();
            }
        }
        return NoFault;
    }

    if (inst->isTranslationDelayed() && load_fault == NoFault) {
        return load_fault;
    }

    if (load_fault != NoFault && inst->translationCompleted() &&
        inst->savedRequest->isPartialFault() &&
        !inst->savedRequest->isComplete()) {
        assert(inst->savedRequest->isSplit());
        // If we have a partial fault where the mem access is not complete yet
        // then the cache must have been blocked. This load will be re-executed
        // when the cache gets unblocked. We will handle the fault when the
        // mem access is complete.
        return NoFault;
    }

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    if (load_fault != NoFault || !inst->readPredicate()) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (!inst->readPredicate()) {
            inst->forwardOldRegs();
        }
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n", inst->seqNum,
                (load_fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
    } else {
        if (inst->effAddrValid()) {
            auto it = inst->lqIt;
            ++it;

            // Check if any younger loads to the same address executed
            // before this load
            if (checkLoads) {
                return checkViolations(it, inst);
            }
        }
    }

    return load_fault;
}

Fault
LSQUnit::executeStore(const DynInstPtr &store_inst)
{
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);

    ssize_t store_idx = store_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing store PC %s [sn:%lli]\n",
            store_inst->pcState(), store_inst->seqNum);

    assert(!store_inst->isSquashed());

    if (po3MemPipeline && !isInPO3MemPipeline(store_inst)) {
        enqueuePO3MemInst(store_inst);
        return NoFault;
    }

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = store_inst->lqIt;

    Fault store_fault = store_inst->initiateAcc();

    if (store_inst->isTranslationDelayed() && store_fault == NoFault) {
        return store_fault;
    }

    if (!store_inst->readPredicate()) {
        DPRINTF(LSQUnit, "Store [sn:%lli] not executed from predication\n",
                store_inst->seqNum);
        store_inst->forwardOldRegs();
        return store_fault;
    }

    if (storeQueue[store_idx].size() == 0) {
        DPRINTF(LSQUnit, "Fault on Store PC %s, [sn:%lli], Size = 0\n",
                store_inst->pcState(), store_inst->seqNum);

        if (store_inst->isAtomic()) {
            // If the instruction faulted, then we need to send it along
            // to commit without the instruction completing.
            if (!(store_inst->hasRequest() && store_inst->strictlyOrdered()) ||
                store_inst->isAtCommit()) {
                store_inst->setExecuted();
            }
            iewStage->instToCommit(store_inst);
            iewStage->activityThisCycle();
        }

        return store_fault;
    }

    assert(store_fault == NoFault);

    if (store_inst->isStoreConditional() || store_inst->isAtomic()) {
        // Store conditionals and Atomics need to set themselves as able to
        // writeback if we haven't had a fault by here.
        storeQueue[store_idx].canWB() = true;

        ++storesToWB;
    }

    return checkViolations(loadIt, store_inst);
}

void
LSQUnit::commitLoad()
{
    assert(loadQueue.front().valid());

    DynInstPtr inst = loadQueue.front().instruction();

    DPRINTF(LSQUnit, "Committing head load instruction, PC %s\n",
            inst->pcState());

    // Update histogram with memory latency from load
    // Only take latency from load demand that where issued and did not fault
    if (!inst->isInstPrefetch() && !inst->isDataPrefetch() &&
        inst->firstIssue != -1 && inst->lastWakeDependents != -1) {
        stats.loadToUse.sample(
            cpu->ticksToCycles(inst->lastWakeDependents - inst->firstIssue));
    }

    loadQueue.front().clear();
    loadQueue.pop_front();

    stats.lqAvgOccupancy = queueOccupancy(loadQueue);
}

void
LSQUnit::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loadQueue.size() == 0 || loadQueue.front().valid());

    while (loadQueue.size() != 0 &&
           loadQueue.front().instruction()->seqNum <= youngest_inst) {
        commitLoad();
    }
}

void
LSQUnit::commitStores(InstSeqNum &youngest_inst)
{
    assert(storeQueue.size() == 0 || storeQueue.front().valid());

    /* Forward iterate the store queue (age order). */
    for (auto &x : storeQueue) {
        assert(x.valid());
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!x.canWB()) {
            if (x.instruction()->seqNum > youngest_inst) {
                break;
            }
            DPRINTF(LSQUnit,
                    "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    x.instruction()->pcState(), x.instruction()->seqNum);

            x.canWB() = true;

            ++storesToWB;
        }
    }
}

void
LSQUnit::writebackBlockedStore()
{
    assert(isStoreBlocked);
    storeWBIt->request()->sendPacketToCache();
    if (storeWBIt->request()->isSent()) {
        storePostSend();
    }
}

void
LSQUnit::recordMBStoreBlock(bool mb_full)
{
    if (storeQueue.empty() || !storeWBIt.dereferenceable() ||
        storeWBIt.idx() != storeQueue.head()) {
        return;
    }

    ++stats.mbSqHeadBlockedCycles;
    if (!mb_full) {
        return;
    }

    ++stats.mbFullEvents;
    if (cpu->frontendStalled(lsqID)) {
        ++stats.mbFullFrontendStalledEvents;
    }
}

void
LSQUnit::writebackStores()
{
    Cycles now = cpu->curCycle();

    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Writing back blocked store\n");
        writebackBlockedStore();
    }

    if (mergeBufferEnabled) {
        if (needsTSO && storeInFlight && mergeBuffer.hasDrainableEntry()) {
            ++stats.mbTsoStoreInFlightDrainStallCycles;
        }

        // Keep issuing independent drains while progress is possible. A
        // partial drain may only start its RMW read here; drainOne() skips
        // that entry until its X+2 write has the merge buffer's cache port,
        // allowing another entry to enter the RMW pipeline.
        while ((!needsTSO || !storeInFlight) && !lsq->cacheBlocked() &&
               mergeBuffer.drainOne(this)) {
            if (needsTSO) {
                break;
            }
        }
    }

    // Track store queue deallocations per cycle for head removals.
    if (lastStoreDeallocCycle != cpu->curCycle()) {
        storeDeallocsThisCycle = 0;
        mbStorePipe0Used = false;
        mbStorePipe1Used = false;
        lastStoreDeallocCycle = cpu->curCycle();
    }

    bool forcedMBRetire = false;

    while (storesToWB > 0 && storeWBIt.dereferenceable() &&
           storeWBIt->valid() && storeWBIt->canWB()) {

        DPRINTF(LSQUnit, "Trying to drain store at idx:%i PC:%s [sn:%lu]\n",
                storeWBIt.idx(), storeWBIt->instruction()->pcState(),
                storeWBIt->instruction()->seqNum);

        if (storeDeallocsThisCycle >= storeDeallocateWidth) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, store "
                             " dealloc bandwidth reached!\n");
            break;
        }

        // Store didn't write any data so no need to write it back to
        // memory.
        if (storeWBIt->size() == 0) {
            completeStore(storeWBIt++);
            ++storeDeallocsThisCycle;
            continue;
        }

        if (storeWBIt->instruction()->isDataPrefetch()) {
            storeWBIt++;
            continue;
        }

        assert(storeWBIt->hasRequest());

        DynInstPtr inst = storeWBIt->instruction();
        LSQRequest *request = storeWBIt->request();
        bool is_atomic_req = request->mainReq()->isAtomic();
        const unsigned store_pipe = storePipeIndex(inst->memPipe());
        constexpr CachePort store_port = CachePort::MergeBufferWrite;

        bool can_use_mb = mergeBufferEnabled &&
                          !request->mainReq()->isLocalAccess() &&
                          !request->mainReq()->isLLSC() && !is_atomic_req;
        bool can_use_mb_atomic =
            mergeBufferEnabled && !request->mainReq()->isLocalAccess() &&
            !request->mainReq()->isLLSC() && is_atomic_req;

        if (!can_use_mb && !can_use_mb_atomic && isStoreBlocked) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, cache"
                             " is blocked!\n");
            break;
        }

        // Process store conditionals or store release after all previous
        // stores are completed
        if ((request->mainReq()->isLLSC() ||
             request->mainReq()->isRelease()) &&
            (storeWBIt.idx() != storeQueue.head())) {
            DPRINTF(LSQUnit,
                    "Store idx:%i PC:%s to Addr:%#x "
                    "[sn:%lli] is %s%s and not head of the queue\n",
                    storeWBIt.idx(), inst->pcState(),
                    request->mainReq()->getPaddr(), inst->seqNum,
                    request->mainReq()->isLLSC() ? "SC" : "",
                    request->mainReq()->isRelease() ? "/Release" : "");
            break;
        }

        assert(!storeWBIt->committed());

        if (can_use_mb_atomic) {
            const bool pipe_used =
                store_pipe == 0 ? mbStorePipe0Used : mbStorePipe1Used;
            if (pipe_used) {
                DPRINTF(LSQUnit,
                        "Unable to write atomic [sn:%lli] into merge buffer: "
                        "load/store pipe %u input is busy\n",
                        inst->seqNum, store_pipe);
                break;
            }

            // Ordering tags are metadata only in PO3 for now.
            constexpr uint64_t store_version = 0;
            MergeBuffer::MergeBufferEntry *mb_entry =
                mergeBuffer.addAtomic(now, request, storeWBIt, store_version);

            DPRINTF(LSQUnit,
                    "Merge for atomic store idx:%i PC:%s "
                    "to Addr:%#x [sn:%lli] %s\n",
                    storeWBIt.idx(), inst->pcState(),
                    request->mainReq()->getPaddr(), inst->seqNum,
                    mb_entry ? "accepted" : "blocked");

            if (mb_entry) {
                if (store_pipe == 0) {
                    mbStorePipe0Used = true;
                    ++stats.mbLoadStorePipe0Writes;
                } else {
                    mbStorePipe1Used = true;
                    ++stats.mbLoadStorePipe1Writes;
                }
                if (mbStorePipe0Used && mbStorePipe1Used) {
                    ++stats.mbDualPipeWriteCycles;
                }

                if (request->mainReq()->isRelease()) {
                    mb_entry->isRelease = true;
                    // Without version-aware wait bits, keep the release
                    // atomic behind every older merge-buffer entry.
                    mergeBuffer.forceRetireAll();
                }

                // The SQ entry and LSQRequest must remain live until the
                // atomic response writes its result back.
                storeWBIt->canWB() = false;
            } else {
                const bool mb_full = mergeBuffer.isFull();
                recordMBStoreBlock(mb_full);
                if (mb_full) {
                    ++stats.mbFullStoreDeallocStalls;
                }
                if (!forcedMBRetire &&
                    (inst->isWriteBarrier() || inst->isSerializeBefore() ||
                     inst->isSerializeAfter() ||
                     request->mainReq()->isRelease())) {
                    mergeBuffer.forceRetireAll();
                    forcedMBRetire = true;
                }
                break;
            }
        } else if (can_use_mb) {
            const bool pipe_used =
                store_pipe == 0 ? mbStorePipe0Used : mbStorePipe1Used;
            if (pipe_used) {
                DPRINTF(LSQUnit,
                        "Unable to write store [sn:%lli] into merge buffer: "
                        "load/store pipe %u input is busy\n",
                        inst->seqNum, store_pipe);
                break;
            }

            bool merged_ok = true;
            bool mb_full = false;
            MergeBuffer::MergeBufferEntry *mb_entry = nullptr;
            MergeBuffer::MergeBufferEntry *mb_entry2 = nullptr;
            // Ordering tags are metadata only in PO3 for now. Keep the
            // merge buffer's versioning behavior disabled.
            constexpr uint64_t store_version = 0;

            bool is_release_req = request->mainReq()->isRelease();
            bool is_release_store =
                optimizeStoreRelease && can_use_mb && is_release_req;

            std::vector<bool> release_wait_bits;
            if (is_release_store) {
                release_wait_bits = mergeBuffer.validVector();
            }
            auto clear_self_wait_bit =
                [this](MergeBuffer::MergeBufferEntry *entry) {
                    if (!entry || entry->waitBits.empty()) {
                        return;
                    }
                    const size_t idx = mergeBuffer.indexOf(entry);
                    if (idx != std::numeric_limits<size_t>::max() &&
                        idx < entry->waitBits.size()) {
                        entry->waitBits[idx] = false;
                    }
                };

            if (request->isSplit()) {
                // For split stores, make sure both fragments can be merged
                // before releasing the SQ entry.
                auto req0 = request->req(0);
                auto req1 = request->req(1);
                const size_t size0 = req0->getSize();
                const size_t size1 = req1->getSize();

                bool can_merge_both = mergeBuffer.canAcceptSplitStore(
                    request, store_version, mb_full);

                if (can_merge_both) {
                    mb_entry = mergeBuffer.addStore(
                        now, req0->getPaddr(),
                        reinterpret_cast<uint8_t *>(storeWBIt->data()), size0,
                        storeWBIt, storeWBIt->isAllZeros());

                    mb_entry2 = mergeBuffer.addStore(
                        now, req1->getPaddr(),
                        reinterpret_cast<uint8_t *>(storeWBIt->data()) + size0,
                        size1, storeWBIt, storeWBIt->isAllZeros());

                    if (!mb_entry || !mb_entry2) {
                        panic("Only one part of a split store merged!");
                        merged_ok = false;
                    }
                } else {
                    merged_ok = false;
                }
            } else {
                mb_entry = mergeBuffer.addStore(
                    now, request->mainReq()->getPaddr(),
                    (uint8_t *)storeWBIt->data(), request->_size, storeWBIt,
                    storeWBIt->isAllZeros());
                mb_full = !mb_entry && mergeBuffer.isFull();
            }

            DPRINTF(LSQUnit,
                    "Merge for store idx:%i PC:%s "
                    "to Addr:%#x, data:%#x [sn:%lli] %s\n",
                    storeWBIt.idx(), inst->pcState(),
                    request->mainReq()->getPaddr(), (int)*(storeWBIt->data()),
                    inst->seqNum,
                    (mb_entry && merged_ok) ? "accepted" : "blocked");

            if (mb_entry && merged_ok) {
                if (store_pipe == 0) {
                    mbStorePipe0Used = true;
                    ++stats.mbLoadStorePipe0Writes;
                } else {
                    mbStorePipe1Used = true;
                    ++stats.mbLoadStorePipe1Writes;
                }
                if (mbStorePipe0Used && mbStorePipe1Used) {
                    ++stats.mbDualPipeWriteCycles;
                }
                if (is_release_req) {
                    mb_entry->isRelease = true;
                    if (is_release_store) {
                        mb_entry->waitBits = release_wait_bits;
                        clear_self_wait_bit(mb_entry);
                    }
                    if (mb_entry2) {
                        mb_entry2->isRelease = true;
                        if (is_release_store) {
                            mb_entry2->waitBits = release_wait_bits;
                            clear_self_wait_bit(mb_entry2);
                        }
                    }
                    DPRINTF(
                        LSQUnit,
                        "Store-release idx:%i tracked via MB entry ver:%llu\n",
                        storeWBIt.idx(), store_version);
                }
                // Should never merge the same store twice.
                assert(!storeWBIt->completed());
                // Complete and remove this store from the SQ;
                // merge buffer owns the data from here on.
                completeStore(storeWBIt);
                ++storeDeallocsThisCycle;
                if (!storeQueue.empty()) {
                    storeWBIt++;
                } else {
                    storeWBIt = storeQueue.end();
                }
            } else {
                recordMBStoreBlock(mb_full);

                // If a barrier/release store is stalled, force retire MB
                // entries once to unblock serialization.
                if (mb_full) {
                    ++stats.mbFullStoreDeallocStalls;
                }
                if (!forcedMBRetire &&
                    (inst->isWriteBarrier() || inst->isSerializeBefore() ||
                     inst->isSerializeAfter() ||
                     request->mainReq()->isRelease())) {
                    mergeBuffer.forceRetireAll();
                    forcedMBRetire = true;
                }
                // Unable to merge, stop trying
                break;
            }
        } else if (((!needsTSO) || (!storeInFlight)) &&
                   lsq->cachePortAvailable(store_port)) {

            storeWBIt->committed() = true;

            assert(!inst->memData);
            inst->memData = new uint8_t[request->_size];

            if (storeWBIt->isAllZeros()) {
                memset(inst->memData, 0, request->_size);
            } else {
                memcpy(inst->memData, storeWBIt->data(), request->_size);
            }

            request->buildPackets();

            DPRINTF(LSQUnit,
                    "D-Cache: Writing back store idx:%i PC:%s "
                    "to Addr:%#x, data:%#x [sn:%lli]\n",
                    storeWBIt.idx(), inst->pcState(),
                    request->mainReq()->getPaddr(), (int)*(inst->memData),
                    inst->seqNum);

            // @todo: Remove this SC hack once the memory system handles it.
            if (inst->isStoreConditional()) {
                // Disable recording the result temporarily.  Writing to
                // misc regs normally updates the result, but this is not
                // the desired behavior when handling store conditionals.
                inst->recordResult(false);
                bool success = inst->tcBase()->getIsaPtr()->handleLockedWrite(
                    inst.get(), request->mainReq(), cacheBlockMask);
                inst->recordResult(true);
                request->packetSent();

                if (!success) {
                    request->complete();
                    // Instantly complete this store.
                    DPRINTF(LSQUnit,
                            "Store conditional [sn:%lli] failed.  "
                            "Instantly completing it.\n",
                            inst->seqNum);

                    PacketPtr new_pkt = new Packet(*request->packet());
                    WritebackEvent *wb =
                        new WritebackEvent(inst, new_pkt, this);
                    cpu->schedule(wb, curTick() + 1);
                    completeStore(storeWBIt);
                    ++storeDeallocsThisCycle;
                    if (!storeQueue.empty()) {
                        storeWBIt++;
                    } else {
                        storeWBIt = storeQueue.end();
                    }
                    continue;
                }
            }

            if (request->mainReq()->isLocalAccess()) {
                assert(!inst->isStoreConditional());
                assert(!inst->inHtmTransactionalState());
                gem5::ThreadContext *thread = cpu->tcBase(lsqID);
                PacketPtr main_pkt =
                    new Packet(request->mainReq(), MemCmd::WriteReq);
                main_pkt->dataStatic(inst->memData);
                request->mainReq()->localAccessor(thread, main_pkt);
                delete main_pkt;
                completeStore(storeWBIt);
                storeWBIt++;
                ++storeDeallocsThisCycle;
                continue;
            }
            /* Send to cache */
            request->sendPacketToCache();

            /* If successful, do the post send */
            if (request->isSent()) {
                storePostSend();
                ++storeDeallocsThisCycle;
            } else {
                DPRINTF(LSQUnit,
                        "D-Cache became blocked when writing [sn:%lli], "
                        "will retry later\n",
                        inst->seqNum);
            }
        } else {
            // This store must use the write port associated with the
            // load/store pipe on which it issued.
            DPRINTF(LSQUnit,
                    "Unable to write back store [sn:%lli]: its load/store "
                    "pipe write port is busy\n",
                    inst->seqNum);
            break;
        }
        assert(storesToWB >= 0);
    }
}

void
LSQUnit::updateMergeBufferRetire()
{
    if (!mergeBufferEnabled) {
        return;
    }
    if (mergeBuffer.isEmpty()) {
        return;
    }

    mergeBuffer.updateRetiredEntries(cpu->curCycle());
    cpu->activityThisCycle();
}

void
LSQUnit::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit,
            "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n",
            squashed_num, loadQueue.size(), storeQueue.size());

    while (loadQueue.size() != 0 &&
           loadQueue.back().instruction()->seqNum > squashed_num) {
        DPRINTF(LSQUnit,
                "Load Instruction PC %s squashed, "
                "[sn:%lli]\n",
                loadQueue.back().instruction()->pcState(),
                loadQueue.back().instruction()->seqNum);

        if (isStalled() && loadQueue.tail() == stallingLoadIdx) {
            stalled = false;
            stallingStoreIsn = 0;
            stallingLoadIdx = 0;
        }

        // hardware transactional memory
        // Squashing instructions can alter the transaction nesting depth
        // and must be corrected before fetching resumes.
        if (loadQueue.back().instruction()->isHtmStart()) {
            htmStarts = (--htmStarts < 0) ? 0 : htmStarts;
            DPRINTF(HtmCpu, ">> htmStarts-- (%d) : htmStops (%d)\n", htmStarts,
                    htmStops);
        }
        if (loadQueue.back().instruction()->isHtmStop()) {
            htmStops = (--htmStops < 0) ? 0 : htmStops;
            DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops-- (%d)\n", htmStarts,
                    htmStops);
        }
        // Clear the smart pointer to make sure it is decremented.
        loadQueue.back().instruction()->setSquashed();
        loadQueue.back().clear();

        loadQueue.pop_back();
        ++stats.squashedLoads;
    }

    stats.lqAvgOccupancy = queueOccupancy(loadQueue);

    // hardware transactional memory
    // scan load queue (from oldest to youngest) for most recent valid htmUid
    auto scan_it = loadQueue.begin();
    uint64_t in_flight_uid = 0;
    while (scan_it != loadQueue.end()) {
        if (scan_it->instruction()->isHtmStart() &&
            !scan_it->instruction()->isSquashed()) {
            in_flight_uid = scan_it->instruction()->getHtmTransactionUid();
            DPRINTF(HtmCpu, "loadQueue[%d]: found valid HtmStart htmUid=%u\n",
                    scan_it._idx, in_flight_uid);
        }
        scan_it++;
    }
    // If there's a HtmStart in the pipeline then use its htmUid,
    // otherwise use the most recently committed uid
    const auto &htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    if (htm_cpt) {
        const uint64_t old_local_htm_uid = htm_cpt->getHtmUid();
        uint64_t new_local_htm_uid;
        if (in_flight_uid > 0) {
            new_local_htm_uid = in_flight_uid;
        } else {
            new_local_htm_uid = lastRetiredHtmUid;
        }

        if (old_local_htm_uid != new_local_htm_uid) {
            DPRINTF(HtmCpu, "flush: lastRetiredHtmUid=%u\n",
                    lastRetiredHtmUid);
            DPRINTF(HtmCpu, "flush: resetting localHtmUid=%u\n",
                    new_local_htm_uid);

            htm_cpt->setHtmUid(new_local_htm_uid);
        }
    }

    if (memDepViolator && squashed_num < memDepViolator->seqNum) {
        memDepViolator = NULL;
    }
    if (memOrderViolator && squashed_num < memOrderViolator->seqNum) {
        memOrderViolator = NULL;
    }

    while (storeQueue.size() != 0 &&
           storeQueue.back().instruction()->seqNum > squashed_num) {
        // Instructions marked as can WB are already committed.
        if (storeQueue.back().canWB()) {
            break;
        }

        DPRINTF(LSQUnit,
                "Store Instruction PC %s squashed, "
                "idx:%i [sn:%lli]\n",
                storeQueue.back().instruction()->pcState(), storeQueue.tail(),
                storeQueue.back().instruction()->seqNum);

        // I don't think this can happen.  It should have been cleared
        // by the stalling load.
        if (isStalled() &&
            storeQueue.back().instruction()->seqNum == stallingStoreIsn) {
            panic("Is stalled should have been cleared by stalling load!\n");
            stalled = false;
            stallingStoreIsn = 0;
        }

        // Clear the smart pointer to make sure it is decremented.
        storeQueue.back().instruction()->setSquashed();

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        storeQueue.back().clear();

        storeQueue.pop_back();
        ++stats.squashedStores;
    }
    stats.sqAvgOccupancy = queueOccupancy(storeQueue);
}

uint64_t
LSQUnit::getLatestHtmUid() const
{
    const auto &htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    return htm_cpt->getHtmUid();
}

void
LSQUnit::storePostSend()
{
    if (isStalled() && storeWBIt->instruction()->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit,
                "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    if (!storeWBIt->instruction()->isStoreConditional()) {
        // The store is basically completed at this time. This
        // only works so long as the checker doesn't try to
        // verify the value in memory for stores.
        storeWBIt->instruction()->setCompleted();

        if (cpu->checker) {
            cpu->checker->verify(storeWBIt->instruction());
        }
    }

    if (needsTSO) {
        storeInFlight = true;
    }

    storeWBIt++;
}

void
LSQUnit::writeback(const DynInstPtr &inst, PacketPtr pkt)
{
    iewStage->wakeCPU();
    bool completed_load = false;

    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert(!inst->isStore() || inst->isStoreConditional());
        ++stats.ignoredResponses;
        return;
    }

    if (!inst->isExecuted()) {
        inst->setExecuted();

        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
            completed_load = inst->isLoad();
        } else {
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec or - in case of a SplitRequest - a partial
            // translation fault

            // Unless it's a hardware transactional memory fault
            auto htm_fault =
                std::dynamic_pointer_cast<GenericHtmFailureFault>(inst->fault);

            if (!htm_fault) {
                assert(dynamic_cast<ReExec *>(inst->fault.get()) != nullptr ||
                       inst->savedRequest->isPartialFault());

            } else if (!pkt->htmTransactionFailedInCache()) {
                // Situation in which the instruction has a hardware
                // transactional memory fault but not the packet itself. This
                // can occur with ldp_uop microops since access is spread over
                // multiple packets.
                DPRINTF(HtmCpu,
                        "%s writeback with HTM failure fault, "
                        "however, completing packet is not aware of "
                        "transaction failure. cause=%s htmUid=%u\n",
                        inst->staticInst->getName(),
                        htmFailureToStr(htm_fault->getHtmFailureFaultCause()),
                        htm_fault->getHtmUid());
            }

            DPRINTF(LSQUnit,
                    "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n",
                    inst->seqNum);
        }
    }

    if (completed_load && needsTSO) {
        checkCompletedLoadSnoopHazards(inst);
    }

    // Need to insert instruction into queue to commit
    iewStage->instToCommit(inst);

    iewStage->activityThisCycle();

    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

void
LSQUnit::completeStore(typename StoreQueue::iterator store_idx)
{
    assert(store_idx->valid());
    assert(!store_idx->completed());

    // Only a store sent directly from the SQ owns storeInFlight. Stores
    // transferred into the merge buffer complete their SQ entry without
    // completing an already-issued MB drain. Capture this before a head
    // completion clears the SQ entry.
    const bool completes_direct_store = store_idx->committed();

    store_idx->completed() = true;
    --storesToWB;
    // A bit conservative because a store completion may not free up entries,
    // but hopefully avoids two store completions in one cycle from making
    // the CPU tick twice.
    cpu->wakeCPU();
    cpu->activityThisCycle();

    /* We 'need' a copy here because we may clear the entry from the
     * store queue. */
    DynInstPtr store_inst = store_idx->instruction();
    if (store_idx == storeQueue.begin()) {
        do {
            storeQueue.front().clear();
            storeQueue.pop_front();
        } while (storeQueue.front().completed() && !storeQueue.empty());

        iewStage->updateLSQNextCycle = true;
    }

    stats.sqAvgOccupancy = queueOccupancy(storeQueue);

    DPRINTF(LSQUnit,
            "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

    store_inst->storeTick = curTick() - store_inst->fetchTick;

    if (isStalled() && store_inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit,
                "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    store_inst->setCompleted();

    if (needsTSO && completes_direct_store) {
        storeInFlight = false;
    }

    // Tell the checker we've completed this instruction.  Some stores
    // may get reported twice to the checker, but the checker can
    // handle that case.
    // Store conditionals cannot be sent to the checker yet, they have
    // to update the misc registers first which should take place
    // when they commit
    if (cpu->checker && !store_inst->isStoreConditional()) {
        cpu->checker->verify(store_inst);
    }
}

bool
LSQUnit::trySendPacket(bool isLoad, PacketPtr data_pkt)
{
    bool ret = true;
    bool cache_got_blocked = false;

    LSQRequest *request = dynamic_cast<LSQRequest *>(data_pkt->senderState);
    const bool isMergeBufferPkt = request == nullptr;
    CachePort data_port = CachePort::MergeBufferWrite;
    if (isLoad) {
        if (isMergeBufferPkt) {
            data_port =
                lsq->cachePortAvailable(CachePort::LoadStorePipe0Read)
                    ? CachePort::LoadStorePipe0Read
                    : CachePort::LoadStorePipe1Read;
        } else {
            data_port = readPortFor(request->instruction()->memPipe());
        }
    }

    const bool partial_write =
        !isLoad && data_pkt->isWrite() &&
        !data_pkt->isWholeLineWrite(cpu->cacheLineSize());

    auto rmw_it = partialWriteRMWs.find(data_pkt);
    if (partial_write && rmw_it == partialWriteRMWs.end()) {
        // Cycle X: read the bank. The packet remains local while the old and
        // new bytes merge in X+1, then it is sent during the reserved write
        // phase in X+2.
        constexpr CachePort rmw_read_port = CachePort::MergeBufferRead;
        constexpr CachePort rmw_write_port = CachePort::MergeBufferWrite;
        const bool port_available =
            lsq->cachePortAvailable(rmw_read_port);
        const bool bank_available =
            port_available &&
            lsq->cacheBankAvailable(rmw_read_port, data_pkt->getAddr());
        if (!lsq->cacheBlocked() && bank_available) {
            lsq->cachePortBusy(rmw_read_port, data_pkt->getAddr());
            partialWriteRMWs.emplace(
                data_pkt,
                PartialWriteRMWState{Cycles(2), false, rmw_write_port});
            DPRINTF(LSQUnit,
                    "Started partial-write RMW bank read at addr %#x\n",
                    data_pkt->getAddr());
        } else if (!lsq->cacheBlocked() && port_available) {
            lsq->cacheBankConflict();
            DPRINTF(LSQUnit,
                    "D-cache bank conflict for partial-write RMW read "
                    "at addr %#x\n",
                    data_pkt->getAddr());
        }
        ret = false;
    } else if (partial_write && !rmw_it->second.writeReserved) {
        data_port = rmw_it->second.writePort;
        // X+1 is the merge-only cycle. A write phase which could not reserve
        // its bank at X+2 also waits here for the next reservation cycle.
        ret = false;
    } else {
        const bool bank_reserved =
            partial_write && rmw_it->second.writeReserved;
        if (bank_reserved) {
            data_port = rmw_it->second.writePort;
        }
        const bool port_available =
            bank_reserved || lsq->cachePortAvailable(data_port);
        const bool bank_available =
            bank_reserved ||
            (port_available &&
             lsq->cacheBankAvailable(data_port, data_pkt->getAddr()));

        if (!lsq->cacheBlocked() && bank_available) {
            if (!dcachePort->sendTimingReq(data_pkt)) {
                ret = false;
                cache_got_blocked = true;
            }
        } else {
            ret = false;
            if (!lsq->cacheBlocked() && port_available && !bank_available) {
                lsq->cacheBankConflict();
                DPRINTF(LSQUnit, "D-cache bank conflict for %s at addr %#x\n",
                        isLoad ? "load" : "store", data_pkt->getAddr());
            }
        }

        if (ret) {
            if (bank_reserved) {
                partialWriteRMWs.erase(rmw_it);
            } else {
                lsq->cachePortBusy(data_port, data_pkt->getAddr());
            }
            if (isMergeBufferPkt && !isLoad) {
                assert(data_port == CachePort::MergeBufferWrite);
                ++stats.mbCacheWritePortUses;
                DPRINTF(LSQUnit,
                        "Merge-buffer drain used its cache write port "
                        "at addr %#x\n",
                        data_pkt->getAddr());
            }
        } else if (bank_reserved) {
            rmw_it->second.writeReserved = false;
        }
    }

    if (ret) {
        // isStoreBlocked belongs exclusively to the direct SQ request at
        // storeWBIt. A successful merge-buffer drain must not clear a
        // bank-conflicted SQ store's retry state, or that store remains
        // committed without an outstanding packet or a future retry.
        if (!isLoad && !isMergeBufferPkt) {
            isStoreBlocked = false;
        }
        if (!isMergeBufferPkt) {
            request->packetSent();
        }
    } else {
        if (cache_got_blocked) {
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        if (!isLoad && !isMergeBufferPkt) {
            assert(request == storeWBIt->request());
            isStoreBlocked = true;
        }
        if (!isMergeBufferPkt) {
            request->packetNotSent();
        }
    }
    if (!isMergeBufferPkt) {
        DPRINTF(LSQUnit,
                "Memory request (pkt: %s) from inst [sn:%llu] was"
                " %ssent (cache is blocked: %d, cache_got_blocked: %d)\n",
                data_pkt->print(), request->instruction()->seqNum,
                ret ? "" : "not ", lsq->cacheBlocked(), cache_got_blocked);
    } else {
        DPRINTF(LSQUnit,
                "Merge buffer request (pkt: %s) was %ssent "
                "(cache blocked: %d, cache_got_blocked: %d)\n",
                data_pkt->print(), ret ? "" : "not ", lsq->cacheBlocked(),
                cache_got_blocked);
    }
    return ret;
}

void
LSQUnit::startStaleTranslationFlush()
{
    DPRINTF(LSQUnit, "Unit %p marking stale translations %d %d\n", this,
            storeQueue.size(), loadQueue.size());
    for (auto &entry : storeQueue) {
        if (entry.valid() && entry.hasRequest()) {
            entry.request()->markAsStaleTranslation();
        }
    }
    for (auto &entry : loadQueue) {
        if (entry.valid() && entry.hasRequest()) {
            entry.request()->markAsStaleTranslation();
        }
    }
}

bool
LSQUnit::checkStaleTranslations() const
{
    DPRINTF(LSQUnit, "Unit %p checking stale translations\n", this);
    for (auto &entry : storeQueue) {
        if (entry.valid() && entry.hasRequest() &&
            entry.request()->hasStaleTranslation()) {
            return true;
        }
    }
    for (auto &entry : loadQueue) {
        if (entry.valid() && entry.hasRequest() &&
            entry.request()->hasStaleTranslation()) {
            return true;
        }
    }
    DPRINTF(LSQUnit, "Unit %p found no stale translations\n", this);
    return false;
}

void
LSQUnit::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
}

void
LSQUnit::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loadQueue.size());
    cprintf("Load queue: ");

    for (const auto &e : loadQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", storeQueue.size());
    cprintf("Store queue: ");

    for (const auto &e : storeQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }

    cprintf("\n");
}

void
LSQUnit::schedule(Event &ev, Tick when)
{
    cpu->schedule(ev, when);
}

BaseMMU *
LSQUnit::getMMUPtr()
{
    return cpu->mmu;
}

unsigned int
LSQUnit::cacheLineSize()
{
    return cpu->cacheLineSize();
}

Fault
LSQUnit::read(LSQRequest *request, ssize_t load_idx)
{
    LQEntry &load_entry = loadQueue[load_idx];
    const DynInstPtr &load_inst = load_entry.instruction();

    load_entry.setRequest(request);
    assert(load_inst);

    assert(!load_inst->isExecuted());

    // Make sure this isn't a strictly ordered load
    // A bit of a hackish way to get strictly ordered accesses to work
    // only if they're at the head of the LSQ and are ready to commit
    // (at the head of the ROB too).

    if (request->mainReq()->isStrictlyOrdered() &&
        (load_idx != loadQueue.head() || !load_inst->isAtCommit())) {
        // Tell IQ/mem dep unit that this instruction will need to be
        // rescheduled eventually
        iewStage->rescheduleMemInst(load_inst);
        load_inst->clearIssued();
        load_inst->effAddrValid(false);
        ++stats.rescheduledLoads;
        DPRINTF(LSQUnit, "Strictly ordered load [sn:%lli] PC %s\n",
                load_inst->seqNum, load_inst->pcState());

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        load_entry.setRequest(nullptr);
        request->discard();
        return std::make_shared<GenericISA::M5PanicFault>(
            "Strictly ordered load [sn:%llx] PC %s\n", load_inst->seqNum,
            load_inst->pcState());
    }

    DPRINTF(LSQUnit,
            "[sn:%lli] Read called, load idx: %i, store idx: %i, "
            "storeHead: %i addr: %#x%s\n",
            load_inst->seqNum, load_idx - 1, load_inst->sqIt._idx,
            storeQueue.head() - 1, request->mainReq()->getPaddr(),
            request->isSplit() ? " split" : "");

    if (request->mainReq()->isLLSC()) {
        // Disable recording the result temporarily.  Writing to misc
        // regs normally updates the result, but this is not the
        // desired behavior when handling store conditionals.
        load_inst->recordResult(false);
        load_inst->tcBase()->getIsaPtr()->handleLockedRead(load_inst.get(),
                                                           request->mainReq());
        load_inst->recordResult(true);
    }

    if (request->mainReq()->isLocalAccess()) {
        assert(!load_inst->memData);
        load_inst->memData = new uint8_t[MaxDataBytes];

        gem5::ThreadContext *thread = cpu->tcBase(lsqID);
        PacketPtr main_pkt = new Packet(request->mainReq(), MemCmd::ReadReq);

        main_pkt->dataStatic(load_inst->memData);

        Cycles delay = request->mainReq()->localAccessor(thread, main_pkt);

        WritebackEvent *wb = new WritebackEvent(load_inst, main_pkt, this);
        cpu->schedule(wb, cpu->clockEdge(delay));
        return NoFault;
    }

    // Check the SQ for any previous stores that might lead to forwarding
    auto store_it = load_inst->sqIt;
    assert(store_it >= storeWBIt);
    // End once we've reached the top of the LSQ
    while (store_it != storeWBIt && !load_inst->isDataPrefetch()) {
        // Move the index to one younger
        store_it--;
        assert(store_it->valid());
        assert(store_it->instruction()->seqNum < load_inst->seqNum);
        int store_size = store_it->size();

        // Cache maintenance instructions go down via the store
        // path but they carry no data and they shouldn't be
        // considered for forwarding
        if (store_size != 0 && !store_it->instruction()->strictlyOrdered() &&
            !(store_it->request()->mainReq() &&
              store_it->request()->mainReq()->isCacheMaintenance())) {
            assert(store_it->instruction()->effAddrValid());

            // Check if the store data is within the lower and upper bounds of
            // addresses that the request needs.
            auto req_s = request->mainReq()->getVaddr();
            auto req_e = req_s + request->mainReq()->getSize();
            auto st_s = store_it->instruction()->effAddr;
            auto st_e = st_s + store_size;

            bool store_has_lower_limit = req_s >= st_s;
            bool store_has_upper_limit = req_e <= st_e;
            bool lower_load_has_store_part = req_s < st_e;
            bool upper_load_has_store_part = req_e > st_s;

            auto coverage = AddrRangeCoverage::NoAddrRangeCoverage;

            // If the store entry is not atomic (atomic does not have valid
            // data), the store has all of the data needed, and
            // the load is not LLSC, then
            // we can forward data from the store to the load
            if (!store_it->instruction()->isAtomic() &&
                store_has_lower_limit && store_has_upper_limit &&
                !request->mainReq()->isLLSC()) {

                const auto &store_req = store_it->request()->mainReq();
                coverage = store_req->isMasked()
                               ? AddrRangeCoverage::PartialAddrRangeCoverage
                               : AddrRangeCoverage::FullAddrRangeCoverage;
            } else if (
                // This is the partial store-load forwarding case where a store
                // has only part of the load's data and the load isn't LLSC
                (!request->mainReq()->isLLSC() &&
                 ((store_has_lower_limit && lower_load_has_store_part) ||
                  (store_has_upper_limit && upper_load_has_store_part) ||
                  (lower_load_has_store_part && upper_load_has_store_part))) ||
                // The load is LLSC, and the store has all or part of the
                // load's data
                (request->mainReq()->isLLSC() &&
                 ((store_has_lower_limit || upper_load_has_store_part) &&
                  (store_has_upper_limit || lower_load_has_store_part))) ||
                // The store entry is atomic and has all or part of the load's
                // data
                (store_it->instruction()->isAtomic() &&
                 ((store_has_lower_limit || upper_load_has_store_part) &&
                  (store_has_upper_limit || lower_load_has_store_part)))) {

                coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
            }

            if (coverage == AddrRangeCoverage::FullAddrRangeCoverage) {
                // Get shift amount for offset into the store's data.
                int shift_amt = request->mainReq()->getVaddr() -
                                store_it->instruction()->effAddr;

                // Allocate memory if this is the first time a load is issued.
                if (!load_inst->memData) {
                    load_inst->memData =
                        new uint8_t[request->mainReq()->getSize()];
                }
                if (store_it->isAllZeros()) {
                    memset(load_inst->memData, 0,
                           request->mainReq()->getSize());
                } else {
                    memcpy(load_inst->memData, store_it->data() + shift_amt,
                           request->mainReq()->getSize());
                }

                DPRINTF(LSQUnit,
                        "Forwarding from store idx %i to load to "
                        "addr %#x\n",
                        store_it._idx, request->mainReq()->getVaddr());

                PacketPtr data_pkt =
                    new Packet(request->mainReq(), MemCmd::ReadReq);
                data_pkt->dataStatic(load_inst->memData);

                // hardware transactional memory
                // Store to load forwarding within a transaction
                // This should be okay because the store will be sent to
                // the memory subsystem and subsequently get added to the
                // write set of the transaction. The write set has a stronger
                // property than the read set, so the load doesn't necessarily
                // have to be there.
                assert(!request->mainReq()->isHTMCmd());
                if (load_inst->inHtmTransactionalState()) {
                    assert(!storeQueue[store_it._idx].completed());
                    assert(storeQueue[store_it._idx]
                               .instruction()
                               ->inHtmTransactionalState());
                    assert(load_inst->getHtmTransactionUid() ==
                           storeQueue[store_it._idx]
                               .instruction()
                               ->getHtmTransactionUid());
                    data_pkt->setHtmTransactional(
                        load_inst->getHtmTransactionUid());
                    DPRINTF(HtmCpu,
                            "HTM LD (ST2LDF) "
                            "pc=0x%lx - vaddr=0x%lx - "
                            "paddr=0x%lx - htmUid=%u\n",
                            load_inst->pcState().instAddr(),
                            data_pkt->req->hasVaddr()
                                ? data_pkt->req->getVaddr()
                                : 0lu,
                            data_pkt->getAddr(),
                            load_inst->getHtmTransactionUid());
                }

                if (request->isAnyOutstandingRequest()) {
                    assert(request->_numOutstandingPackets > 0);
                    // There are memory requests packets in flight already.
                    // This may happen if the store was not complete the
                    // first time this load got executed. Signal the senderSate
                    // that response packets should be discarded.
                    request->discard();
                    // Avoid checking snoops on this discarded request.
                    load_entry.setRequest(nullptr);
                }

                WritebackEvent *wb =
                    new WritebackEvent(load_inst, data_pkt, this);

                // We'll say this has a 1 cycle load-store forwarding latency
                // for now.
                // @todo: Need to make this a parameter.
                cpu->schedule(wb, curTick());

                // Don't need to do anything special for split loads.
                ++stats.forwLoads;

                return NoFault;
            } else if (coverage ==
                       AddrRangeCoverage::PartialAddrRangeCoverage) {
                // If it's already been written back, then don't worry about
                // stalling on it.
                if (store_it->completed()) {
                    panic("Should not check one of these");
                    continue;
                }

                // Must stall load and force it to retry, so long as it's the
                // oldest load that needs to do so.
                if (!stalled ||
                    (stalled &&
                     load_inst->seqNum <
                         loadQueue[stallingLoadIdx].instruction()->seqNum)) {
                    stalled = true;
                    stallingStoreIsn = store_it->instruction()->seqNum;
                    stallingLoadIdx = load_idx;
                }

                // Tell IQ/mem dep unit that this instruction will need to be
                // rescheduled eventually
                iewStage->rescheduleMemInst(load_inst);
                load_inst->clearIssued();
                load_inst->effAddrValid(false);
                ++stats.rescheduledLoads;
                ++stats.sqPartialFwdRescheduledLoads;

                // Do not generate a writeback event as this instruction is not
                // complete.
                DPRINTF(LSQUnit,
                        "Load-store forwarding mis-match. "
                        "Store idx %i to load addr %#x\n",
                        store_it._idx, request->mainReq()->getVaddr());

                // Must discard the request.
                request->discard();
                load_entry.setRequest(nullptr);
                return NoFault;
            }
        }
    }

    Addr stallBlockAddr = 0;
    // Check merge buffer entries for forwarding.
    if (mergeBufferEnabled) {
        AddrRangeCoverage coverage;
        if (request->isSplit()) {
            Addr first_part_addr = request->req(0)->getPaddr();
            size_t first_access_size = request->req(0)->getSize();
            Addr second_part_addr = request->req(1)->getPaddr();
            size_t second_access_size = request->req(1)->getSize();

            coverage = mergeBuffer.forwardCoverage(first_part_addr,
                                                   first_access_size);

            // TODO: forward from both MB entries if possible
            if (coverage == AddrRangeCoverage::NoAddrRangeCoverage) {
                coverage = mergeBuffer.forwardCoverage(second_part_addr,
                                                       second_access_size);
                stallBlockAddr = second_part_addr & cacheBlockMask;
            } else {
                stallBlockAddr = first_part_addr & cacheBlockMask;
            }

            // For split requests, even if we can fully forward the data from
            // the MB, we replay the load only after the MB is drained
            if (coverage != AddrRangeCoverage::NoAddrRangeCoverage) {
                coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
            }

        } else {
            coverage = mergeBuffer.forwardCoverage(
                request->mainReq()->getPaddr(), request->mainReq()->getSize());
        }

        if (coverage == AddrRangeCoverage::FullAddrRangeCoverage) {
            if (!load_inst->memData) {
                load_inst->memData =
                    new uint8_t[request->mainReq()->getSize()];
            }
            if (mergeBuffer.forwardData(request->mainReq()->getPaddr(),
                                        load_inst->memData,
                                        request->mainReq()->getSize())) {

                DPRINTF(LSQUnit,
                        "Forwarding from merge buffer to load to "
                        "addr %#x\n",
                        request->mainReq()->getVaddr());

                PacketPtr data_pkt =
                    new Packet(request->mainReq(), MemCmd::ReadReq);
                data_pkt->dataStatic(load_inst->memData);

                if (request->isAnyOutstandingRequest()) {
                    assert(request->_numOutstandingPackets > 0);
                    // There are memory requests packets in flight already.
                    // This may happen if the store was not complete the
                    // first time this load got executed. Signal the senderSate
                    // that response packets should be discarded.
                    request->discard();
                    // Avoid checking snoops on this discarded request.
                    load_entry.setRequest(nullptr);
                }

                ++stats.mbForwards;

                // load_inst->setExecuted();
                // load_inst->completeAcc(nullptr);
                // request->packetSent();
                // request->complete();
                WritebackEvent *wb =
                    new WritebackEvent(load_inst, data_pkt, this);
                cpu->schedule(wb, cpu->clockEdge(Cycles(1)));
                return NoFault;
            }
        } else if (coverage == AddrRangeCoverage::PartialAddrRangeCoverage) {
            if (!stalled ||
                (stalled &&
                 load_inst->seqNum <
                     loadQueue[stallingLoadIdx].instruction()->seqNum)) {
                stalled = true;
                if (!request->isSplit()) {
                    stallingMBAddr =
                        request->mainReq()->getPaddr() & cacheBlockMask;
                } else {
                    stallingMBAddr = stallBlockAddr;
                }
                stallingStoreIsn = 0;
                stallingLoadIdx = load_idx;
            }

            // Tell IQ/mem dep unit that this instruction will need to be
            // rescheduled eventually
            iewStage->rescheduleMemInst(load_inst);
            load_inst->clearIssued();
            load_inst->effAddrValid(false);
            ++stats.rescheduledLoads;
            ++stats.mbPartialFwdRescheduledLoads;

            // Do not generate a writeback event as this instruction is not
            // complete.
            DPRINTF(LSQUnit,
                    "Load-store forwarding mis-match. "
                    "Merge buffer to load addr %#x\n",
                    request->mainReq()->getVaddr());

            // Must discard the request.
            request->discard();
            load_entry.setRequest(nullptr);

            return NoFault;
        }
    }

    // If there's no forwarding case, then go access memory
    DPRINTF(LSQUnit, "Doing memory access for inst [sn:%lli] PC %s\n",
            load_inst->seqNum, load_inst->pcState());

    // Allocate memory if this is the first time a load is issued.
    if (!load_inst->memData) {
        load_inst->memData = new uint8_t[request->mainReq()->getSize()];
    }

    // hardware transactional memory
    if (request->mainReq()->isHTMCmd()) {
        // this is a simple sanity check
        // the Ruby cache controller will set
        // memData to 0x0ul if successful.
        *load_inst->memData = (uint64_t)0x1ull;
    }

    // For now, load throughput is constrained by the number of
    // load FUs only, and loads do not consume a cache port (only
    // stores do).
    // @todo We should account for cache port contention
    // and arbitrate between loads and stores.

    // if we the cache is not blocked, do cache access
    // if the request is not sent and cache is unblocked
    // then put the instruction into retry queue so we do not need
    // an extra cycle to re-issue and execute
    request->buildPackets();
    request->sendPacketToCache();
    if (!request->isSent()) {
        if (!lsq->cacheBlocked()) {
            iewStage->retryMemInst(load_inst);
        } else {
            iewStage->blockMemInst(load_inst);
        }
    }

    return NoFault;
}

Fault
LSQUnit::write(LSQRequest *request, uint8_t *data, ssize_t store_idx)
{
    assert(storeQueue[store_idx].valid());

    DPRINTF(LSQUnit,
            "Doing write to store idx %i, addr %#x | storeHead:%i "
            "[sn:%llu]\n",
            store_idx - 1, request->req()->getPaddr(), storeQueue.head() - 1,
            storeQueue[store_idx].instruction()->seqNum);

    storeQueue[store_idx].setRequest(request);
    unsigned size = request->_size;
    storeQueue[store_idx].size() = size;
    bool store_no_data =
        request->mainReq()->getFlags() & Request::STORE_NO_DATA;
    storeQueue[store_idx].isAllZeros() = store_no_data;
    assert(size <= SQEntry::DataSize || store_no_data);

    // copy data into the storeQueue only if the store request has valid data
    if (!(request->req()->getFlags() & Request::CACHE_BLOCK_ZERO) &&
        !request->req()->isCacheMaintenance() && !request->req()->isAtomic()) {
        memcpy(storeQueue[store_idx].data(), data, size);
    }

    // This function only writes the data to the store queue, so no fault
    // can happen here.
    return NoFault;
}

InstSeqNum
LSQUnit::getLoadHeadSeqNum()
{
    if (loadQueue.front().valid()) {
        return loadQueue.front().instruction()->seqNum;
    } else {
        return 0;
    }
}

InstSeqNum
LSQUnit::getStoreHeadSeqNum()
{
    if (storeQueue.front().valid()) {
        return storeQueue.front().instruction()->seqNum;
    } else {
        return 0;
    }
}

LSQEntry::~LSQEntry()
{
    if (_request != nullptr) {
        _request->freeLSQEntry();
        _request = nullptr;
    }
}

LSQUnit::MergeBuffer::MergeBufferEntry *
LSQUnit::MergeBuffer::addStore(Cycles now, Addr addr, uint8_t *data,
                               size_t size,
                               typename StoreQueue::iterator store_it,
                               bool is_all_zero)
{
    Addr currAddr = addr;
    size_t remaining = size;
    MergeBufferEntry *last_entry = nullptr;

    while (remaining > 0) {
        Addr lineAddr = currAddr & ~(lineSize - 1);
        uint32_t offset = currAddr & (lineSize - 1);
        size_t chunk = std::min(lineSize - offset, remaining);

        auto it = std::find_if(entries.begin(), entries.end(),
                               [lineAddr](const MergeBufferEntry &e) {
                                   return e.valid && e.blockAddr == lineAddr;
                               });

        if (it != entries.end()) {

            if (it->isAtomic) {
                DPRINTF(LSQUnit,
                        "Blocking merge into atomic MB entry Addr:%#x\n",
                        lineAddr);
                return nullptr;
            }

            // A partial write keeps its packet while the banked RMW read,
            // merge cycle, and reserved write complete. The packet contains
            // a snapshot of blockData, so accepting another store after it
            // has been built would lose the newer bytes when that packet
            // drains.
            if (it->drainPkt) {
                DPRINTF(LSQUnit,
                        "Blocking merge into MB entry Addr:%#x with a "
                        "staged drain packet\n",
                        lineAddr);
                return nullptr;
            }

            if (lsqPtr && &(*it) != &entries.back()) {
                const bool mergeable_state =
                    it->state == EntryState::MERGING ||
                    (it->state == EntryState::RETIRED &&
                     it->unretireCount < maxUnretire);
                if (mergeable_state) {
                    ++lsqPtr->stats.mbTsoBlockedMergeOpportunities;
                    ++lsqPtr->stats.mbTsoMergeBlockedByAllocationOrder;

                    const size_t sq_pressure_count =
                        (lsqPtr->storeQueue.capacity() *
                             lsqPtr->mergeBufferSqPressureThreshold +
                         99) /
                        100;
                    if (lsqPtr->storeQueue.size() >= sq_pressure_count) {
                        ++lsqPtr->stats.mbTsoBlockedMergesUnderSqPressure;
                    }

                    const size_t free_entries = numEntries - entries.size();
                    if (free_entries <
                        lsqPtr->mergeBufferFreeEntryPressureThreshold) {
                        ++lsqPtr->stats.mbTsoBlockedMergesUnderMbPressure;
                    }
                }
                DPRINTF(LSQUnit,
                        "Blocking merge for Addr:%#x; matching MB "
                        "entry is not the most recent allocation\n",
                        lineAddr);
                return nullptr;
            }

            DPRINTF(LSQUnit,
                    "Found an existing MB entry for Addr:%#x retiring in %lu, "
                    "now:%lu\n",
                    lineAddr, it->retireCycle, now);
            if (it->state == EntryState::RETIRED &&
                it->unretireCount < maxUnretire) {
                // Allow unretire if new data arrives later.
                it->state = EntryState::MERGING;
                it->retireCycle = now + retireWindow;
                it->unretireCount++;
                if (lsqPtr) {
                    lsqPtr->stats.mbUnretire++;
                }
            } else if (it->state != EntryState::MERGING) {
                DPRINTF(LSQUnit, "MB entry for Addr:%#x marked %s\n", lineAddr,
                        (it->state == EntryState::RETIRED) ? "RETIRED"
                        : (it->state == EntryState::DRAINING)
                            ? "DRAINING"
                            : "FORCE_RETIRED");
                return nullptr;
            }

            updateEntry(*it, data + (currAddr - addr), offset, chunk,
                        is_all_zero);

            if (!it->baseReq) {
                it->baseReq = std::make_shared<Request>(
                    *(store_it->request()->mainReq()));
            }
            if (resetRetireOnMerge) {
                it->retireCycle = now + resetRetireWindow;
            }
            if (lsqPtr) {
                lsqPtr->stats.mbMerges++;
            }
            last_entry = &(*it);
        } else {
            if (entries.size() >= numEntries) {
                return nullptr;
            }
            MergeBufferEntry newEntry(lineSize);
            newEntry.blockAddr = lineAddr;
            newEntry.valid = true;
            newEntry.allocCycle = now;
            newEntry.retireCycle = now + retireWindow;
            newEntry.baseReq =
                std::make_shared<Request>(*(store_it->request()->mainReq()));
            updateEntry(newEntry, data + (currAddr - addr), offset, chunk,
                        is_all_zero);

            entries.push_back(std::move(newEntry));
            last_entry = &entries.back();

            if (lsqPtr && lsqPtr->mergeBufferPrefetchEnabled &&
                lsqPtr->mergeBufferPfInFlight == 0) {
                // Prefetch the cache line to speed up later drains.
                RequestPtr base = store_it->request()->mainReq();
                Request::Flags flags = base->getFlags() | Request::PREFETCH;
                RequestorID rid = base->requestorId();
                RequestPtr pf_req =
                    std::make_shared<Request>(lineAddr, lineSize, flags, rid);
                if (base->hasContextId()) {
                    pf_req->setContext(base->contextId());
                }
                if (base->hasPC()) {
                    pf_req->setPC(base->getPC());
                }
                pf_req->taskId(base->taskId());

                PacketPtr pf_pkt = Packet::createRead(pf_req);
                // Use a soft prefetch so it can go through cache/MSHR
                // normally.
                pf_pkt->cmd = MemCmd::SoftPFReq;
                // Give the packet a data buffer to satisfy downstream asserts.
                pf_pkt->allocate();
                pf_pkt->senderState =
                    new MergeBufferPrefetchSenderState(lsqPtr);

                // Merge-buffer prefetches are opportunistic. Respect both
                // the read-port and bank constraints, but drop a rejected
                // prefetch instead of marking the LSQ cache-blocked and
                // stalling demand traffic behind it.
                const CachePort port =
                    lsqPtr->lsq->cachePortAvailable(
                        CachePort::LoadStorePipe0Read)
                        ? CachePort::LoadStorePipe0Read
                        : CachePort::LoadStorePipe1Read;
                const bool port_available =
                    lsqPtr->lsq->cachePortAvailable(port);
                const bool bank_available =
                    port_available &&
                    lsqPtr->lsq->cacheBankAvailable(port, lineAddr);
                if (!lsqPtr->lsq->cacheBlocked() && bank_available &&
                    lsqPtr->dcachePort->sendTimingReq(pf_pkt)) {
                    lsqPtr->lsq->cachePortBusy(port, lineAddr);
                    ++lsqPtr->mergeBufferPfInFlight;
                    last_entry->prefetchIssued = true;
                    last_entry->prefetchIssueCycle = now;
                } else {
                    delete static_cast<MergeBufferPrefetchSenderState *>(
                        pf_pkt->senderState);
                    delete pf_pkt;
                }
            }

            if (lsqPtr) {
                lsqPtr->stats.mbAllocations++;
                lsqPtr->stats.mbAvgOccupancy =
                    (double)entries.size() / (double)numEntries;
            }
            // Reset unretire count on new allocations
            last_entry->unretireCount = 0;
            DPRINTF(LSQUnit,
                    "Allocating a new MB entry for Addr:%#x retiring in %lu, "
                    "now: %lu\n",
                    lineAddr, now + retireWindow, now);
        }

        currAddr += chunk;
        remaining -= chunk;
    }

    return last_entry;
}

LSQUnit::MergeBuffer::MergeBufferEntry *
LSQUnit::MergeBuffer::addAtomic(Cycles now, LSQRequest *request,
                                typename StoreQueue::iterator store_it,
                                uint64_t version)
{
    if (request->isSplit()) {
        return nullptr;
    }

    if (entries.size() >= numEntries) {
        return nullptr;
    }

    const Addr paddr = request->mainReq()->getPaddr();
    const Addr lineAddr = paddr & ~(lineSize - 1);

    // A preceding buffered store to this line must become visible before the
    // atomic reads it. Later stores are blocked from merging into an atomic
    // entry by addStore().
    if (std::any_of(entries.begin(), entries.end(),
                    [lineAddr](const MergeBufferEntry &entry) {
                        return entry.valid && entry.blockAddr == lineAddr;
                    })) {
        return nullptr;
    }

    MergeBufferEntry newEntry(lineSize);
    newEntry.blockAddr = lineAddr;
    newEntry.baseReq =
        std::make_shared<Request>(*(store_it->request()->mainReq()));
    newEntry.state = EntryState::RETIRED;
    newEntry.retireCycle = now;
    newEntry.allocCycle = now;
    newEntry.unretireCount = 0;
    newEntry.valid = true;
    newEntry.isAtomic = true;
    newEntry.atomicReq = store_it->request();
    newEntry.version = version;

    entries.push_back(std::move(newEntry));

    if (lsqPtr) {
        lsqPtr->stats.mbAllocations++;
        lsqPtr->stats.mbAvgOccupancy =
            (double)entries.size() / (double)numEntries;
    }

    DPRINTF(LSQUnit,
            "Allocating a new atomic MB entry for Addr:%#x "
            "now:%lu\n",
            lineAddr, now);

    return &entries.back();
}

bool
LSQUnit::MergeBuffer::canAcceptSplitStore(LSQRequest *request,
                                          uint64_t version,
                                          bool &mb_full) const
{
    mb_full = false;
    auto req0 = request->req(0);
    auto req1 = request->req(1);

    std::vector<Addr> blocks;
    auto append_blocks = [this, &blocks](Addr addr, size_t size) {
        for (Addr current = addr; current < addr + size;) {
            const Addr block = current & ~(lineSize - 1);
            if (std::find(blocks.begin(), blocks.end(), block) ==
                blocks.end()) {
                blocks.push_back(block);
            }
            current = std::min<Addr>(block + lineSize, addr + size);
        }
    };
    append_blocks(req0->getPaddr(), req0->getSize());
    append_blocks(req1->getPaddr(), req1->getSize());

    size_t planned_allocations = 0;
    bool planned_merge = false;
    for (const Addr block : blocks) {
        auto it =
            std::find_if(entries.begin(), entries.end(),
                         [block](const MergeBufferEntry &entry) {
                             return entry.valid && entry.blockAddr == block;
                         });

        if (it == entries.end()) {
            ++planned_allocations;
            continue;
        }
        if (it->isAtomic || it->drainPkt ||
            (it->state != EntryState::MERGING &&
             !(it->state == EntryState::RETIRED &&
               it->unretireCount < maxUnretire))) {
            return false;
        }
        if (lsqPtr && &(*it) != &entries.back()) {
            return false;
        }
        planned_merge = true;
    }

    if (entries.size() + planned_allocations > numEntries) {
        mb_full = true;
        return false;
    }

    // addStore() processes the fragments separately. In TSO mode an
    // allocation changes which entry is the newest, so a split store that
    // mixes an existing-entry merge with an allocation cannot be admitted
    // atomically.
    if (lsqPtr && planned_merge && planned_allocations != 0) {
        return false;
    }

    return true;
}

bool
LSQUnit::MergeBuffer::canForward(Addr paddr, size_t size) const
{
    Addr end = paddr + size;
    for (const auto &entry : entries) {
        if (!entry.valid) {
            continue;
        }
        if (entry.isAtomic) {
            continue;
        }
        Addr blk_start = entry.blockAddr;
        Addr blk_end = entry.blockAddr + lineSize;
        if (end <= blk_start || paddr >= blk_end) {
            continue;
        }
        size_t offset = paddr - blk_start;
        size_t to_check = std::min<size_t>(size, lineSize - offset);
        bool all_present = true;
        for (size_t i = 0; i < to_check; ++i) {
            if (!entry.byteValids[offset + i]) {
                all_present = false;
                break;
            }
        }
        if (all_present) {
            return true;
        }
    }
    return false;
}

LSQUnit::AddrRangeCoverage
LSQUnit::MergeBuffer::forwardCoverage(Addr paddr, size_t size) const
{
    Addr end = paddr + size;
    for (const auto &entry : entries) {
        if (!entry.valid) {
            continue;
        }
        if (entry.isAtomic) {
            continue;
        }
        Addr blk_start = entry.blockAddr;
        Addr blk_end = entry.blockAddr + lineSize;
        if (end <= blk_start || paddr >= blk_end) {
            continue;
        }
        size_t offset = paddr - blk_start;
        bool fits = (offset + size) <= lineSize;
        size_t to_check = std::min<size_t>(size, lineSize - offset);
        bool all_present = true;
        bool any_present = false;
        for (size_t i = 0; i < to_check; ++i) {
            if (entry.byteValids[offset + i]) {
                any_present = true;
            } else {
                all_present = false;
            }
        }
        if (!any_present) {
            continue;
        }
        if (fits && all_present) {
            return AddrRangeCoverage::FullAddrRangeCoverage;
        }

        return AddrRangeCoverage::PartialAddrRangeCoverage;
    }
    return AddrRangeCoverage::NoAddrRangeCoverage;
}

bool
LSQUnit::MergeBuffer::forwardData(Addr paddr, uint8_t *dst, size_t size) const
{
    Addr end = paddr + size;
    for (const auto &entry : entries) {
        if (!entry.valid) {
            continue;
        }
        if (entry.isAtomic) {
            continue;
        }
        Addr blk_start = entry.blockAddr;
        Addr blk_end = entry.blockAddr + lineSize;
        if (end <= blk_start || paddr >= blk_end) {
            continue;
        }
        size_t offset = paddr - blk_start;
        bool fits = (offset + size) <= lineSize;
        size_t to_copy = std::min<size_t>(size, lineSize - offset);
        bool all_present = true;
        for (size_t i = 0; i < to_copy; ++i) {
            if (!entry.byteValids[offset + i]) {
                all_present = false;
                break;
            }
        }
        if (!all_present || !fits) {
            continue;
        }
        std::memcpy(dst, &entry.blockData[offset], to_copy);
        return true;
    }
    return false;
}

void
LSQUnit::MergeBuffer::updateRetiredEntries(Cycles now)
{
    for (auto &entry : entries) {
        if (entry.valid && entry.state == EntryState::MERGING) {

            bool all_valid =
                std::all_of(entry.byteValids.begin(), entry.byteValids.end(),
                            [](bool v) { return v; });

            if (now >= entry.retireCycle ||
                (lsqPtr && lsqPtr->mbRetireWhenFullValid && all_valid)) {
                entry.state = EntryState::RETIRED;
                if (lsqPtr) {
                    lsqPtr->stats.mbRetired++;
                }
            }
        }
    }
}

void
LSQEntry::clear()
{
    _inst = nullptr;
    if (_request != nullptr) {
        _request->freeLSQEntry();
    }
    _request = nullptr;
    _valid = false;
    _size = 0;
}

void
LSQEntry::set(const DynInstPtr &new_inst)
{
    assert(!_valid);
    _inst = new_inst;
    _valid = true;
    _size = 0;
}

SQEntry::SQEntry()
{
    std::memset(_data, 0, DataSize);
}

void
SQEntry::set(const DynInstPtr &inst)
{
    LSQEntry::set(inst);
}

void
SQEntry::clear()
{
    LSQEntry::clear();
    _canWB = _completed = _committed = _isAllZeros = false;
}

LSQUnit::LSQUnit(const LSQUnit &l) : stats(nullptr)
{
    panic("LSQUnit is not copy-able");
}

void
LSQUnit::MergeBuffer::reset()
{
    for (auto &entry : entries) {
        if (!entry.drainPkt) {
            continue;
        }
        if (lsqPtr) {
            lsqPtr->partialWriteRMWs.erase(entry.drainPkt);
        }
        delete static_cast<MergeBufferDrainSenderState *>(
            entry.drainPkt->senderState);
        delete entry.drainPkt;
    }
    entries.clear();
}

void
LSQUnit::MergeBuffer::forceRetireAll()
{
    for (auto &entry : entries) {
        if (!entry.valid) {
            continue;
        }
        if (entry.state == EntryState::MERGING ||
            entry.state == EntryState::RETIRED) {
            entry.state = EntryState::FORCE_RETIRED;
            entry.retireCycle = Cycles(0);
        }
    }
}

void
LSQUnit::MergeBuffer::forceRetireVersionsBefore(uint64_t version)
{
    for (auto &entry : entries) {
        if (!entry.valid || entry.version >= version) {
            continue;
        }
        if (entry.state == EntryState::MERGING ||
            entry.state == EntryState::RETIRED) {
            entry.state = EntryState::FORCE_RETIRED;
            entry.retireCycle = Cycles(0);
            if (lsqPtr) {
                ++lsqPtr->stats.mbVersionAdvanceForceRetires;
            }
        }
    }
}

std::optional<uint64_t>
LSQUnit::MergeBuffer::oldestVersion() const
{
    // Stub: versioning not enabled, return nullopt.
    return std::nullopt;
}

size_t
LSQUnit::MergeBuffer::indexOf(const MergeBufferEntry *entry) const
{
    size_t idx = 0;
    for (const auto &e : entries) {
        if (&e == entry) {
            return idx;
        }
        ++idx;
    }
    return std::numeric_limits<size_t>::max();
}

std::optional<uint64_t>
LSQUnit::MergeBuffer::youngestVersion() const
{
    if (versionCounts.empty()) {
        return std::nullopt;
    }
    return versionCounts.front().first;
}

std::optional<uint64_t>
LSQUnit::youngestMBVersion() const
{
    if (!mergeBufferEnabled) {
        return std::nullopt;
    }
    return mergeBuffer.youngestVersion();
}

bool
LSQUnit::loadBlockedByMBVersion(uint64_t version) const
{
    if (!mergeBufferEnabled || !cpu->versioningEnabled()) {
        return false;
    }

    auto youngest = mergeBuffer.youngestVersion();
    if (!youngest) {
        return false;
    }

    return version > *youngest;
}

bool
LSQUnit::MergeBuffer::hasDrainableEntry() const
{
    return std::any_of(
        entries.begin(), entries.end(), [](const MergeBufferEntry &entry) {
            return entry.valid && (entry.state == EntryState::RETIRED ||
                                   entry.state == EntryState::FORCE_RETIRED);
        });
}

bool
LSQUnit::MergeBuffer::drainOne(LSQUnit *lsq_ptr)
{
    auto is_drainable = [](const MergeBufferEntry &entry) {
        return entry.valid &&
               (entry.state == EntryState::RETIRED ||
                entry.state == EntryState::FORCE_RETIRED);
    };

    // Oldest entry is at the front of the list; enforce FIFO draining in TSO.
    auto it = entries.begin();
    while (it != entries.end() && !it->valid) {
        it = entries.erase(it);
    }

    if (lsq_ptr->needsTSO) {
        if (it == entries.end() || !is_drainable(*it)) {
            return false;
        }
    } else {
        auto has_reserved_write = [&](MergeBufferEntry &entry) {
            if (!is_drainable(entry) || !entry.drainPkt) {
                return false;
            }
            auto rmw = lsq_ptr->partialWriteRMWs.find(entry.drainPkt);
            return rmw != lsq_ptr->partialWriteRMWs.end() &&
                   rmw->second.writeReserved;
        };
        auto can_begin_send = [&](MergeBufferEntry &entry) {
            return is_drainable(entry) && entry.drainPkt &&
                   !lsq_ptr->partialWriteRMWs.contains(entry.drainPkt);
        };
        auto has_older_valid_entry = [&](const MergeBufferEntry &entry) {
            for (auto older = entries.begin();
                 older != entries.end() && &(*older) != &entry; ++older) {
                if (older->valid) {
                    return true;
                }
            }
            return false;
        };
        auto can_start = [&](MergeBufferEntry &entry) {
            if (!is_drainable(entry) || entry.drainPkt) {
                return false;
            }
            if (entry.isAtomic && entry.isRelease &&
                has_older_valid_entry(entry)) {
                ++lsq_ptr->stats.mbReleaseWaitCycles;
                return false;
            }
            if (entry.isRelease) {
                const bool deps_clear =
                    std::none_of(entry.waitBits.begin(), entry.waitBits.end(),
                                 [](bool value) { return value; });
                if (!deps_clear) {
                    ++lsq_ptr->stats.mbReleaseWaitCycles;
                    return false;
                }
            }
            return true;
        };

        // Consume an RMW write whose X+2 bank was reserved before choosing
        // any packet that would begin a new RMW read.
        it = std::find_if(it, entries.end(), has_reserved_write);
        if (it == entries.end()) {
            it = std::find_if(entries.begin(), entries.end(), can_begin_send);
        }
        if (it == entries.end()) {
            it = std::find_if(entries.begin(), entries.end(), can_start);
        }
        if (it == entries.end()) {
            dumpWaitBits();
            return false;
        }
    }

    MergeBufferEntry &entry = *it;

    assert(entry.baseReq);
    RequestPtr base = entry.baseReq;

    if (entry.isAtomic) {
        PacketPtr pkt = entry.drainPkt;
        if (!pkt) {
            RequestPtr atomic_req = std::make_shared<Request>(*base);
            pkt = Packet::createWrite(atomic_req);
            if (atomic_req->getSize() > 0) {
                uint8_t *buf = new uint8_t[atomic_req->getSize()];
                std::memset(buf, 0, atomic_req->getSize());
                pkt->dataDynamic(buf);
            }
            pkt->senderState =
                new MergeBufferDrainSenderState(&entry, lsq_ptr);
            entry.drainPkt = pkt;
        }

        const bool rmw_pending =
            lsq_ptr->partialWriteRMWs.contains(pkt);
        if (!lsq_ptr->trySendPacket(false, pkt)) {
            return !rmw_pending &&
                   lsq_ptr->partialWriteRMWs.contains(pkt);
        }
        entry.drainPkt = nullptr;

        DPRINTF(LSQUnit,
                "Sending an atomic MB drain request for addr %#x "
                "version ver:%llu now:%lli\n",
                base->getPaddr(), entry.version, lsqPtr->cpu->curCycle());

        if (lsqPtr) {
            lsqPtr->stats.mbDrains++;
        }

        entry.drainIssueCycle = lsq_ptr->cpu->curCycle();
        if (entry.prefetchIssued) {
            lsq_ptr->stats.mbPrefetchToDrainLeadTime.sample(
                entry.drainIssueCycle - entry.prefetchIssueCycle);
        }

        if (lsq_ptr->needsTSO) {
            lsq_ptr->storeInFlight = true;
        }

        entry.state = EntryState::DRAINING;
        return true;
    }

    PacketPtr pkt = entry.drainPkt;
    if (!pkt) {
        Request::Flags flags = base->getFlags();
        RequestorID rid = base->requestorId();
        RequestPtr merged_req =
            std::make_shared<Request>(entry.blockAddr, lineSize, flags, rid);

        std::vector<bool> byte_enable = entry.byteValids;
        bool full_line = std::find(byte_enable.begin(), byte_enable.end(),
                                   false) == byte_enable.end();

        if (base->hasContextId()) {
            merged_req->setContext(base->contextId());
        }
        if (base->hasPC()) {
            merged_req->setPC(base->getPC());
        }
        merged_req->taskId(base->taskId());
        merged_req->setByteEnable(byte_enable);

        pkt = full_line ? new Packet(merged_req, MemCmd::WriteLineReq)
                        : Packet::createWrite(merged_req);
        uint8_t *buf = new uint8_t[lineSize];
        std::memcpy(buf, entry.blockData.data(), lineSize);
        pkt->dataDynamic(buf);
        pkt->senderState = new MergeBufferDrainSenderState(&entry, lsq_ptr);
        entry.drainPkt = pkt;
    }

    const bool rmw_pending = lsq_ptr->partialWriteRMWs.contains(pkt);
    if (!lsq_ptr->trySendPacket(false, pkt)) {
        return !rmw_pending && lsq_ptr->partialWriteRMWs.contains(pkt);
    }
    entry.drainPkt = nullptr;

    if (lsqPtr) {
        lsqPtr->stats.mbDrains++;
    }

    entry.drainIssueCycle = lsq_ptr->cpu->curCycle();
    if (entry.prefetchIssued) {
        lsq_ptr->stats.mbPrefetchToDrainLeadTime.sample(
            entry.drainIssueCycle - entry.prefetchIssueCycle);
    }

    if (lsq_ptr->needsTSO) {
        lsq_ptr->storeInFlight = true;
    }

    entry.state = EntryState::DRAINING;
    return true;
}

void
LSQUnit::MergeBuffer::dumpWaitBits() const
{
    auto stateStr = [](EntryState state) -> const char * {
        switch (state) {
            case EntryState::MERGING:
                return "MERGING";
            case EntryState::RETIRED:
                return "RETIRED";
            case EntryState::DRAINING:
                return "DRAINING";
            case EntryState::FORCE_RETIRED:
                return "FORCE_RETIRED";
        }
        return "UNKNOWN";
    };

    size_t idx = 0;
    for (const auto &e : entries) {
        if (!e.valid) {
            ++idx;
            continue;
        }
        std::string bits;
        bits.reserve(e.waitBits.size());
        for (bool b : e.waitBits) {
            bits.push_back(b ? '1' : '0');
        }
        DPRINTF(LSQUnit,
                "MB[%llu] ver:%llu state:%s release:%d waitBits:%s "
                "alloc:%llu retire:%llu\n",
                (unsigned long long)idx, (unsigned long long)e.version,
                stateStr(e.state), e.isRelease,
                bits.empty() ? "-" : bits.c_str(),
                (unsigned long long)e.allocCycle,
                (unsigned long long)e.retireCycle);
        ++idx;
    }
}

void
LSQUnit::MergeBuffer::handleDrainResp(MergeBufferEntry *entry,
                                      LSQUnit *lsq_ptr)
{
    assert(entry);
    if (!entry) {
        return;
    }

    DPRINTF(LSQUnit,
            "Drain response for merge buffer entry with "
            "block addr:%#x cycle:%lu\n",
            entry->blockAddr, lsq_ptr->cpu->curCycle());

    lsq_ptr->stats.mbResidencyCycles +=
        lsq_ptr->cpu->curCycle() - entry->allocCycle;
    lsq_ptr->stats.mbDrainLatency.sample(lsq_ptr->cpu->curCycle() -
                                         entry->drainIssueCycle);
    lsq_ptr->handleMBDrain(entry);

    entries.remove_if(
        [entry](const MergeBufferEntry &e) { return &e == entry; });
    lsq_ptr->stats.mbAvgOccupancy =
        static_cast<double>(entries.size()) / numEntries;

    if (lsq_ptr->needsTSO) {
        lsq_ptr->storeInFlight = false;
    }
}

} // namespace po3
} // namespace gem5
