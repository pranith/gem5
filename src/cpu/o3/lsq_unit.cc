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

#include "cpu/o3/lsq_unit.hh"

#include <algorithm>
#include <cstring>

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
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "debug/HtmCpu.hh"
#include "debug/LSQUnit.hh"
#include "mem/htm.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/faults.hh"

namespace gem5
{

namespace o3
{

namespace
{
uint64_t
extractValue(const uint8_t *data, size_t size)
{
    uint64_t value = 0;
    if (data && size) {
        const size_t copy_size = std::min<size_t>(size, sizeof(value));
        std::memcpy(&value, data, copy_size);
    }
    return value;
}
} // anonymous namespace

LSQUnit::WritebackEvent::WritebackEvent(const DynInstPtr &_inst,
        PacketPtr _pkt, LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), pkt(_pkt), lsqPtr(lsq_ptr)
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
    delete pkt;
}

const char *
LSQUnit::WritebackEvent::description() const
{
    return "Store writeback";
}

bool
LSQUnit::recvTimingResp(PacketPtr pkt)
{
    if (auto *sq_state =
            dynamic_cast<SQEarlyPrelockSenderState *>(pkt->senderState)) {
        const Cycles ready_cycle =
            cpu->curCycle() + zfenceMbLockAcquireLatency;
        bool claimed = false;
        for (auto &entry : storeQueue) {
            if (entry.valid() && entry.instruction() &&
                entry.instruction()->seqNum == sq_state->expectedSeqNum &&
                entry.zfLineAddrValid() &&
                entry.zfLineAddr() == sq_state->expectedBlockAddr) {
                entry.earlyPrelockInFlight() = false;
                entry.earlyPrelockReadyCycle() = ready_cycle;
                claimed = true;
                break;
            }
        }
        if (!claimed) {
            claimed = mergeBuffer.completeSQEarlyPrelock(
                sq_state->expectedSeqNum, sq_state->expectedBlockAddr,
                ready_cycle);
        }
        // A committed store remains in either the SQ or MB until its early
        // lock response is associated with it.
        assert(claimed);
        delete sq_state;
        delete pkt;
        return true;
    } else if (auto *pf_state =
            dynamic_cast<MergeBufferPrefetchSenderState *>(pkt->senderState)) {
        assert(mergeBufferPfInFlight > 0);
        --mergeBufferPfInFlight;
        if (pf_state->entry &&
            pf_state->entry->blockAddr == pf_state->expectedBlockAddr &&
            pf_state->entry->allocCycle == pf_state->expectedAllocCycle) {
            pf_state->entry->zfPrefetchInFlight = false;
            if (pf_state->publicationPrefetch) {
                pf_state->entry->zfPublicationReady = true;
            }
            if (pf_state->earlyReadinessPrefetch) {
                ++stats.mbTagCompleteEarlyReadinessCompletions;
            }
            if (pf_state->conflictRecovery) {
                pf_state->entry->zfConflictRefetchPending = false;
            } else {
                pf_state->entry->zfInitialPrefetchNeeded = false;
            }
        }
        delete pf_state;
        delete pkt;
        return true;
    } else if (auto *zf_state = dynamic_cast<MergeBufferZFLineLockSenderState *>(
                   pkt->senderState)) {
        if (zf_state->release) {
            if (zf_state->entry &&
                zf_state->entry->blockAddr == zf_state->expectedBlockAddr) {
                zf_state->entry->zfLockReqInFlight = false;
                zf_state->entry->zfLockAcquired = false;
                zf_state->entry->zfPermReady = false;
                zf_state->entry->zfEligibleForRelaxedRetire = false;
                zf_state->entry->zfLeaseExpireCycle = Cycles(0);
            }
            if (zf_state->entry &&
                       zf_state->entry->blockAddr ==
                           zf_state->expectedBlockAddr) {
                auto *entry = zf_state->entry;
                entry->zfLockReqPending =
                    !entry->zfPrelockConflictRevoked &&
                    (!tsoTagCompleteStoreMerging ||
                     entry->tagCompleteCandidate != 0 ||
                     entry->tagCompleteGroup != 0);
            }
            delete zf_state;
            delete pkt;
            return true;
        }
        if (zf_state->entry &&
            zf_state->entry->blockAddr == zf_state->expectedBlockAddr) {
            auto *entry = zf_state->entry;
            entry->zfLockReqPending = false;
            if (entry->zfLockReqInFlight) {
                entry->zfLockReqInFlight = false;
            }
            if (!entry->zfLockAcquired &&
                entry->zfLockReadyCycle == Cycles(0)) {
                entry->zfLockReadyCycle =
                    cpu->curCycle() + zfenceMbLockAcquireLatency;
                if (zfenceMbLockAcquireLatency == Cycles(0)) {
                    if (!entry->zfPermReady) {
                        entry->zfPermReady = true;
                        ++stats.numPermReadySet;
                    }
                    entry->zfLockAcquired = true;
                    entry->zfPublicationReady = true;
                    entry->zfLeaseExpireCycle =
                        cpu->curCycle() + tsoTagCompleteLockLease;
                    entry->zfEligibleForRelaxedRetire =
                        !entry->zfPrelockConflictRevoked &&
                        zfenceRelaxRetire && entry->zfPermReady &&
                        entry->zfLineAddrValid;
                    entry->zfLockReqInFlight = false;
                }
            }
        }
        delete zf_state;
        delete pkt;
        return true;
    } else if (auto *mb_state = dynamic_cast<MergeBufferDrainSenderState *>(
                   pkt->senderState)) {
        if (mb_state->publicationId != 0) {
            mergeBuffer.handleAtomicPublicationResp(
                pkt->req->isZFPublicationComplete());
            delete mb_state;
            delete pkt;
            return true;
        }
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

    if (entry->isRelease) {
        DPRINTF(LSQUnit,
                "MB drain handling release entry block addr:%#x ver:%llu "
                "sn:%lli\n",
                entry->blockAddr, entry->version, entry->seqNum);

        const auto marked = markAcquireLoadsHitExternalSnoopAfter(0);
        if (marked) {
            DPRINTF(LSQUnit,
                    "Marked %u acquire loads for replay after release "
                    "MB drain [sn:%lli]\n",
                    marked, entry->seqNum);
        }
    }

    handleMBDrain(entry->blockAddr);
}

void
LSQUnit::handleMBDrain(Addr block_addr)
{
    if (isStalled() && (block_addr & cacheBlockMask) == stallingMBAddr) {
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
LSQUnit::mergeBufferPrefetch(MergeBuffer::MergeBufferEntry *entry)
{
    if (!entry || !entry->baseReq || entry->zfPrefetchInFlight) {
        return;
    }

    const bool conflict_recovery = entry->zfConflictRefetchPending;
    if (!conflict_recovery && !entry->zfInitialPrefetchNeeded) {
        return;
    }

    const size_t line_size = cacheLineSize();
    const Addr line_addr = entry->blockAddr & cacheBlockMask;

    RequestPtr base = entry->baseReq;
    const bool early_readiness_prefetch =
        tsoTagCompleteStoreMerging &&
        tsoTagCompleteEarlyReadinessPrefetch &&
        entry->tagCompleteCandidate == 0 &&
        entry->tagCompleteGroup == 0 && !conflict_recovery;
    const bool publication_prefetch = early_readiness_prefetch ||
        entry->tagCompleteCandidate != 0 ||
        entry->tagCompleteGroup != 0;
    Request::Flags flags = base->getFlags() | Request::PREFETCH |
                           Request::MB_PREFETCH_COMPLETION;
    if (publication_prefetch) {
        flags.set(Request::PF_EXCLUSIVE);
    }
    RequestorID rid = base->requestorId();
    RequestPtr pf_req =
        std::make_shared<Request>(line_addr, line_size, flags, rid);
    if (base->hasContextId()) {
        pf_req->setContext(base->contextId());
    }
    if (base->hasPC()) {
        pf_req->setPC(base->getPC());
    }
    pf_req->taskId(base->taskId());

    PacketPtr pf_pkt = Packet::createRead(pf_req);
    // This MB-tagged SoftPFReq is acknowledged only after its matching MSHR
    // completes; see the two tagged-prefetch exceptions in Cache.
    pf_pkt->allocate();
    pf_pkt->senderState = new MergeBufferPrefetchSenderState(
        this, entry, entry->blockAddr, entry->allocCycle,
        conflict_recovery, publication_prefetch,
        early_readiness_prefetch);

    // Merge-buffer prefetches are opportunistic. If the cache port is
    // backpressured, drop them instead of marking the LSQ cache-blocked and
    // stalling real store / MB drain traffic behind a best-effort request.
    if (!lsq->cacheBlocked() && lsq->cachePortAvailable(false) &&
        dcachePort->sendTimingReq(pf_pkt)) {
        lsq->cachePortBusy(false);
        entry->zfPrefetchInFlight = true;
        ++mergeBufferPfInFlight;
        if (early_readiness_prefetch) {
            ++stats.mbTagCompleteEarlyReadinessPrefetches;
        }
    } else {
        delete static_cast<MergeBufferPrefetchSenderState *>(
            pf_pkt->senderState);
        delete pf_pkt;
    }
}

void
LSQUnit::prepareSQEarlyPrelock(SQEntry &entry)
{
    if (!mergeBufferEnabled || !tsoTagCompleteStoreMerging || !needsTSO ||
        !cpu->versioningEnabled() || !zfenceEnable || !zfenceLockLines ||
        !entry.valid() || !entry.hasRequest()) {
        return;
    }

    LSQRequest *request = entry.request();
    RequestPtr req = request->mainReq();
    if (!req || request->isSplit() || !req->hasPaddr() ||
        req->isLocalAccess() || req->isLLSC() || req->isAtomic() ||
        req->isCacheMaintenance() || req->isRelease() ||
        entry.instruction()->staticInst->isRelease()) {
        return;
    }

    const Addr line_addr = req->getPaddr() & cacheBlockMask;
    if (mergeBuffer.hasEntryForLine(line_addr)) {
        return;
    }

    // Avoid taking a second speculative reference for a line already owned
    // by an older store which has not reached the merge buffer yet.
    for (const auto &older : storeQueue) {
        if (!older.valid() || !older.instruction() ||
            older.instruction()->seqNum >= entry.instruction()->seqNum) {
            continue;
        }
        if (!older.hasRequest()) {
            continue;
        }
        auto *older_req = const_cast<LSQRequest *>(older.request());
        if (older_req &&
            older_req->isCacheBlockHit(line_addr, cacheBlockMask)) {
            return;
        }
    }

    entry.zfLineAddrValid() = true;
    entry.zfLineAddr() = line_addr;
    entry.earlyPrelockPending() = true;
    entry.earlyPrelockConflict() = false;
}

void
LSQUnit::trySendSQEarlyPrelock(SQEntry &entry)
{
    if (!entry.valid() || !entry.hasRequest() ||
        !entry.earlyPrelockPending() || entry.earlyPrelockInFlight() ||
        entry.lockAcquired() || entry.earlyPrelockReadyCycle() != Cycles(0)) {
        return;
    }

    RequestPtr base = entry.request()->mainReq();
    if (!base || !entry.zfLineAddrValid()) {
        entry.earlyPrelockPending() = false;
        return;
    }

    Request::Flags flags = base->getFlags();
    RequestPtr lock_req = std::make_shared<Request>(
        entry.zfLineAddr(), cacheLineSize(), flags, base->requestorId());
    lock_req->setFlags(Request::ZFENCE_LOCK_LINE);
    if (base->hasContextId()) {
        lock_req->setContext(base->contextId());
    }
    if (base->hasPC()) {
        lock_req->setPC(base->getPC());
    }
    lock_req->taskId(base->taskId());
    lock_req->setByteEnable(std::vector<bool>(cacheLineSize(), false));

    PacketPtr pkt = Packet::createWrite(lock_req);
    auto *buf = new uint8_t[cacheLineSize()];
    std::memset(buf, 0, cacheLineSize());
    pkt->dataDynamic(buf);
    pkt->senderState = new SQEarlyPrelockSenderState(
        this, entry.instruction()->seqNum, entry.zfLineAddr());

    if (!lsq->cacheBlocked() && lsq->cachePortAvailable(false) &&
        dcachePort->sendTimingReq(pkt)) {
        lsq->cachePortBusy(false);
        entry.earlyPrelockPending() = false;
        entry.earlyPrelockInFlight() = true;
    } else {
        delete pkt->senderState;
        delete pkt;
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
        const HtmCacheFailure htm_rc =
            pkt->getHtmTransactionFailedInCacheRC();
        if (pkt->isWrite()) {
            DPRINTF(HtmCpu,
                "store notification (ignored) of HTM transaction failure "
                "in cache - addr=0x%lx - rc=%s - htmUid=%d\n",
                pkt->getAddr(), htmFailureToStr(htm_rc),
                pkt->getHtmTransactionUid());
        } else {
            HtmFailureFaultCause fail_reason =
                HtmFailureFaultCause::INVALID;

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

            inst->fault =
            std::make_shared<GenericHtmFailureFault>(
                inst->getHtmTransactionUid(),
                fail_reason);
            DPRINTF(LSQUnit,
                    "Setting fault %s for inst [sn:%lli] at addr %#x\n",
                    inst->fault->name(), inst->seqNum, pkt->getAddr());

            DPRINTF(HtmCpu,
                "load notification of HTM transaction failure "
                "in cache - pc=%s - addr=0x%lx - "
                "rc=%u - htmUid=%d\n",
                inst->pcState(), pkt->getAddr(),
                htmFailureToStr(htm_rc), pkt->getHtmTransactionUid());
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
                    pkt->getHtmTransactionFailedInCacheRC() );
            }

            if (inst->isLoad() && pkt->isRead()) {
                const uint64_t value =
                    extractValue(pkt->getConstPtr<uint8_t>(), pkt->getSize());
                DPRINTF(LSQUnit,
                        "Load value from cache [sn:%lli] PC %s addr %#x "
                        "size:%u value:%#llx\n",
                        inst->seqNum, inst->pcState(), pkt->getAddr(),
                        pkt->getSize(), (unsigned long long)value);

                // Safe load via cache ordering tag map
                auto tag = lookupCacheTag(pkt->getAddr(), pkt->getSize());
                if (tag && *tag == inst->getMemOrderVersion() &&
                    !inst->stlfForwarded()) {
                    inst->safeCacheOrdered(true);
                }
            }

            writeback(inst, request->mainPacket());
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
    : lsqID(-1), storeQueue(sqEntries), loadQueue(lqEntries),
      storesToWB(0),
      htmStarts(0), htmStops(0),
      lastRetiredHtmUid(0),
      cacheBlockMask(0), stalled(false),
      isStoreBlocked(false), storeInFlight(false), stats(nullptr)
{
}

void
LSQUnit::init(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params,
        LSQ *lsq_ptr, unsigned id)
{
    lsqID = id;

    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    cpu->addStatGroup(csprintf("lsq%i", lsqID).c_str(), &stats);

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n",lsqID);

    depCheckShift = params.LSQDepCheckShift;
    checkLoads = params.LSQCheckLoads;
    needsTSO = params.needsTSO;
    tsoConsecutiveStoreMerging = params.tsoConsecutiveStoreMerging;
    tsoTagCompleteStoreMerging = params.tsoTagCompleteStoreMerging;
    tsoTagCompleteOutOfOrderDrain =
        params.tsoTagCompleteOutOfOrderDrain;
    tsoTagCompleteEarlyReadinessPrefetch =
        params.tsoTagCompleteEarlyReadinessPrefetch;
    tsoTagCompleteEarlyMbPrelock =
        params.tsoTagCompleteEarlyMbPrelock;
    tsoTagCompleteWindow = params.tsoTagCompleteWindow;
    tsoTagCompleteRetryCycles = params.tsoTagCompleteRetryCycles;
    tsoTagCompleteSqPressureThreshold =
        params.tsoTagCompleteSqPressureThreshold;
    fatal_if(tsoTagCompleteSqPressureThreshold > 100,
             "tsoTagCompleteSqPressureThreshold must be in [0, 100]");
    mbSqPressureFreeEntryThreshold =
        params.mbSqPressureFreeEntryThreshold;
    fatal_if(mbSqPressureFreeEntryThreshold > params.mergeBufferEntries,
             "mbSqPressureFreeEntryThreshold (%u) exceeds the number of "
             "merge-buffer entries (%u)",
             mbSqPressureFreeEntryThreshold, params.mergeBufferEntries);
    mbMergingPressureThreshold =
        params.mbMergingPressureThreshold;
    fatal_if(mbMergingPressureThreshold > 100,
             "mbMergingPressureThreshold must be in [0, 100]");
    tsoTagCompleteLockLease = params.tsoTagCompleteLockLease;

    mergeBufferEnabled = params.useMergeBuffer;
    mergeBufferPrefetchEnabled = params.mergeBufferPrefetch;
    mergeBufferPfInFlight = 0;
    mbRetireWhenFullValid = params.mbRetireWhenFullValid;
    optimizeStoreRelease = params.optimizeStoreRelease;
    optimizeAcquirePC = params.optimizeAcquirePC;
    zfenceEnable = params.zfenceEnable;
    zfenceRelaxRetire = params.zfenceRelaxRetire;
    zfenceLockLines = params.zfenceLockLines;
    zfenceMbLockAcquireLatency = params.zfenceMbLockAcquireLatency;
    cacheOrderingTagMap.setCapacity(params.cacheOrderingTagEntries);

    storeDeallocateWidth = params.storeDeallocateWidth;
    storeDeallocsThisCycle = 0;
    lastStoreDeallocCycle = cpu->curCycle();

    if (mergeBufferEnabled) {
        const size_t entry_bytes = params.mergeBufferEntryBytes == 0
                                       ? cacheLineSize()
                                       : params.mergeBufferEntryBytes;
        fatal_if(entry_bytes == 0 || (entry_bytes & (entry_bytes - 1)) != 0,
                 "mergeBufferEntryBytes (%zu) must be a power of two",
                 entry_bytes);
        fatal_if(entry_bytes > cacheLineSize() ||
                     cacheLineSize() % entry_bytes != 0,
                 "mergeBufferEntryBytes (%zu) must divide and not exceed "
                 "the cache line size (%zu)",
                 entry_bytes, cacheLineSize());
        mergeBuffer.init(this, params.mergeBufferEntries, entry_bytes,
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

    stalled = false;

    cacheBlockMask = ~(cpu->cacheLineSize() - 1);

    cacheOrderingTagMap.clear();
}

void
LSQUnit::noteCacheUpdate(Addr paddr, size_t size, uint64_t version,
                         const std::vector<bool> *byte_enable)
{
    const size_t line_size = cacheLineSize();
    Addr line_addr = paddr & cacheBlockMask;
    size_t offset = paddr & (line_size - 1);
    size_t remaining = size;

    while (remaining > 0) {
        const size_t chunk = std::min(line_size - offset, remaining);
        cacheOrderingTagMap.record(line_addr, version, line_size, offset,
                                   chunk, byte_enable);
        remaining -= chunk;
        line_addr += line_size;
        offset = 0;
        if (byte_enable && byte_enable->size() > chunk) {
            byte_enable = nullptr; // only apply mask to first line chunk
        }
    }
}

void
LSQUnit::noteCacheEvict(Addr paddr)
{
    const Addr line_addr = paddr & cacheBlockMask;
    cacheOrderingTagMap.invalidate(line_addr);
}

std::optional<uint64_t>
LSQUnit::lookupCacheTag(Addr paddr, size_t size) const
{
    const Addr line_addr = paddr & cacheBlockMask;
    const size_t line_size = cpu->cacheLineSize();
    const size_t offset = paddr & (line_size - 1);
    return cacheOrderingTagMap.lookup(line_addr, offset, size);
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
      ADD_STAT(tsoLoadCompletionReschedules,
               statistics::units::Count::get(),
               "TSO loads replayed when an older load completed after an "
               "external snoop hit the younger load"),
      ADD_STAT(tsoL1EvictionHazards,
               statistics::units::Count::get(),
               "Executed TSO loads marked hazardous by an L1D replacement"),
      ADD_STAT(possibleConsistencyViolation, statistics::units::Count::get(),
               "Possible consistency violations due to version hazard without "
               "address overlap"),
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
      ADD_STAT(mbPartialFwdForceRetires, statistics::units::Count::get(),
               "MB entries force-retired after a partial MB forwarding "
               "match"),
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
      ADD_STAT(mbSameTagMerges, statistics::units::Count::get(),
               "Same-tag store fragments merged into existing MB entries"),
      ADD_STAT(mbConsecutiveTagMerges, statistics::units::Count::get(),
               "Stores merged using adjacent TSO ordering tags"),
      ADD_STAT(mbTagCompleteMerges, statistics::units::Count::get(),
               "Non-consecutive stores merged using a complete locked tag interval"),
      ADD_STAT(mbTagCompleteCandidates, statistics::units::Count::get(),
               "Structurally complete non-consecutive merge candidates"),
      ADD_STAT(mbTagCompleteFallbacks, statistics::units::Count::get(),
               "Tag-complete attempts that fell back to a normal MB entry"),
      ADD_STAT(mbTagCompleteSqPressureFallbacks,
               statistics::units::Count::get(),
               "Tag-complete candidates that did not wait for incomplete "
               "locks because SQ occupancy exceeded the configured threshold"),
      ADD_STAT(mbTagCompleteRangeGapFallbacks,
               statistics::units::Count::get(),
               "Tag-complete candidates rejected by a resident range gap"),
      ADD_STAT(mbTagCompleteSplitRangeFallbacks,
               statistics::units::Count::get(),
               "Range-gap fallbacks involving a cache-line-split store"),
      ADD_STAT(mbTagCompleteWindowFallbacks,
               statistics::units::Count::get(),
               "Tag-complete candidates exceeding the member-count bound"),
      ADD_STAT(mbTagCompleteLockTimeouts, statistics::units::Count::get(),
               "Tag-complete candidates that timed out waiting for locks"),
      ADD_STAT(mbTagCompleteOnDemandLocks, statistics::units::Count::get(),
               "Member locks started after a tag-complete recurrence"),
      ADD_STAT(mbTagCompleteEarlyReadinessPrefetches,
               statistics::units::Count::get(),
               "One-shot exclusive readiness prefetches issued when "
               "tag-complete MB entries are allocated"),
      ADD_STAT(mbTagCompleteEarlyReadinessCompletions,
               statistics::units::Count::get(),
               "Allocation-time exclusive readiness prefetches completed "
               "for live tag-complete MB entries"),
      ADD_STAT(mbTagCompleteEarlyReadinessInvalidations,
               statistics::units::Count::get(),
               "Allocation-time readiness proofs invalidated by L1 eviction"),
      ADD_STAT(mbTagCompleteEarlyMbPrelocks,
               statistics::units::Count::get(),
               "Revocable MB line prelocks issued before tag-complete "
               "candidate discovery"),
      ADD_STAT(mbTagCompleteWaitCycles, statistics::units::Count::get(),
               "Cycles stores waited for tag-complete interval locks"),
      ADD_STAT(mbTagCompleteLockWaitLatency,
               statistics::units::Cycle::get(),
               "Distribution of completed or abandoned tag-complete "
               "candidate lock waits"),
      ADD_STAT(mbTagCompleteLockWaitMax, statistics::units::Cycle::get(),
               "Maximum observed tag-complete candidate lock wait"),
      ADD_STAT(mbTagCompleteGroups, statistics::units::Count::get(),
               "Atomic publication groups formed by tag-complete merging"),
      ADD_STAT(mbTagCompleteGroupMembers, statistics::units::Count::get(),
               "Merge-buffer entries included in tag-complete groups"),
      ADD_STAT(mbTagCompleteOutOfOrderDrains,
               statistics::units::Count::get(),
               "Frozen-group writes issued before an older group member"),
      ADD_STAT(mbTagCompleteLeaderMerges,
               statistics::units::Count::get(),
               "Consecutive same-line stores absorbed by an unissued "
               "tag-complete group member"),
      ADD_STAT(mbTagCompleteActiveGroupMerges,
               statistics::units::Count::get(),
               "Consecutive stores absorbed by an unissued member after "
               "another member began group publication"),
      ADD_STAT(mbTagCompleteGroupExtensions,
               statistics::units::Count::get(),
               "Younger ready MB entries appended before publication starts"),
      ADD_STAT(mbAtomicPublicationEarlyDeallocs,
               statistics::units::Count::get(),
               "Frozen-group MB slots freed when cache accepts the write"),
      ADD_STAT(mbAtomicPublicationsCompleted,
               statistics::units::Count::get(),
               "Atomic publications completed by the cache"),
      ADD_STAT(mbAtomicPublicationLatencyCycles,
               statistics::units::Cycle::get(),
               "Total cycles from starting an atomic publication until the "
               "cache installs its final member"),
      ADD_STAT(mbAtomicPublicationAvgLatency,
               statistics::units::Rate<statistics::units::Cycle,
                                       statistics::units::Count>::get(),
               "Average cycles from starting an atomic publication until the "
               "cache installs its final member"),
      ADD_STAT(mbRetired, statistics::units::Count::get(),
               "Number of merge buffer entries retired"),
      ADD_STAT(mbDrains, statistics::units::Count::get(),
               "Number of merge buffer entries drained to cache"),
      ADD_STAT(mbUnretire, statistics::units::Count::get(),
               "Number of merge buffer entries unretired from RETIRED state"),
      ADD_STAT(mbForwards, statistics::units::Count::get(),
               "Number of loads forwarded from merge buffer"),
      ADD_STAT(mbFullStoreDeallocStalls, statistics::units::Count::get(),
               "Stores blocked from dealloc because merge buffer is full"),
      ADD_STAT(mbForceRetiresOlderVersion, statistics::units::Count::get(),
               "MB entries force retired due to lower version on same block"),
      ADD_STAT(mbSqPressureCandidateEntriesCanceled,
               statistics::units::Count::get(),
               "Unfrozen tag-complete candidate entries canceled because "
               "SQ occupancy crossed its pressure threshold"),
      ADD_STAT(mbSqPressureForceRetires,
               statistics::units::Count::get(),
               "MB entries force retired because SQ occupancy crossed its "
               "pressure threshold"),
      ADD_STAT(mbMergingPressureForceRetires,
               statistics::units::Count::get(),
               "MB entries force retired because valid MERGING occupancy "
               "crossed its pressure threshold"),
      ADD_STAT(mbAvgOccupancy, statistics::units::Ratio::get(),
               "Average merge buffer occupancy (UsedEntries/TotalEntries)"),
      ADD_STAT(mbResidencyCycles, statistics::units::Count::get(),
               "Total cycles entries reside in the merge buffer"),
      ADD_STAT(barrierSqStallCycles, statistics::units::Count::get(),
               "Cycles store WB/dealloc stalled by a barrier at the head"),
      ADD_STAT(barrierSqStallOccupancy, statistics::units::Count::get(),
               "Sum of SQ occupancy during barrier stall cycles"),
      ADD_STAT(mbReleaseWaitCycles, statistics::units::Count::get(),
               "Cycles release MB entries waited for outstanding bytes"),
      ADD_STAT(mbReleaseOlderThanLoadHits, statistics::units::Count::get(),
               "Times a load found older release MB entries"),
      ADD_STAT(mbReleaseAvgOutstanding, statistics::units::Ratio::get(),
               "Average release MB entries outstanding"),
      ADD_STAT(mbReleaseMaxOutstanding, statistics::units::Count::get(),
               "Max release MB entries outstanding"),
      ADD_STAT(barrierReschedulesLSQ, statistics::units::Count::get(),
               "Instructions rescheduled/replayed due to barrier in LSQ"),
      ADD_STAT(numPermReadySet, statistics::units::Count::get(),
               "Store queue entries marked with early permission"),
      ADD_STAT(numFenceWaitCyclesSaved, statistics::units::Count::get(),
               "Cycles saved by fence relaxed-retirement"),
      ADD_STAT(numUnsafeLoadWaitCyclesSaved, statistics::units::Count::get(),
               "Cycles saved by unsafe-load relaxed-retirement"),
      ADD_STAT(numLockConflicts, statistics::units::Count::get(),
               "Lock conflicts causing fallback in zFence flow"),
      ADD_STAT(numDeferredSnoops, statistics::units::Count::get(),
               "Deferred snoops observed for zFence-locked lines"),
      ADD_STAT(numFallbacks, statistics::units::Count::get(),
               "Fallbacks to baseline ordering behavior for zFence")
{
    loadToUse
        .init(0, 299, 10)
        .flags(statistics::nozero);
    mbTagCompleteLockWaitLatency
        .init(0, 64, 1)
        .flags(statistics::nozero);

    lqAvgOccupancy.precision(2);

    sqAvgOccupancy.precision(2);

    mbAvgOccupancy.precision(2);
    mbReleaseAvgOutstanding.precision(2);
    mbAtomicPublicationAvgLatency.precision(2);
    mbAtomicPublicationAvgLatency =
        mbAtomicPublicationLatencyCycles / mbAtomicPublicationsCompleted;
}

void
LSQUnit::setDcachePort(RequestPort *dcache_port)
{
    dcachePort = dcache_port;
}

void
LSQUnit::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.capacity(); ++i)
        assert(!loadQueue[i].valid());

    assert(storesToWB == 0);
    assert(!retryPkt);
}

void
LSQUnit::takeOverFrom()
{
    resetState();
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
        DPRINTF(HtmCpu, ">> htmStarts++ (%d) : htmStops (%d)\n",
                htmStarts, htmStops);

        const int htm_depth = htmStarts - htmStops;
        const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
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
        DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops++ (%d)\n",
                htmStarts, htmStops);

        if (htmStops==1 && htmStarts==0) {
            DPRINTF(HtmCpu,
            "htmStops==1 && htmStarts==0. "
            "This generally shouldn't happen "
            "(unless due to misspeculation)\n");
        }
    }
}

void
LSQUnit::insertStore(const DynInstPtr& store_inst)
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
LSQUnit::checkCompletedLoadSnoopHazards(
    const DynInstPtr &completed_load)
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
                "younger executed load observed a snoop [sn:%lli]\n",
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

        // Squashing the oldest hazardous younger load also removes every
        // younger instruction, so no additional recovery point is needed.
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

    const Addr evict_addr = pkt->getAddr() & cacheBlockMask;
    noteCacheEvict(evict_addr);
    mergeBuffer.invalidatePublicationReady(evict_addr);

    if (!needsTSO || loadQueue.empty()) {
        return;
    }

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
    if (pkt->req && pkt->req->isZFencePrelockConflict()) {
        const Addr conflict_addr = pkt->getAddr() & cacheBlockMask;
        unsigned conflicts = 0;
        for (auto &sq_entry : storeQueue) {
            if (!sq_entry.valid() || sq_entry.completed() ||
                !sq_entry.zfLineAddrValid() ||
                sq_entry.zfLineAddr() != conflict_addr ||
                (!sq_entry.earlyPrelockInFlight() &&
                 sq_entry.earlyPrelockReadyCycle() == Cycles(0) &&
                 !sq_entry.lockAcquired())) {
                continue;
            }
            if (!sq_entry.earlyPrelockConflict()) {
                sq_entry.earlyPrelockConflict() = true;
                ++conflicts;
            }
        }
        if (zfenceEnable && mergeBufferEnabled) {
            conflicts += mergeBuffer.invalidateZFLine(conflict_addr);
        }
        stats.numLockConflicts += conflicts;
        stats.numDeferredSnoops += conflicts;
        stats.numFallbacks += conflicts;
        return;
    }

    // Ordinary coherence snoops reaching the LSQ are invalidations.
    assert(pkt->isInvalidate());

    DPRINTF(LSQUnit, "Got snoop for address %#x\n", pkt->getAddr());

    for (int x = 0; x < cpu->numContexts(); x++) {
        gem5::ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        tc->getIsaPtr()->handleLockedSnoop(pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;
    if (zfenceEnable && zfenceRelaxRetire) {
        unsigned zf_conflicts = 0;
        for (auto &sq_entry : storeQueue) {
            if (!sq_entry.valid() || sq_entry.completed() ||
                !sq_entry.zfLineAddrValid() ||
                sq_entry.zfLineAddr() != invalidate_addr) {
                continue;
            }
            if (sq_entry.eligibleForRelaxedRetire()) {
                sq_entry.eligibleForRelaxedRetire() = false;
                sq_entry.lockAcquired() = false;
                ++zf_conflicts;
            }
        }
        if (mergeBufferEnabled) {
            zf_conflicts += mergeBuffer.invalidateZFLine(invalidate_addr);
        }
        stats.numLockConflicts += zf_conflicts;
        stats.numDeferredSnoops += zf_conflicts;
        stats.numFallbacks += zf_conflicts;
    }

    if (loadQueue.empty())
        return;

    auto iter = loadQueue.begin();

    noteCacheEvict(invalidate_addr);
    DynInstPtr ld_inst = iter->instruction();
    assert(ld_inst);
    LSQRequest *request = iter->request();

    // Check that this snoop didn't just invalidate our lock flag
    if (ld_inst->effAddrValid() && request &&
        request->isCacheBlockHit(invalidate_addr, cacheBlockMask)
        && ld_inst->memReqFlags & Request::LLSC) {
        ld_inst->tcBase()->getIsaPtr()->handleLockedSnoopHit(ld_inst.get());
    }

    bool force_squash = false;

    for (; iter != loadQueue.end(); ++iter) {
        ld_inst = iter->instruction();
        assert(ld_inst);
        request = iter->request();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered() || !request)
            continue;

        DPRINTF(LSQUnit, "-- inst [sn:%lli] to pktAddr:%#x\n",
                    ld_inst->seqNum, invalidate_addr);

        const bool address_hit =
            request->isCacheBlockHit(invalidate_addr, cacheBlockMask);
        if (force_squash || address_hit) {
            if (mergeBufferEnabled) {
                if (loadBlockedByReleaseMB(ld_inst->getMemOrderVersion())) {
                    // pending store release in merge buffer
                    // squash this load and re-execute
                    force_squash = true;
                }
                if (loadBlockedByMBVersion(ld_inst->getMemOrderVersion())) {
                    // pending older version store in merge buffer
                    // squash this load and re-execute
                    force_squash = true;
                }
            }
            if (loadBlockedByReleaseSQ(ld_inst->getMemOrderVersion(),
                                       ld_inst->seqNum)) {
                // pending store release in store queue
                // squash this load and re-execute
                force_squash = true;
            }
            if (needsTSO && !force_squash) {
                if (address_hit && ld_inst->isExecuted()) {
                    DPRINTF(LSQUnit,
                            "Recording TSO snoop hazard for addr %#x "
                            "[sn:%lli]\n",
                            pkt->getAddr(), ld_inst->seqNum);
                    ld_inst->hitExternalSnoop(true);
                }
                continue;
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
                DPRINTF(LSQUnit, "Setting fault %s for load [sn:%lli]\n",
                        ld_inst->fault->name(), ld_inst->seqNum);
                request->setStateToFault();
                ++stats.barrierReschedulesLSQ;
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC) {
                    ld_inst->tcBase()->getIsaPtr()->
                        handleLockedSnoopHit(ld_inst.get());
                }

                // If an older barrier or load checks this and it's true
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
        // the stores actually completed updating the cache by this time.
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
            setMemOrderViolatorIfOlder(ld_inst);
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

        if (!ld_inst->staticInst->isAcquire()) {
            continue;
        }

        if (ld_inst->staticInst->isAcquirePC() && optimizeAcquirePC) {
            DPRINTF(LSQUnit,
                    "Skipping acquire-PC load in scan [sn:%lli] PC:%s %s\n",
                    ld_inst->seqNum, ld_inst->pcState(),
                    ld_inst->staticInst->getName());
            continue;
        }

        DPRINTF(LSQUnit,
                "Acquire scan [sn:%lli] PC:%s exec:%d snoop:%d "
                "fault:%s squashed:%d\n",
                ld_inst->seqNum, ld_inst->pcState(), ld_inst->isExecuted(),
                ld_inst->hitExternalSnoop(),
                ld_inst->fault ? ld_inst->fault->name() : "NoFault",
                ld_inst->isSquashed());

        // Unlike in checkViolations, we can use isExecuted() here because
        // the stores actually completed updating the cache by this time.
        if (!ld_inst->isExecuted()) {
            continue;
        }

        if (!ld_inst->hitExternalSnoop()) {
            continue;
        }

        if (ld_inst->fault == NoFault) {
            DPRINTF(LSQUnit,
                    "Marking acquire load for re-exec due to external snoop "
                    "[sn:%lli] barrier [sn:%lli]\n",
                    ld_inst->seqNum, barrier_sn);
            ld_inst->fault = std::make_shared<ReExec>();
            setMemOrderViolatorIfOlder(ld_inst);
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
            setMemOrderViolatorIfOlder(ld_inst);
            if (entry.hasRequest()) {
                entry.request()->setStateToFault();
            }
            ++stats.barrierReschedulesLSQ;
            ++marked;
        }
    }

    return marked;
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
        bool possible_ordering_hazard = false &&
                                        (inst_mem_version < ld_mem_version) &&
                                        !ld_inst->stlfForwarded();

        Addr ld_eff_addr1 = ld_inst->effAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> depCheckShift;

        bool addr_overlap = (inst_eff_addr2 >= ld_eff_addr1) &&
                            (inst_eff_addr1 <= ld_eff_addr2);

        if (addr_overlap || possible_ordering_hazard) {
            if (inst->isLoad()) {
                // If this load is to the same block as an external snoop
                // invalidate that we've observed then the load needs to be
                // squashed as it could have newer data
                if (ld_inst->hitExternalSnoop()) {
                    if (!memDepViolator ||
                            ld_inst->seqNum < memDepViolator->seqNum) {
                        DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] "
                                "and [sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        memDepViolator = ld_inst;

                        ++stats.memOrderViolation;
                        if (possible_ordering_hazard && !addr_overlap) {
                            ++stats.possibleConsistencyViolation;
                        }

                        DPRINTF(LSQUnit,
                                "Setting fault M5PanicFault for inst "
                                "[sn:%lli] due to load/load violation\n",
                                inst->seqNum);
                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // If the younger load did not see an invalidation snoop yet,
                // we don't need to mark the younger load as a possible
                // violation in a weak memory model
                if (!needsTSO) {
                    ++loadIt;
                    continue;
                }
                // Otherwise, mark the load has a possible load violation and
                // if we see a snoop before it's commited, we need to squash
                ld_inst->possibleLoadViolation(true);
                DPRINTF(LSQUnit,
                        "Found possible load violation at addr:%#x"
                        " between instructions [sn:%lli] and [sn:%lli]\n",
                        inst_eff_addr1, inst->seqNum, ld_inst->seqNum);
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum)
                    break;

                DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] and "
                        "[sn:%lli] at address %#x\n",
                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                memDepViolator = ld_inst;

                ++stats.memOrderViolation;

                DPRINTF(LSQUnit,
                        "Setting fault M5PanicFault for inst "
                        "[sn:%lli] due to store violation\n",
                        inst->seqNum);
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

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli] ver:%llu\n",
            inst->pcState(), inst->seqNum, inst->getMemOrderVersion());

    assert(!inst->isSquashed());

    if (inst->isExecuted()) {
        DPRINTF(LSQUnit, "Load [sn:%lli] already executed\n", inst->seqNum);
        return NoFault;
    }

    load_fault = inst->initiateAcc();
    if (load_fault != NoFault) {
        DPRINTF(LSQUnit, "Load [sn:%lli] got fault %s from initiateAcc\n",
                inst->seqNum, load_fault->name());
    }

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

    if (inst->isTranslationDelayed() && load_fault == NoFault)
        return load_fault;

    if (load_fault != NoFault && inst->translationCompleted() &&
            inst->savedRequest->isPartialFault()
            && !inst->savedRequest->isComplete()) {
        // Translation already delivered a fault on one fragment; no packets
        // were sent. Propagate the fault immediately instead of waiting for
        // nonexistent cache responses.
        inst->setExecuted();
        inst->setCanCommit();
        inst->setCompleted();
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return load_fault;
    }

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    if (load_fault != NoFault || !inst->readPredicate()) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (!inst->readPredicate())
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n",
                inst->seqNum,
                (load_fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        // Faulted/predicated loads bypass normal writeback; mark them ready
        // so commit can retire or process the fault.
        inst->setCanCommit();
        inst->setCompleted();
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

    DPRINTF(LSQUnit, "Executing store PC %s [sn:%lli] ver:%llu\n",
            store_inst->pcState(), store_inst->seqNum,
            store_inst->getMemOrderVersion());

    assert(!store_inst->isSquashed());

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = store_inst->lqIt;

    Fault store_fault = store_inst->initiateAcc();
    if (store_fault != NoFault) {
        DPRINTF(LSQUnit, "Store [sn:%lli] got fault %s from initiateAcc\n",
                store_inst->seqNum, store_fault->name());
    }

    if (store_inst->isTranslationDelayed() &&
        store_fault == NoFault)
        return store_fault;

    if (!store_inst->readPredicate()) {
        DPRINTF(LSQUnit, "Store [sn:%lli] not executed from predication\n",
                store_inst->seqNum);
        store_inst->forwardOldRegs();
        return store_fault;
    }

    if (storeQueue[store_idx].size() == 0) {
        DPRINTF(LSQUnit,"Fault on Store PC %s, [sn:%lli], Size = 0\n",
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
    if (!inst->isInstPrefetch() && !inst->isDataPrefetch()
            && inst->firstIssue != -1
            && inst->lastWakeDependents != -1) {
        stats.loadToUse.sample(cpu->ticksToCycles(
                    inst->lastWakeDependents - inst->firstIssue));
    }

    loadQueue.front().clear();
    loadQueue.pop_front();

    stats.lqAvgOccupancy = queueOccupancy(loadQueue);
}

void
LSQUnit::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loadQueue.size() == 0 || loadQueue.front().valid());

    while (loadQueue.size() != 0 && loadQueue.front().instruction()->seqNum
            <= youngest_inst) {
        commitLoad();
    }
}

void
LSQUnit::commitStores(InstSeqNum &youngest_inst)
{
    assert(storeQueue.size() == 0 || storeQueue.front().valid());

    /* Forward iterate the store queue (age order). */
    for (auto& x : storeQueue) {
        assert(x.valid());
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!x.canWB()) {
            if (x.instruction()->seqNum > youngest_inst) {
                break;
            }
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    x.instruction()->pcState(),
                    x.instruction()->seqNum);

            x.canWB() = true;

            // Start tag-complete permission acquisition while the committed
            // store is still in the SQ.  This does not change MB version
            // retirement: the lock is merely carried into a subsequently
            // allocated MB entry if it is already acquired or in flight.
            prepareSQEarlyPrelock(x);
            trySendSQEarlyPrelock(x);

            ++storesToWB;
        }
    }
}

void
LSQUnit::writebackBlockedStore()
{
    assert(isStoreBlocked);
    storeWBIt->request()->sendPacketToCache();
    if (storeWBIt->request()->isSent()){
        storePostSend();
    }
}

void
LSQUnit::writebackStores()
{
    Cycles now = cpu->curCycle();
    DPRINTF(LSQUnit, "Writing back stores in cycle:%lli\n", now);

    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Writing back blocked store\n");
        writebackBlockedStore();
    }

    if (mergeBufferEnabled) {
        if (!needsTSO || !storeInFlight) {
            if (lsq->cachePortAvailable(false)) {
                mergeBuffer.drainOne(this);
            } else {
                DPRINTF(LSQUnit, "Unable to drain merge buffer "
                                 "since the cache is blocked.\n");
            }
        }
    }

    // A partial-forward replay normally wakes when its blocking SQ or MB
    // entry drains.  Conflict recovery and force-retire paths can remove the
    // blocker through a different teardown path, however.  Repair that
    // missed notification here: once the recorded blocker no longer exists,
    // retaining the global LSQ stall can only deadlock the oldest load.
    if (isStalled() && stallingLoadIdx >= 0 &&
        loadQueue[stallingLoadIdx].valid()) {
        bool blocker_present = false;
        if (stallingStoreIsn != 0) {
            const InstSeqNum stalled_load_seq =
                loadQueue[stallingLoadIdx].instruction()->seqNum;
            for (const auto &store_entry : storeQueue) {
                if (store_entry.valid() && store_entry.instruction() &&
                    store_entry.instruction()->seqNum == stallingStoreIsn &&
                    store_entry.instruction()->seqNum < stalled_load_seq &&
                    store_entry.canWB() && !store_entry.completed()) {
                    blocker_present = true;
                    break;
                }
            }
        } else {
            blocker_present =
                mergeBuffer.hasEntryForLine(stallingMBAddr);
        }
        if (!blocker_present) {
            DynInstPtr stalled_load =
                loadQueue[stallingLoadIdx].instruction();
            DPRINTF(LSQUnit,
                    "Replaying orphaned stalled load [sn:%lli] idx:%li "
                    "former %s blocker:%#x\n",
                    stalled_load->seqNum, stallingLoadIdx,
                    stallingStoreIsn == 0 ? "MB" : "SQ",
                    stallingStoreIsn == 0 ? stallingMBAddr
                                         : (Addr)stallingStoreIsn);
            stalled = false;
            stallingStoreIsn = 0;
            stallingMBAddr = 0;
            iewStage->replayMemInst(stalled_load);
        }
    }

    // Track store queue deallocations per cycle for head removals.
    if (lastStoreDeallocCycle != cpu->curCycle()) {
        storeDeallocsThisCycle = 0;
        lastStoreDeallocCycle = cpu->curCycle();
    }

    bool forcedMBRetire = false;

    while (storesToWB > 0 && storeWBIt.dereferenceable() &&
           storeWBIt->valid() && storeWBIt->canWB()) {

        DPRINTF(LSQUnit, "Trying to drain store at idx:%i PC:%s [sn:%lu] "
                "ver:%llu\n",
                storeWBIt.idx(), storeWBIt->instruction()->pcState(),
                storeWBIt->instruction()->seqNum,
                storeWBIt->instruction()->getMemOrderVersion());

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

        if (storeWBIt->earlyPrelockReadyCycle() != Cycles(0) &&
            now >= storeWBIt->earlyPrelockReadyCycle()) {
            storeWBIt->earlyPrelockReadyCycle() = Cycles(0);
            storeWBIt->lockAcquired() = true;
            storeWBIt->permReady() = true;
            storeWBIt->eligibleForRelaxedRetire() = false;
            ++stats.numPermReadySet;
        }
        trySendSQEarlyPrelock(*storeWBIt);
        bool can_use_mb = mergeBufferEnabled &&
                          !request->mainReq()->isLocalAccess() &&
                          !request->mainReq()->isLLSC() && !is_atomic_req;

        if (!can_use_mb && isStoreBlocked) {
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
            ++stats.barrierSqStallCycles;
            stats.barrierSqStallOccupancy += storeQueue.size();
            break;
        }

        assert(!storeWBIt->committed());

        if (can_use_mb) {

            bool merged_ok = true;
            MergeBuffer::MergeBufferEntry *mb_entry = nullptr;
            MergeBuffer::MergeBufferEntry *mb_entry2 = nullptr;
            uint64_t store_version = inst->getMemOrderVersion();

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

                bool mb_full = false;
                bool can_merge_both =
		  mergeBuffer.canAcceptSplitStore(request, store_version, mb_full);

                if (can_merge_both) {
                    mb_entry = mergeBuffer.addStore(
                        now, req0->getPaddr(),
                        reinterpret_cast<uint8_t *>(storeWBIt->data()), size0,
                        storeWBIt, storeWBIt->isAllZeros(), store_version,
                        false);

                    mb_entry2 = mergeBuffer.addStore(
                        now, req1->getPaddr(),
                        reinterpret_cast<uint8_t *>(storeWBIt->data()) + size0,
                        size1, storeWBIt, storeWBIt->isAllZeros(),
                        store_version, false);

                    if (!mb_entry || !mb_entry2) {
                        panic("Only one part of a split store merged!");
                        merged_ok = false;
                    } else {
                        // If either fragment later drains independently, the
                        // survivor no longer proves that this entire tag is
                        // unpublished. Exclude both from atomic intervals.
                        mb_entry->tagCompleteRangeEligible = false;
                        mb_entry2->tagCompleteRangeEligible = false;
                    }
                } else {
                    if (mb_full) {
                        stats.mbFullStoreDeallocStalls++;
                    }
                    merged_ok = false;
                }
            } else {
                mb_entry = mergeBuffer.addStore(
                    now, request->mainReq()->getPaddr(),
                    (uint8_t *)storeWBIt->data(), request->_size, storeWBIt,
                    storeWBIt->isAllZeros(), store_version);
            }

            DPRINTF(LSQUnit,
                    "Merge for store idx:%i PC:%s "
                    "to Addr:%#x, data:%#x [sn:%lli] %s\n",
                    storeWBIt.idx(), inst->pcState(),
                    request->mainReq()->getPaddr(), (int)*(storeWBIt->data()),
                    inst->seqNum,
                    (mb_entry && merged_ok) ? "accepted" : "blocked");

            if (mb_entry && merged_ok) {
                DPRINTF(LSQUnit,
                        "MB merge store idx:%i sn:%lli addr:%#x "
                        "req_release:%d entry_release_before:%d\n",
                        storeWBIt.idx(), inst->seqNum,
                        request->mainReq()->getPaddr(),
                        request->mainReq()->isRelease(), mb_entry->isRelease);
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
                    DPRINTF(LSQUnit,
                            "Store-release idx:%i tracked via MB entry ver:%llu\n",
                            storeWBIt.idx(), store_version);
                    DPRINTF(LSQUnit,
                            "MB merge store idx:%i sn:%lli addr:%#x "
                            "entry_release_after:%d\n",
                            storeWBIt.idx(), inst->seqNum,
                            request->mainReq()->getPaddr(),
                            mb_entry->isRelease);
                }
                // Should never merge the same store twice.
                assert(!storeWBIt->completed());
                // Complete and remove this store from the SQ;
                // merge buffer owns the data from here on.
                completeStore(storeWBIt);
                ++storeDeallocsThisCycle;
                if (!storeQueue.empty())
                    storeWBIt++;
                else
                    storeWBIt = storeQueue.end();
            } else {
                // If a barrier/release store is stalled, force retire MB
                // entries once to unblock serialization.
                if (mergeBuffer.isFull()) {
                    stats.mbFullStoreDeallocStalls++;
                }
                if (!forcedMBRetire &&
                    (inst->isWriteBarrier() || inst->isSerializeBefore() ||
                     inst->isSerializeAfter() ||
                     request->mainReq()->isRelease())) {
                    mergeBuffer.forceRetireVersionsBefore(
                        inst->getMemOrderVersion());
                    forcedMBRetire = true;
                }
                // Unable to merge, stop trying
                break;
            }
        } else if (((!needsTSO) || (!storeInFlight)) &&
                   lsq->cachePortAvailable(false)) {

            storeWBIt->committed() = true;

            assert(!inst->memData);
            inst->memData = new uint8_t[request->_size];

            if (storeWBIt->isAllZeros()) {
                memset(inst->memData, 0, request->_size);
            } else {
                memcpy(inst->memData, storeWBIt->data(), request->_size);
            }

            request->buildPackets();
            if (zfenceEnable && zfenceLockLines &&
                !request->mainReq()->isLLSC() &&
                !request->mainReq()->isAtomic() &&
                !request->mainReq()->isLocalAccess() &&
                (storeWBIt->lockAcquired() ||
                 storeWBIt->earlyPrelockInFlight() ||
                 storeWBIt->earlyPrelockReadyCycle() != Cycles(0))) {
                markRequestZFLineLock(request);
            }

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
            // Cache not available (or TSO store in-flight); stop trying this
            // entry this cycle to avoid spinning on the same store.
            DPRINTF(LSQUnit,
                    "Unable to write back store idx:%i PC:%s "
                    "(cache not available or store in-flight)\n",
                    storeWBIt.idx(), storeWBIt->instruction()->pcState());
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
LSQUnit::forceMBDrain(uint64_t version, InstSeqNum seq_num)
{
    if (cpu->versioningEnabled()) {
        // Load ordering tags are fence-scoped and deliberately do not encode
        // exact store age. Recovery must therefore select committed stores by
        // program sequence number, not by comparing the load tag with store
        // tags.
        mergeBuffer.forceRetireStoresBefore(seq_num);
    } else {
        mergeBuffer.forceRetireVersionsBefore(~uint64_t(0));
    }
}

void
LSQUnit::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit, "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n", squashed_num, loadQueue.size(),
            storeQueue.size());

    while (loadQueue.size() != 0 &&
            loadQueue.back().instruction()->seqNum > squashed_num) {
        DPRINTF(LSQUnit,"Load Instruction PC %s squashed, "
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
        if (loadQueue.back().instruction()->isHtmStart())
        {
            htmStarts = (--htmStarts < 0) ? 0 : htmStarts;
            DPRINTF(HtmCpu, ">> htmStarts-- (%d) : htmStops (%d)\n",
              htmStarts, htmStops);
        }
        if (loadQueue.back().instruction()->isHtmStop())
        {
            htmStops = (--htmStops < 0) ? 0 : htmStops;
            DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops-- (%d)\n",
              htmStarts, htmStops);
        }
        if (loadQueue.back().hasRequest()) {
            sendLockedRMWAbort(loadQueue.back().request());
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
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    if (htm_cpt) {
        const uint64_t old_local_htm_uid = htm_cpt->getHtmUid();
        uint64_t new_local_htm_uid;
        if (in_flight_uid > 0)
            new_local_htm_uid = in_flight_uid;
        else
            new_local_htm_uid = lastRetiredHtmUid;

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

        DPRINTF(LSQUnit,"Store Instruction PC %s squashed, "
                "idx:%i [sn:%lli]\n",
                storeQueue.back().instruction()->pcState(),
                storeQueue.tail(), storeQueue.back().instruction()->seqNum);

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
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    return htm_cpt->getHtmUid();
}

void
LSQUnit::storePostSend()
{
    if (isStalled() &&
        storeWBIt->instruction()->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
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

    if (zfenceEnable && zfenceLockLines && storeWBIt->hasRequest()) {
        RequestPtr req = storeWBIt->request()->mainReq();
        if (req && req->isZFenceLockLine()) {
            storeWBIt->lockAcquired() = true;
            storeWBIt->eligibleForRelaxedRetire() =
                zfenceRelaxRetire && storeWBIt->permReady() &&
                storeWBIt->zfLineAddrValid();
        } else {
            storeWBIt->lockAcquired() = false;
            storeWBIt->eligibleForRelaxedRetire() = false;
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

    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert (!inst->isStore() || inst->isStoreConditional());
        ++stats.ignoredResponses;
        return;
    }

    bool completed_load = false;
    if (!inst->isExecuted()) {
        inst->setExecuted();

        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
            completed_load = inst->isLoad();
        } else {
            DPRINTF(LSQUnit, "Writeback sees fault %s for inst [sn:%lli]\n",
                    inst->fault->name(), inst->seqNum);
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec or - in case of a SplitRequest - a partial
            // translation fault

            // Unless it's a hardware transactional memory fault
            auto htm_fault = std::dynamic_pointer_cast<
                GenericHtmFailureFault>(inst->fault);

            if (!htm_fault) {
                assert(dynamic_cast<ReExec*>(inst->fault.get()) != nullptr ||
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

            DPRINTF(LSQUnit, "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n", inst->seqNum);
        }

    }

    // RC permits independent load/load reordering. Its explicit fence and
    // acquire/release paths consume snoop hazards separately.
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
    // completing any already-issued MB drain. In particular, clearing the
    // bit for an MB transfer can let a second TSO drain issue while the first
    // is still upgrading its cache line, creating a D-cache/I-cache snoop
    // cycle. Capture this before a head completion clears the SQ entry.
    const bool completes_direct_store = store_idx->committed();

    store_idx->completed() = true;
    store_idx->lockAcquired() = false;
    store_idx->eligibleForRelaxedRetire() = false;
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
        } while (storeQueue.front().completed() &&
                 !storeQueue.empty());

        iewStage->updateLSQNextCycle = true;
    }

    stats.sqAvgOccupancy = queueOccupancy(storeQueue);

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

    store_inst->storeTick = curTick() - store_inst->fetchTick;

    if (isStalled() &&
        store_inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
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
    if (cpu->checker &&  !store_inst->isStoreConditional()) {
        cpu->checker->verify(store_inst);
    }
}

bool
LSQUnit::trySendPacket(bool isLoad, PacketPtr data_pkt)
{
    bool ret = true;
    bool cache_got_blocked = false;

    LSQRequest *request = dynamic_cast<LSQRequest*>(data_pkt->senderState);
    bool isMergeBufferPkt = (request == nullptr);
    uint64_t store_version = 0;
    const std::vector<bool> *byte_enable = nullptr;
    if (!isLoad && request) {
        store_version = request->instruction()->getMemOrderVersion();
        auto req = data_pkt->req;
        if (req && req->isMasked()) {
            const auto &be = req->getByteEnable();
            byte_enable = &be;
        }
    }
    size_t pkt_size = data_pkt->getSize();

    if (!lsq->cacheBlocked() &&
        lsq->cachePortAvailable(isLoad)) {
        if (!dcachePort->sendTimingReq(data_pkt)) {
            ret = false;
            cache_got_blocked = true;
        }
    } else {
        ret = false;
    }

    if (ret) {
        if (!isLoad) {
            isStoreBlocked = false;
        }
        lsq->cachePortBusy(isLoad);
        if (!isMergeBufferPkt) {
            request->packetSent();
            if (!isLoad) {
                noteCacheUpdate(data_pkt->getAddr(), pkt_size, store_version,
                                byte_enable);
            }
        } else if (auto *mb_state =
                       dynamic_cast<MergeBufferDrainSenderState *>(
                           data_pkt->senderState)) {
            if (mb_state->entry) {
                noteCacheUpdate(data_pkt->getAddr(), pkt_size,
                                mb_state->entry->version,
                                &mb_state->entry->byteValids);
            }
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
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
}

bool
LSQUnit::checkStaleTranslations() const
{
    DPRINTF(LSQUnit, "Unit %p checking stale translations\n", this);
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    DPRINTF(LSQUnit, "Unit %p found no stale translations\n", this);
    return false;
}

void
LSQUnit::recvRetry()
{
    retryLockedRMWAborts();
    if (!pendingLockedRMWAbortPkts.empty()) {
        return;
    }
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
}

void
LSQUnit::sendLockedRMWAbort(LSQRequest *request)
{
    if (!request) {
        return;
    }

    // Requests with all bytes disabled do not create any memory fragments.
    // They should not be treated as LockedRMW accesses during squash.
    if (!request->isMemAccessRequired()) {
        return;
    }

    RequestPtr main_req = request->mainReq();
    if (!main_req || !main_req->isLockedRMW()) {
        return;
    }

    RequestPtr abort_req = std::make_shared<Request>(*main_req);
    PacketPtr pkt = new Packet(abort_req, MemCmd::LockedRMWReadAbortReq);
    pkt->senderState = nullptr;

    if (!trySendPacket(false, pkt)) {
        pendingLockedRMWAbortPkts.push_back(pkt);
    }
}

void
LSQUnit::retryLockedRMWAborts()
{
    while (!pendingLockedRMWAbortPkts.empty()) {
        PacketPtr pkt = pendingLockedRMWAbortPkts.front();
        if (!trySendPacket(false, pkt)) {
            return;
        }
        pendingLockedRMWAbortPkts.pop_front();
    }
}

void
LSQUnit::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("stalled:%i stallingStoreIsn:%llu stallingLoadIdx:%li "
            "stallingMBAddr:%#x storesToWB:%i storeInFlight:%i "
            "storeWBIt:%li dereferenceable:%i\n",
            stalled, stallingStoreIsn, stallingLoadIdx, stallingMBAddr,
            storesToWB, storeInFlight, storeWBIt.idx(),
            storeWBIt.dereferenceable());
    cprintf("Load queue size: %i\n", loadQueue.size());
    cprintf("Load queue: ");

    for (const auto& e: loadQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", storeQueue.size());
    cprintf("Store queue: ");

    for (const auto& e: storeQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu valid:%i canWB:%i committed:%i completed:%i] ",
                inst->pcState(), inst->seqNum, e.valid(), e.canWB(),
                e.committed(), e.completed());
    }

    cprintf("\n");
    mergeBuffer.dumpState();
}

void LSQUnit::schedule(Event& ev, Tick when) { cpu->schedule(ev, when); }

BaseMMU *LSQUnit::getMMUPtr() { return cpu->mmu; }

unsigned int
LSQUnit::cacheLineSize()
{
    return cpu->cacheLineSize();
}

Fault
LSQUnit::read(LSQRequest *request, ssize_t load_idx)
{
    LQEntry& load_entry = loadQueue[load_idx];
    const DynInstPtr& load_inst = load_entry.instruction();

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
            "Strictly ordered load [sn:%llx] PC %s\n",
            load_inst->seqNum, load_inst->pcState());
    }

    DPRINTF(LSQUnit,
            "[sn:%lli] Read called, load idx: %i, store idx: %i, "
            "storeHead: %i addr: %#x ver:%llu%s\n",
            load_inst->seqNum, load_idx - 1, load_inst->sqIt._idx,
            storeQueue.head() - 1, request->mainReq()->getPaddr(),
            load_inst->getMemOrderVersion(),
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
    assert (store_it >= storeWBIt);

    // If this split request ended translation with a partial fault, do not
    // attempt to access merge buffer or cache; propagate the recorded fault.
    if (request->isSplit() && request->isPartialFault()) {
        load_entry.setRequest(nullptr);
        request->discard();
        // Ensure the fault is visible at commit.
        load_inst->setExecuted();
        load_inst->setCompleted();
        load_inst->setCanCommit();
        return load_inst->getFault();
    }

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

                const auto& store_req = store_it->request()->mainReq();
                coverage = store_req->isMasked() ?
                    AddrRangeCoverage::PartialAddrRangeCoverage :
                    AddrRangeCoverage::FullAddrRangeCoverage;
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
                if (store_it->isAllZeros())
                    memset(load_inst->memData, 0,
                            request->mainReq()->getSize());
                else
                    memcpy(load_inst->memData,
                        store_it->data() + shift_amt,
                        request->mainReq()->getSize());

                DPRINTF(LSQUnit, "Forwarding from store idx %i to load to "
                        "addr %#x\n", store_it._idx,
                        request->mainReq()->getVaddr());
                const uint64_t fwd_value = extractValue(
                    load_inst->memData, request->mainReq()->getSize());
                DPRINTF(LSQUnit,
                        "Load value from SQ [sn:%lli] PC %s addr %#x "
                        "size:%u value:%#llx\n",
                        load_inst->seqNum, load_inst->pcState(),
                        request->mainReq()->getVaddr(),
                        request->mainReq()->getSize(),
                        (unsigned long long)fwd_value);

                PacketPtr data_pkt = new Packet(request->mainReq(),
                        MemCmd::ReadReq);
                data_pkt->dataStatic(load_inst->memData);
                load_inst->stlfForwarded(true);
                load_inst->stlfVersion(
                    store_it->instruction()->getMemOrderVersion());

                // hardware transactional memory
                // Store to load forwarding within a transaction
                // This should be okay because the store will be sent to
                // the memory subsystem and subsequently get added to the
                // write set of the transaction. The write set has a stronger
                // property than the read set, so the load doesn't necessarily
                // have to be there.
                assert(!request->mainReq()->isHTMCmd());
                if (load_inst->inHtmTransactionalState()) {
                    assert (!storeQueue[store_it._idx].completed());
                    assert (
                        storeQueue[store_it._idx].instruction()->
                          inHtmTransactionalState());
                    assert (
                        load_inst->getHtmTransactionUid() ==
                        storeQueue[store_it._idx].instruction()->
                          getHtmTransactionUid());
                    data_pkt->setHtmTransactional(
                        load_inst->getHtmTransactionUid());
                    DPRINTF(HtmCpu, "HTM LD (ST2LDF) "
                      "pc=0x%lx - vaddr=0x%lx - "
                      "paddr=0x%lx - htmUid=%u\n",
                      load_inst->pcState().instAddr(),
                      data_pkt->req->hasVaddr() ?
                        data_pkt->req->getVaddr() : 0lu,
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

                WritebackEvent *wb = new WritebackEvent(load_inst, data_pkt,
                        this);

                // We'll say this has a 1 cycle load-store forwarding latency
                // for now.
                // @todo: Need to make this a parameter.
                cpu->schedule(wb, curTick());

                // Don't need to do anything special for split loads.
                ++stats.forwLoads;

                return NoFault;
            } else if (
                    coverage == AddrRangeCoverage::PartialAddrRangeCoverage) {
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
                DPRINTF(LSQUnit, "Load-store forwarding mis-match. "
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
            auto req0 = request->req(0);
            auto req1 = request->req(1);

            // Only consider fragments that have valid paddrs; a faulted
            // fragment will be handled by the early-return above.
            const bool req0_valid = req0->hasPaddr();
            const bool req1_valid = req1->hasPaddr();

            Addr first_part_addr = req0_valid ? req0->getPaddr() : 0;
            size_t first_access_size = req0_valid ? req0->getSize() : 0;
            Addr second_part_addr = req1_valid ? req1->getPaddr() : 0;
            size_t second_access_size = req1_valid ? req1->getSize() : 0;

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
            uint64_t forward_version = 0;
            if (mergeBuffer.forwardData(request->mainReq()->getPaddr(),
                                        load_inst->memData,
                                        request->mainReq()->getSize(),
                                        forward_version)) {

                DPRINTF(LSQUnit,
                        "Forwarding from merge buffer to load to "
                        "addr %#x\n",
                        request->mainReq()->getVaddr());
                const uint64_t mb_value = extractValue(
                    load_inst->memData, request->mainReq()->getSize());
                DPRINTF(LSQUnit,
                        "Load value from MB [sn:%lli] PC %s addr %#x "
                        "size:%u value:%#llx\n",
                        load_inst->seqNum, load_inst->pcState(),
                        request->mainReq()->getVaddr(),
                        request->mainReq()->getSize(),
                        (unsigned long long)mb_value);

                PacketPtr data_pkt =
                    new Packet(request->mainReq(), MemCmd::ReadReq);
                data_pkt->dataStatic(load_inst->memData);
                load_inst->stlfForwarded(true);
                load_inst->stlfVersion(forward_version);

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
            const Addr partial_mb_line =
                request->isSplit()
                    ? stallBlockAddr
                    : request->mainReq()->getPaddr() & cacheBlockMask;
            if (!stalled ||
                (stalled &&
                 load_inst->seqNum <
                     loadQueue[stallingLoadIdx].instruction()->seqNum)) {
                stalled = true;
                stallingMBAddr = partial_mb_line;
                stallingStoreIsn = 0;
                stallingLoadIdx = load_idx;
            }

            // Register the load for rescheduling before trying to drain the
            // matching MB line. A tag-complete publication can be accepted
            // immediately and deallocate the MB entry below; its drain
            // callback must not race ahead of this reschedule and lose the
            // replay wakeup.
            iewStage->rescheduleMemInst(load_inst);
            load_inst->clearIssued();
            load_inst->effAddrValid(false);
            ++stats.rescheduledLoads;
            ++stats.mbPartialFwdRescheduledLoads;

            // A partial MB match cannot safely combine forwarded and cache
            // bytes. Retire the matching entry and every older ordering
            // version required to make it drainable, then use an available
            // cache port to start that drain immediately. drainOne() still
            // enforces version order, tag-group atomicity, and prelock
            // completion.
            stats.mbPartialFwdForceRetires +=
                mergeBuffer.forceRetireThroughLine(partial_mb_line);
            if ((!needsTSO || !storeInFlight) &&
                lsq->cachePortAvailable(false)) {
                mergeBuffer.drainOne(this);
            }

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
        *load_inst->memData = (uint64_t) 0x1ull;
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

    DPRINTF(LSQUnit, "Doing write to store idx %i, addr %#x | storeHead:%i "
            "[sn:%llu] ver:%llu\n",
            store_idx - 1, request->req()->getPaddr(), storeQueue.head() - 1,
            storeQueue[store_idx].instruction()->seqNum,
            storeQueue[store_idx].instruction()->getMemOrderVersion());

    storeQueue[store_idx].setRequest(request);
    unsigned size = request->_size;
    storeQueue[store_idx].size() = size;
    bool store_no_data =
        request->mainReq()->getFlags() & Request::STORE_NO_DATA;
    storeQueue[store_idx].isAllZeros() = store_no_data;
    assert(size <= SQEntry::DataSize || store_no_data);
    storeQueue[store_idx].eligibleForRelaxedRetire() = false;
    if (zfenceEnable) {
        auto req = request->mainReq();
        if (!request->isSplit() && req && req->hasPaddr()) {
            storeQueue[store_idx].zfLineAddr() =
                req->getPaddr() & cacheBlockMask;
            storeQueue[store_idx].zfLineAddrValid() = true;
        }
    }

    // copy data into the storeQueue only if the store request has valid data
    if (!(request->req()->getFlags() & Request::CACHE_BLOCK_ZERO) &&
        !request->req()->isCacheMaintenance() &&
        !request->req()->isAtomic())
        memcpy(storeQueue[store_idx].data(), data, size);

    // This function only writes the data to the store queue, so no fault
    // can happen here.
    return NoFault;
}

InstSeqNum
LSQUnit::getLoadHeadSeqNum()
{
    if (loadQueue.front().valid())
        return loadQueue.front().instruction()->seqNum;
    else
        return 0;
}

InstSeqNum
LSQUnit::getStoreHeadSeqNum()
{
    if (storeQueue.front().valid())
        return storeQueue.front().instruction()->seqNum;
    else
        return 0;
}

LSQEntry::~LSQEntry()
{
    if (_request != nullptr) {
        _request->freeLSQEntry();
        _request = nullptr;
    }
}

LSQUnit::MergeBuffer::MergeBufferEntry *
LSQUnit::MergeBuffer::tryTagCompleteMerge(
    Cycles now, Addr addr, uint8_t *data, size_t size,
    typename StoreQueue::iterator store_it, bool is_all_zero,
    uint64_t version, bool &wait_for_locks)
{
    wait_for_locks = false;
    if (!lsqPtr || !lsqPtr->tsoTagCompleteStoreMerging ||
        !lsqPtr->needsTSO || !lsqPtr->cpu->versioningEnabled() ||
        !lsqPtr->zfenceEnable || !lsqPtr->zfenceLockLines ||
        size == 0 || ((addr & ~(lineSize - 1)) !=
                      ((addr + size - 1) & ~(lineSize - 1))) ||
        store_it->instruction()->staticInst->isRelease()) {
        return nullptr;
    }

    const Addr block = addr & ~(lineSize - 1);
    size_t candidate = entries.size();
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx] || entries[idx].blockAddr != block ||
            entries[idx].isAtomic || entries[idx].tagCompleteGroup != 0 ||
            entries[idx].lastVersion >= version) {
            continue;
        }
        if (candidate == entries.size() ||
            entries[idx].lastVersion > entries[candidate].lastVersion) {
            candidate = idx;
        }
    }
    if (candidate == entries.size()) {
        return nullptr;
    }

    auto &leader = entries[candidate];
    if (version - leader.lastVersion <= 1 ||
        leader.state == EntryState::DRAINING) {
        return nullptr;
    }

    const InstSeqNum candidate_owner = store_it->instruction()->seqNum;
    auto record_candidate_wait =
        [this, candidate_owner, now](bool include_zero_wait) {
            auto wait_it = tagCompleteWaitStart.find(candidate_owner);
            if (wait_it == tagCompleteWaitStart.end() &&
                !include_zero_wait) {
                return;
            }
            const Cycles waited = wait_it == tagCompleteWaitStart.end() ?
                Cycles(0) : now - wait_it->second;
            const uint64_t waited_cycles =
                static_cast<uint64_t>(waited);
            lsqPtr->stats.mbTagCompleteLockWaitLatency.sample(
                waited_cycles);
            if (waited_cycles >
                lsqPtr->stats.mbTagCompleteLockWaitMax.value()) {
                lsqPtr->stats.mbTagCompleteLockWaitMax = waited_cycles;
            }
        };
    std::vector<size_t> members;
    bool interval_resident = true;
    bool split_range_present = false;
    const uint64_t interval_first = leader.firstVersion;
    const uint64_t interval_last = version - 1;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        auto &entry = entries[idx];
        if (entry.lastVersion < interval_first ||
            entry.firstVersion > interval_last) {
            continue;
        }
        const bool usable = !entry.isAtomic && !entry.isRelease &&
            entry.tagCompleteGroup == 0 &&
            entry.state != EntryState::DRAINING &&
            entry.tagCompleteRangeEligible &&
            !entry.zfPrelockConflictRevoked &&
            (entry.tagCompleteCandidate == 0 ||
             entry.tagCompleteCandidate == candidate_owner);
        if (!usable) {
            split_range_present |= !entry.tagCompleteRangeEligible;
            interval_resident = false;
            break;
        }
        members.push_back(idx);
    }

    std::sort(members.begin(), members.end(),
        [this](size_t a, size_t b) {
            if (entries[a].firstVersion != entries[b].firstVersion) {
                return entries[a].firstVersion < entries[b].firstVersion;
            }
            if (entries[a].lastVersion != entries[b].lastVersion) {
                return entries[a].lastVersion < entries[b].lastVersion;
            }
            return a < b;
        });

    // Consecutive merging compresses several adjacent store tags into one
    // resident entry. Validate the union of the represented tag ranges rather
    // than requiring a distinct versionCounts record for every original tag.
    uint64_t covered_through = interval_first;
    for (size_t idx : members) {
        const auto &entry = entries[idx];
        if (entry.lastVersion < covered_through) {
            continue;
        }
        if (entry.firstVersion > covered_through) {
            interval_resident = false;
            break;
        }
        covered_through = entry.lastVersion + 1;
        if (covered_through > interval_last) {
            break;
        }
    }
    if (covered_through <= interval_last) {
        interval_resident = false;
    }

    auto clear_candidate = [this, candidate_owner]() {
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx] ||
                entries[idx].tagCompleteCandidate != candidate_owner) {
                continue;
            }
            auto &entry = entries[idx];
            entry.tagCompleteCandidate = 0;
            // Once the candidate is abandoned, a merely pending request will
            // no longer be serviced because this entry is neither a candidate
            // nor a group member. Clear it unconditionally so it cannot become
            // a permanent ordinary-drain blocker. An already-issued or
            // acquired lock remains usable by the entry's normal write.
            entry.zfLockReqPending = false;
            entry.zfInitialPrefetchNeeded = false;
        }
    };

    // A same-line recurrence is not enough: every entry in the intervening
    // tag interval must still be resident. If baseline retirement has already
    // drained any member, allocate normally without delaying this store.
    if (!interval_resident) {
        clear_candidate();
        record_candidate_wait(false);
        tagCompleteWaitStart.erase(candidate_owner);
        ++lsqPtr->stats.mbTagCompleteFallbacks;
        ++lsqPtr->stats.mbTagCompleteRangeGapFallbacks;
        if (split_range_present) {
            ++lsqPtr->stats.mbTagCompleteSplitRangeFallbacks;
        }
        return nullptr;
    }

    // Bound protected hardware state by the number of distinct resident MB
    // entries, not by raw tag distance. A consecutively merged entry can
    // represent many tags while consuming only one lock and one data slot.
    if (members.empty() ||
        members.size() > lsqPtr->tsoTagCompleteWindow) {
        clear_candidate();
        record_candidate_wait(false);
        tagCompleteWaitStart.erase(candidate_owner);
        ++lsqPtr->stats.mbTagCompleteFallbacks;
        ++lsqPtr->stats.mbTagCompleteWindowFallbacks;
        return nullptr;
    }

    // A real same-line recurrence with a complete resident interval is now
    // an atomic-group candidate. Only at this point may lock acquisition hold
    // otherwise baseline-drainable members.
    for (size_t idx : members) {
        entries[idx].tagCompleteCandidate = candidate_owner;
        if (!entries[idx].zfPublicationReady) {
            entries[idx].zfInitialPrefetchNeeded = true;
        }
    }

    const bool first_candidate_attempt =
        tagCompleteWaitStart.find(candidate_owner) ==
        tagCompleteWaitStart.end();
    if (first_candidate_attempt) {
        ++lsqPtr->stats.mbTagCompleteCandidates;
    }

    const auto lock_complete = [this](size_t idx) {
        const auto &entry = entries[idx];
        return entry.zfPublicationReady &&
            entry.zfPermReady && entry.zfLockAcquired &&
            !entry.zfLockReqPending && !entry.zfLockReqInFlight &&
            entry.zfLockReadyCycle == Cycles(0) &&
            !entry.zfInitialPrefetchNeeded &&
            !entry.zfPrefetchInFlight;
    };
    const bool locks_already_complete = std::all_of(
        members.begin(), members.end(), lock_complete);

    // Preserve a zero-wait grouping opportunity under SQ pressure. If any
    // member lock is incomplete, however, do not hold the completing store:
    // returning with wait_for_locks false makes addStore() immediately take
    // its ordinary new-entry allocation path.
    const bool high_sq_pressure =
        lsqPtr->storeQueue.size() * 100 >
        lsqPtr->storeQueue.capacity() *
            lsqPtr->tsoTagCompleteSqPressureThreshold;
    if (high_sq_pressure && !locks_already_complete) {
        clear_candidate();
        record_candidate_wait(true);
        tagCompleteWaitStart.erase(candidate_owner);
        ++lsqPtr->stats.mbTagCompleteFallbacks;
        ++lsqPtr->stats.mbTagCompleteSqPressureFallbacks;
        DPRINTF(LSQUnit,
                "Bypassing tag-complete lock wait for store [%llu]: "
                "SQ occupancy %u/%u exceeds %u percent\n",
                candidate_owner,
                static_cast<unsigned>(lsqPtr->storeQueue.size()),
                static_cast<unsigned>(lsqPtr->storeQueue.capacity()),
                lsqPtr->tsoTagCompleteSqPressureThreshold);
        return nullptr;
    }

    // Lock ownership alone does not guarantee a resident writable L1 line.
    // First bring every member to L1 with a completion-tracked exclusive
    // prefetch. Only then may the metadata prelock complete and the candidate
    // cross the irrevocable freeze boundary.
    for (size_t idx : members) {
        auto &entry = entries[idx];
        if (!entry.zfPublicationReady) {
            entry.zfInitialPrefetchNeeded = true;
            if (!entry.zfPrefetchInFlight) {
                lsqPtr->mergeBufferPrefetch(&entry);
            }
            continue;
        }
        const bool lock_started = entry.zfLockAcquired ||
            entry.zfLockReqPending || entry.zfLockReqInFlight ||
            entry.zfLockReadyCycle != Cycles(0);
        if (!lock_started) {
            entry.zfLockReqPending = true;
            ++lsqPtr->stats.mbTagCompleteOnDemandLocks;
        }
        requestZFLineLock(entry);
    }

    std::sort(members.begin(), members.end(),
        [this](size_t a, size_t b) {
            if (entries[a].firstVersion != entries[b].firstVersion) {
                return entries[a].firstVersion < entries[b].firstVersion;
            }
            return a < b;
        });

    const bool locks_complete = std::all_of(
        members.begin(), members.end(), lock_complete);

    if (!locks_complete) {
        const bool locks_pending = std::all_of(
            members.begin(), members.end(),
            [this](size_t idx) {
                const auto &entry = entries[idx];
                return !entry.zfPrelockConflictRevoked &&
                    (entry.zfPrefetchInFlight ||
                     entry.zfInitialPrefetchNeeded ||
                     entry.zfPublicationReady ||
                     entry.zfLockAcquired || entry.zfLockReqPending ||
                     entry.zfLockReqInFlight ||
                     entry.zfLockReadyCycle != Cycles(0));
            });
        if (!locks_pending) {
            clear_candidate();
            record_candidate_wait(true);
            tagCompleteWaitStart.erase(candidate_owner);
            ++lsqPtr->stats.mbTagCompleteFallbacks;
            return nullptr;
        }
        auto [it, inserted] =
            tagCompleteWaitStart.emplace(candidate_owner, now);
        const Cycles waited = now - it->second;
        if (waited < lsqPtr->tsoTagCompleteRetryCycles) {
            wait_for_locks = true;
            ++lsqPtr->stats.mbTagCompleteWaitCycles;
        } else {
            clear_candidate();
            record_candidate_wait(true);
            tagCompleteWaitStart.erase(it);
            ++lsqPtr->stats.mbTagCompleteFallbacks;
            ++lsqPtr->stats.mbTagCompleteLockTimeouts;
        }
        return nullptr;
    }

    record_candidate_wait(true);
    tagCompleteWaitStart.erase(candidate_owner);

    const uint64_t group_id = nextAtomicPublication++;
    for (size_t idx : members) {
        auto &entry = entries[idx];
        entry.tagCompleteCandidate = 0;
        entry.tagCompleteGroup = group_id;
        entry.tagCompleteGroupSize = members.size();
        entry.tagCompleteGroupLastVersion = version;
        entry.state = EntryState::FORCE_RETIRED;
        entry.retireCycle = Cycles(0);
    }

    const size_t offset = addr & (lineSize - 1);
    updateEntry(leader, data, offset, size, is_all_zero);
    leader.lastVersion = version;
    leader.absorbedVersions.push_back(version);
    recordAllocVersion(version, false);
    ++lsqPtr->stats.mbMerges;
    ++lsqPtr->stats.mbTagCompleteMerges;
    ++lsqPtr->stats.mbTagCompleteGroups;
    lsqPtr->stats.mbTagCompleteGroupMembers += members.size();
    DPRINTF(LSQUnit,
            "Formed tag-complete group %llu for block:%#x tags:%llu-%llu "
            "members:%u\n", group_id, block, leader.version, version,
            static_cast<unsigned>(members.size()));
    return &leader;
}

LSQUnit::MergeBuffer::MergeBufferEntry *
LSQUnit::MergeBuffer::tryMergeIntoUnissuedAtomicMember(
    Addr addr, uint8_t *data, size_t size,
    typename StoreQueue::iterator store_it, bool is_all_zero,
    uint64_t version)
{
    if (!lsqPtr || !lsqPtr->tsoTagCompleteStoreMerging ||
        !lsqPtr->needsTSO || !lsqPtr->cpu->versioningEnabled() ||
        !lsqPtr->zfenceEnable || !lsqPtr->zfenceLockLines ||
        size == 0 || ((addr & ~(lineSize - 1)) !=
         ((addr + size - 1) & ~(lineSize - 1))) ||
        store_it->instruction()->staticInst->isRelease()) {
        return nullptr;
    }

    // A valid group member has not emitted its own cache packet: acceptance
    // immediately invalidates that MB slot. Its private data therefore
    // remains mutable even if an earlier member of the same group has begun
    // publication. Only the group at the logical resident tag frontier may
    // absorb the immediately following global store tag.
    uint64_t youngest_version = 0;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        const auto &entry = entries[idx];
        const uint64_t logical_last = entry.tagCompleteGroup != 0 ?
            entry.tagCompleteGroupLastVersion : entry.lastVersion;
        if (logical_last > youngest_version) {
            youngest_version = logical_last;
        }
    }
    uint64_t youngest_group = 0;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        const auto &entry = entries[idx];
        const uint64_t logical_last = entry.tagCompleteGroup != 0 ?
            entry.tagCompleteGroupLastVersion : entry.lastVersion;
        if (logical_last != youngest_version) {
            continue;
        }
        const uint64_t group = entry.tagCompleteGroup;
        if (group == 0 ||
            (youngest_group != 0 && group != youngest_group)) {
            return nullptr;
        }
        youngest_group = group;
    }
    if (youngest_group == 0 || version <= youngest_version ||
        version - youngest_version != 1) {
        return nullptr;
    }

    const Addr block = addr & ~(lineSize - 1);
    size_t leader_idx = entries.size();
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx] ||
            entries[idx].tagCompleteGroup != youngest_group ||
            entries[idx].blockAddr != block) {
            continue;
        }
        if (leader_idx == entries.size() ||
            entries[idx].lastVersion > entries[leader_idx].lastVersion) {
            leader_idx = idx;
        }
    }
    if (leader_idx == entries.size()) {
        return nullptr;
    }

    auto &leader = entries[leader_idx];
    if (leader.state != EntryState::FORCE_RETIRED ||
        leader.zfPrelockConflictRevoked || !leader.zfPublicationReady ||
        !leader.zfPermReady || !leader.zfLockAcquired ||
        leader.zfLockReqPending || leader.zfLockReqInFlight ||
        leader.zfLockReadyCycle != Cycles(0) ||
        leader.zfPrefetchInFlight || leader.zfInitialPrefetchNeeded) {
        return nullptr;
    }

    const size_t offset = addr & (lineSize - 1);
    updateEntry(leader, data, offset, size, is_all_zero);
    leader.lastVersion = version;
    leader.absorbedVersions.push_back(version);
    recordAllocVersion(version, false);
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (entryValid[idx] &&
            entries[idx].tagCompleteGroup == youngest_group) {
            entries[idx].tagCompleteGroupLastVersion = version;
        }
    }
    ++lsqPtr->stats.mbMerges;
    ++lsqPtr->stats.mbConsecutiveTagMerges;
    ++lsqPtr->stats.mbTagCompleteLeaderMerges;
    if (publishingAtomicPublication == youngest_group) {
        ++lsqPtr->stats.mbTagCompleteActiveGroupMerges;
    }
    DPRINTF(LSQUnit,
            "Merged consecutive store ver:%llu block:%#x into unissued "
            "tag-complete group %llu member ver:%llu-%llu\n",
            version, block, youngest_group, leader.firstVersion,
            leader.lastVersion);
    return &leader;
}

bool
LSQUnit::MergeBuffer::tryExtendAtomicPublication(
    MergeBufferEntry *entry, uint64_t version)
{
    if (!lsqPtr || !entry || entry->tagCompleteGroup != 0 ||
        entry->tagCompleteCandidate != 0 || entry->isAtomic ||
        entry->isRelease || !entry->tagCompleteRangeEligible ||
        !entry->zfLockAcquired || !entry->zfPermReady ||
        entry->zfPrelockConflictRevoked ||
        publishingAtomicPublication != 0 ||
        outstandingAtomicPublications != 0) {
        return false;
    }

    uint64_t extend_group = 0;
    uint64_t youngest_version = 0;
    uint32_t old_group_size = 0;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx] || entries[idx].tagCompleteGroup == 0) {
            continue;
        }
        const auto &member = entries[idx];
        if (member.lastVersion > youngest_version) {
            youngest_version = member.lastVersion;
            extend_group = member.tagCompleteGroup;
            old_group_size = member.tagCompleteGroupSize;
        }
    }

    if (extend_group == 0 || youngest_version + 1 != version ||
        old_group_size >= lsqPtr->tsoTagCompleteWindow) {
        return false;
    }

    entry->tagCompleteGroup = extend_group;
    entry->tagCompleteGroupSize = old_group_size + 1;
    entry->tagCompleteGroupLastVersion = version;
    entry->state = EntryState::FORCE_RETIRED;
    entry->retireCycle = Cycles(0);

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (entryValid[idx] &&
            entries[idx].tagCompleteGroup == extend_group) {
            entries[idx].tagCompleteGroupSize = old_group_size + 1;
            entries[idx].tagCompleteGroupLastVersion = version;
        }
    }

    ++lsqPtr->stats.mbTagCompleteGroupExtensions;
    ++lsqPtr->stats.mbTagCompleteGroupMembers;
    DPRINTF(LSQUnit,
            "Extended unpublished tag-complete group %llu with "
            "block:%#x ver:%llu members:%u\n",
            extend_group, entry->blockAddr, version,
            old_group_size + 1);
    return true;
}

LSQUnit::MergeBuffer::MergeBufferEntry *
LSQUnit::MergeBuffer::addStore(Cycles now, Addr addr, uint8_t *data,
                               size_t size,
                               typename StoreQueue::iterator store_it,
                               bool is_all_zero,
                               uint64_t version,
                               bool allow_tag_complete)
{
    // Preserve the baseline consecutive/same-version merge path.  A
    // tag-complete recurrence is considered only when this store cannot
    // already merge exactly as the baseline configuration would.
    bool baseline_merge_available = false;
    if (size != 0 &&
        ((addr & ~(lineSize - 1)) ==
         ((addr + size - 1) & ~(lineSize - 1)))) {
        const Addr block = addr & ~(lineSize - 1);
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx] || entries[idx].blockAddr != block ||
                entries[idx].isAtomic) {
                continue;
            }
            const auto &entry = entries[idx];
            const bool consecutive =
                lsqPtr && lsqPtr->needsTSO &&
                lsqPtr->tsoConsecutiveStoreMerging &&
                version > entry.lastVersion &&
                version - entry.lastVersion == 1;
            const bool mergeable_state =
                entry.state == EntryState::MERGING ||
                (entry.state == EntryState::RETIRED &&
                 entry.unretireCount < maxUnretire);
            const bool allocation_order_ok =
                !lsqPtr || !lsqPtr->needsTSO ||
                lastAllocatedIdx == numEntries ||
                idx == lastAllocatedIdx;
            if ((entry.lastVersion == version || consecutive) &&
                mergeable_state && allocation_order_ok) {
                baseline_merge_available = true;
                break;
            }
        }
    }

    bool wait_for_tag_locks = false;
    if (allow_tag_complete && !baseline_merge_available) {
        if (auto *entry = tryMergeIntoUnissuedAtomicMember(
                addr, data, size, store_it, is_all_zero, version)) {
            return entry;
        }
        if (auto *entry = tryTagCompleteMerge(
                now, addr, data, size, store_it, is_all_zero, version,
                wait_for_tag_locks)) {
            return entry;
        }
    }
    if (wait_for_tag_locks) {
        return nullptr;
    }
    if (tagCompleteWaitStart.erase(store_it->instruction()->seqNum) != 0) {
        ++lsqPtr->stats.mbTagCompleteFallbacks;
    }

    // Preflight every sub-entry touched by this store. This keeps allocation
    // transactional when a store crosses a sub-line merge-buffer boundary.
    std::vector<Addr> store_blocks;
    for (Addr current = addr; current < addr + size;) {
        const Addr block = current & ~(lineSize - 1);
        store_blocks.push_back(block);
        current = std::min<Addr>(block + lineSize, addr + size);
    }

    size_t planned_allocations = 0;
    bool planned_merge = false;
    for (const Addr block : store_blocks) {
        size_t found_idx = entries.size();
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx] || entries[idx].blockAddr != block) {
                continue;
            }
            const auto &entry = entries[idx];
            if (entry.isAtomic) {
                return nullptr;
            }
            const bool consecutive_tso_version =
                lsqPtr && lsqPtr->needsTSO &&
                lsqPtr->tsoConsecutiveStoreMerging &&
                version > entry.lastVersion &&
                version - entry.lastVersion == 1;
            if (entry.lastVersion == version || consecutive_tso_version) {
                found_idx = idx;
                break;
            }
        }

        if (found_idx == entries.size()) {
            ++planned_allocations;
            continue;
        }

        const auto &entry = entries[found_idx];
        if (lsqPtr && lsqPtr->needsTSO && lastAllocatedIdx != numEntries &&
            found_idx != lastAllocatedIdx) {
            return nullptr;
        }
        if (entry.state != EntryState::MERGING &&
            !(entry.state == EntryState::RETIRED &&
              entry.unretireCount < maxUnretire)) {
            return nullptr;
        }
        if (lsqPtr && lsqPtr->optimizeStoreRelease &&
            store_it->instruction()->staticInst->isRelease() &&
            entry.isRelease) {
            return nullptr;
        }
        planned_merge = true;
    }

    const size_t free_entries =
        std::count(entryValid.begin(), entryValid.end(), false);
    if (planned_allocations > free_entries) {
        return nullptr;
    }
    if (lsqPtr && lsqPtr->needsTSO && planned_merge &&
        planned_allocations != 0) {
        return nullptr;
    }

    Addr currAddr = addr;
    size_t remaining = size;
    MergeBufferEntry *last_entry = nullptr;

    while (remaining > 0) {
        Addr lineAddr = currAddr & ~(lineSize - 1);
        uint32_t offset = currAddr & (lineSize - 1);
        size_t chunk = std::min(lineSize - offset, remaining);

        size_t found_idx = entries.size();
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx])
                continue;

            const auto &e = entries[idx];
            if (e.blockAddr == lineAddr) {
                if (e.isAtomic) {
                    DPRINTF(LSQUnit,
                            "Blocking merge into atomic MB entry Addr:%#x "
                            "ver:%llu\n",
                            lineAddr, e.version);
                    return nullptr;
                }
                const bool consecutive_tso_version =
                    lsqPtr && lsqPtr->needsTSO &&
                    lsqPtr->tsoConsecutiveStoreMerging &&
                    version > e.lastVersion &&
                    version - e.lastVersion == 1;
                if (e.lastVersion == version ||
                    consecutive_tso_version) {
                    found_idx = idx;
                    break;
                } else {
                    assert(e.version < version);
                    auto &old_entry = entries[idx];
                    if (old_entry.state == EntryState::MERGING ||
                        old_entry.state == EntryState::RETIRED) {
                        old_entry.state = EntryState::FORCE_RETIRED;
                        old_entry.retireCycle = Cycles(0);
                        if (lsqPtr) {
                            lsqPtr->stats.mbForceRetiresOlderVersion++;
                        }
                    }
                }
            }
        }

        if (found_idx != entries.size()) {
            auto &entry = entries[found_idx];
            const bool same_tag_merge = entry.lastVersion == version;

            if (lsqPtr && lsqPtr->needsTSO) {
                if (lastAllocatedIdx != numEntries &&
                    found_idx != lastAllocatedIdx) {
                    DPRINTF(LSQUnit,
                            "Blocking merge for Addr:%#x ver:%llu; matching MB "
                            "entry is not the most recent allocation",
                            lineAddr, version);
                    return nullptr;
                }
            }

            DPRINTF(LSQUnit,
                    "Found an existing MB entry for Addr:%#x ver:%llu "
                    "retiring in %lu, now:%lu\n",
                    lineAddr, entry.version, entry.retireCycle, now);
            if (lsqPtr && lsqPtr->optimizeStoreRelease &&
                store_it->instruction()->staticInst->isRelease() &&
                entry.isRelease) {
                DPRINTF(LSQUnit,
                        "Blocking merge into existing release MB entry "
                        "Addr:%#x ver:%llu\n",
                        lineAddr, entry.version);
                return nullptr;
            }
            if (entry.state == EntryState::RETIRED &&
                entry.unretireCount < maxUnretire) {
                entry.state = EntryState::MERGING;
                entry.retireCycle = now + retireWindow;
                entry.unretireCount++;
                if (lsqPtr) {
                    lsqPtr->stats.mbUnretire++;
                }
            } else if (entry.state != EntryState::MERGING) {
                DPRINTF(LSQUnit, "MB entry for Addr:%#x ver:%llu marked %s\n",
                        lineAddr, entry.version,
                        (entry.state == EntryState::RETIRED) ? "RETIRED"
                        : (entry.state == EntryState::DRAINING)
                            ? "DRAINING"
                            : "FORCE_RETIRED");
                return nullptr;
            }

            if (entry.version != version) {
                const uint64_t old_version = entry.version;
                entry.version = version;
                entry.lastVersion = version;
                const bool is_release_store =
                    store_it->instruction()->staticInst->isRelease();
                recordMergedVersion(old_version, version, is_release_store);
                DPRINTF(LSQUnit,
                        "Advanced merged MB entry Addr:%#x from ver:%llu "
                        "to consecutive ver:%llu\n",
                        lineAddr, old_version, version);
            }

            updateEntry(entry, data + (currAddr - addr), offset, chunk,
                        is_all_zero);
            if (entry.seqNum == 0 ||
                store_it->instruction()->seqNum < entry.seqNum) {
                entry.seqNum = store_it->instruction()->seqNum;
            }
            if (lsqPtr && lsqPtr->zfenceEnable) {
                entry.zfLineAddr = lineAddr;
                entry.zfLineAddrValid = true;
                if (!lsqPtr->tsoTagCompleteStoreMerging &&
                    !entry.zfLockAcquired && !entry.zfLockReqInFlight &&
                    entry.zfLockReadyCycle == Cycles(0)) {
                    entry.zfLockReqPending = true;
                }
            }

            if (!entry.baseReq) {
                entry.baseReq = std::make_shared<Request>(
                    *(store_it->request()->mainReq()));
            }
            if (resetRetireOnMerge) {
                entry.retireCycle += resetRetireWindow;
            }
            if (lsqPtr) {
                lsqPtr->stats.mbMerges++;
                if (same_tag_merge) {
                    ++lsqPtr->stats.mbSameTagMerges;
                } else {
                    ++lsqPtr->stats.mbConsecutiveTagMerges;
                }
            }
            last_entry = &entry;
        } else {
            size_t free_idx = entryValid.size();
            for (size_t idx = 0; idx < entryValid.size(); ++idx) {
                if (!entryValid[idx]) {
                    free_idx = idx;
                    break;
                }
            }
            if (free_idx == entryValid.size()) {
                return nullptr;
            }
            MergeBufferEntry newEntry(lineSize, version);
            newEntry.blockAddr = lineAddr;
            newEntry.seqNum = store_it->instruction()->seqNum;
            newEntry.retireCycle = now + retireWindow;
            newEntry.baseReq =
                std::make_shared<Request>(*(store_it->request()->mainReq()));
            if (lsqPtr && lsqPtr->zfenceEnable) {
                newEntry.zfPermReady = false;
                newEntry.zfLineAddrValid = true;
                newEntry.zfLineAddr = lineAddr;
                const bool start_mb_prelock =
                    !lsqPtr->tsoTagCompleteStoreMerging ||
                    lsqPtr->tsoTagCompleteEarlyMbPrelock;
                newEntry.zfLockReqPending = start_mb_prelock;
                newEntry.zfInitialPrefetchNeeded =
                    start_mb_prelock ||
                    (lsqPtr->tsoTagCompleteStoreMerging &&
                     lsqPtr->tsoTagCompleteEarlyReadinessPrefetch);
                // Tag-complete mode may acquire permission early in the SQ
                // while retaining baseline MB retirement.  Inherit that
                // specific transaction. Otherwise, an allocation-time
                // exclusive readiness prefetch warms the line without
                // requesting a lock or delaying ordinary retirement.
                if (store_it->lockAcquired() &&
                    store_it->zfLineAddrValid() &&
                    store_it->zfLineAddr() == lineAddr) {
                    newEntry.zfPermReady = store_it->permReady();
                    newEntry.zfLockAcquired = true;
                    newEntry.zfPublicationReady = true;
                    newEntry.zfLockReqPending = false;
                    newEntry.zfInitialPrefetchNeeded = false;
                    newEntry.zfLeaseExpireCycle =
                        now + lsqPtr->tsoTagCompleteLockLease;
                    newEntry.zfPrelockConflictRevoked =
                        store_it->earlyPrelockConflict();
                    newEntry.zfConflictRefetchPending =
                        store_it->earlyPrelockConflict();
                } else if ((store_it->earlyPrelockInFlight() ||
                            store_it->earlyPrelockReadyCycle() != Cycles(0)) &&
                           store_it->zfLineAddrValid() &&
                           store_it->zfLineAddr() == lineAddr) {
                    // The commit-time request is already fetching permission.
                    // Transfer its ownership to the MB without stalling SQ
                    // writeback; the response is matched by sequence number.
                    newEntry.zfLockReqPending = false;
                    newEntry.zfLockReqInFlight =
                        store_it->earlyPrelockInFlight();
                    newEntry.zfLockReadyCycle =
                        store_it->earlyPrelockReadyCycle();
                    newEntry.zfInitialPrefetchNeeded = false;
                    newEntry.zfPrelockConflictRevoked =
                        store_it->earlyPrelockConflict();
                    newEntry.zfConflictRefetchPending =
                        store_it->earlyPrelockConflict();
                }
            }
            updateEntry(newEntry, data + (currAddr - addr), offset, chunk,
                        is_all_zero);

            entries[free_idx] = std::move(newEntry);
            entryValid[free_idx] = true;
            last_entry = &entries[free_idx];
            lastAllocatedIdx = free_idx;
            bool is_release_store =
                store_it->instruction()->staticInst->isRelease();
            recordAllocVersion(version, is_release_store);
            last_entry->allocCycle = now;

            if (lsqPtr) {
                lsqPtr->stats.mbAllocations++;
                const size_t valid_entries =
                    std::count(entryValid.begin(), entryValid.end(), true);
                lsqPtr->stats.mbAvgOccupancy =
                    (double)valid_entries / (double)numEntries;
            }
            // Reset unretire count on new allocations
            last_entry->unretireCount = 0;
            DPRINTF(LSQUnit,
                    "Allocating a new MB entry for Addr:%#x ver:%llu "
                    "retiring in %lu, now: %lu\n",
                    lineAddr, version, now + retireWindow, now);
            requestZFLineLock(entries[free_idx]);
        }

        currAddr += chunk;
        remaining -= chunk;
    }

    // Prefetch on merge buffer allocation to accelerate draining.
    if (lsqPtr && lsqPtr->mergeBufferPrefetchEnabled && last_entry) {
        lsqPtr->mergeBufferPrefetch(last_entry);
    }

    // drainOne() runs before SQ writeback. Therefore a group formed earlier
    // in this writeback pass has not issued any member yet. A younger
    // ordering-contiguous store that already owns its prelock may join that
    // still-unpublished group without adding cache-side buffering or
    // acquiring a post-freeze lock.
    tryExtendAtomicPublication(last_entry, version);

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

    size_t free_idx = entryValid.size();
    for (size_t idx = 0; idx < entryValid.size(); ++idx) {
        if (!entryValid[idx]) {
            free_idx = idx;
            break;
        }
    }
    if (free_idx == entryValid.size()) {
        return nullptr;
    }

    const Addr paddr = request->mainReq()->getPaddr();
    const Addr lineAddr = paddr & ~(lineSize - 1);

    MergeBufferEntry newEntry(lineSize, version);
    newEntry.blockAddr = lineAddr;
    newEntry.seqNum = store_it->instruction()->seqNum;
    newEntry.baseReq =
        std::make_shared<Request>(*(store_it->request()->mainReq()));
    newEntry.state = EntryState::RETIRED;
    newEntry.retireCycle = now;
    newEntry.allocCycle = now;
    newEntry.unretireCount = 0;
    newEntry.isAtomic = true;
    newEntry.atomicReq = store_it->request();

    entries[free_idx] = std::move(newEntry);
    entryValid[free_idx] = true;
    lastAllocatedIdx = free_idx;
    recordAllocVersion(version, false);

    if (lsqPtr) {
        lsqPtr->stats.mbAllocations++;
        const size_t valid_entries =
            std::count(entryValid.begin(), entryValid.end(), true);
        lsqPtr->stats.mbAvgOccupancy =
            (double)valid_entries / (double)numEntries;
    }

    DPRINTF(LSQUnit,
            "Allocating a new atomic MB entry for Addr:%#x ver:%llu "
            "now:%lu\n",
            lineAddr, version, now);

    return &entries[free_idx];
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
    auto appendBlocks = [this, &blocks](Addr addr, size_t size) {
        for (Addr current = addr; current < addr + size;) {
            const Addr block = current & ~(lineSize - 1);
            if (std::find(blocks.begin(), blocks.end(), block) ==
                blocks.end()) {
                blocks.push_back(block);
            }
            current = std::min<Addr>(block + lineSize, addr + size);
        }
    };
    appendBlocks(req0->getPaddr(), req0->getSize());
    appendBlocks(req1->getPaddr(), req1->getSize());

    size_t planned_allocations = 0;
    bool planned_merge = false;
    for (const Addr block : blocks) {
        size_t found_idx = entries.size();
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx] || entries[idx].blockAddr != block) {
                continue;
            }
            const auto &e = entries[idx];
            if (e.isAtomic) {
                return false;
            }
            const bool consecutive_tso_version =
                lsqPtr && lsqPtr->needsTSO &&
                lsqPtr->tsoConsecutiveStoreMerging &&
                version > e.lastVersion &&
                version - e.lastVersion == 1;
            if (e.lastVersion == version || consecutive_tso_version) {
                found_idx = idx;
                break;
            }
        }

        if (found_idx == entries.size()) {
            ++planned_allocations;
            continue;
        }

        const auto &entry = entries[found_idx];
        if (entry.state != EntryState::MERGING &&
            !(entry.state == EntryState::RETIRED &&
              entry.unretireCount < maxUnretire)) {
            return false;
        }
        if (lsqPtr && lsqPtr->needsTSO && lastAllocatedIdx != numEntries &&
            found_idx != lastAllocatedIdx) {
            return false;
        }
        if (lsqPtr && lsqPtr->optimizeStoreRelease &&
            request->mainReq()->isRelease() && entry.isRelease) {
            return false;
        }
        planned_merge = true;
    }

    const size_t free_entries =
        std::count(entryValid.begin(), entryValid.end(), false);
    if (planned_allocations > free_entries) {
        mb_full = true;
        return false;
    }

    // addStore() processes the two request fragments separately. In TSO mode,
    // an allocation changes lastAllocatedIdx, so mixing a merge and an
    // allocation would invalidate the second fragment's admission decision.
    if (lsqPtr && lsqPtr->needsTSO && planned_merge &&
        planned_allocations != 0) {
        return false;
    }

    return true;
}

bool
LSQUnit::MergeBuffer::canForward(Addr paddr, size_t size) const
{
    return forwardCoverage(paddr, size) ==
           AddrRangeCoverage::FullAddrRangeCoverage;
}

LSQUnit::AddrRangeCoverage
LSQUnit::MergeBuffer::forwardCoverage(Addr paddr, size_t size) const
{
    const bool versioned = lsqPtr && lsqPtr->cpu->versioningEnabled();
    std::optional<uint64_t> forwarded_version;
    bool any_present = false;
    bool all_present = true;

    for (size_t byte = 0; byte < size; ++byte) {
        const Addr byte_addr = paddr + byte;
        const MergeBufferEntry *youngest = nullptr;
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx] || entries[idx].isAtomic) {
                continue;
            }
            const auto &entry = entries[idx];
            if (byte_addr < entry.blockAddr ||
                byte_addr >= entry.blockAddr + lineSize) {
                continue;
            }
            const size_t offset = byte_addr - entry.blockAddr;
            if (!entry.byteValids[offset]) {
                continue;
            }
            if (!youngest || entry.lastVersion > youngest->lastVersion) {
                youngest = &entry;
            }
        }

        if (!youngest) {
            all_present = false;
            continue;
        }
        any_present = true;
        if (versioned) {
            if (!forwarded_version) {
                forwarded_version = youngest->lastVersion;
            } else if (*forwarded_version != youngest->lastVersion) {
                all_present = false;
            }
        }
    }

    if (!any_present) {
        return AddrRangeCoverage::NoAddrRangeCoverage;
    }
    return all_present ? AddrRangeCoverage::FullAddrRangeCoverage
                       : AddrRangeCoverage::PartialAddrRangeCoverage;
}

bool
LSQUnit::MergeBuffer::forwardData(Addr paddr, uint8_t *dst, size_t size,
                                  uint64_t &stlf_version) const
{
    const bool versioned = lsqPtr && lsqPtr->cpu->versioningEnabled();
    std::optional<uint64_t> forwarded_version;
    uint64_t youngest_version = 0;

    for (size_t byte = 0; byte < size; ++byte) {
        const Addr byte_addr = paddr + byte;
        const MergeBufferEntry *youngest = nullptr;
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx] || entries[idx].isAtomic) {
                continue;
            }
            const auto &entry = entries[idx];
            if (byte_addr < entry.blockAddr ||
                byte_addr >= entry.blockAddr + lineSize) {
                continue;
            }
            const size_t offset = byte_addr - entry.blockAddr;
            if (!entry.byteValids[offset]) {
                continue;
            }
            if (!youngest || entry.lastVersion > youngest->lastVersion) {
                youngest = &entry;
            }
        }

        if (!youngest) {
            return false;
        }
        if (versioned) {
            if (!forwarded_version) {
                forwarded_version = youngest->lastVersion;
            } else if (*forwarded_version != youngest->lastVersion) {
                return false;
            }
        }
        const size_t offset = byte_addr - youngest->blockAddr;
        dst[byte] = youngest->blockData[offset];
        youngest_version = std::max(youngest_version, youngest->lastVersion);
    }

    stlf_version = youngest_version;
    return true;
}

unsigned
LSQUnit::MergeBuffer::forceRetireThroughLine(Addr line_addr)
{
    line_addr &= ~(lineSize - 1);
    std::optional<uint64_t> target_version;

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx] || entries[idx].isAtomic ||
            entries[idx].blockAddr != line_addr ||
            entries[idx].state == EntryState::DRAINING) {
            continue;
        }
        target_version = std::max(
            target_version.value_or(0), entries[idx].version);
    }

    if (!target_version) {
        return 0;
    }

    unsigned retired = 0;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx] || entries[idx].version > *target_version) {
            continue;
        }
        auto &entry = entries[idx];
        if (entry.state == EntryState::MERGING ||
            entry.state == EntryState::RETIRED) {
            entry.state = EntryState::FORCE_RETIRED;
            entry.retireCycle = Cycles(0);
            ++retired;
            DPRINTF(LSQUnit,
                    "Force retiring MB entry after partial forward "
                    "block:%#x idx:%d ver:%llu target line:%#x "
                    "target ver:%llu\n",
                    entry.blockAddr, idx, entry.version, line_addr,
                    *target_version);
        }
    }

    return retired;
}

bool
LSQUnit::MergeBuffer::hasEntryForLine(Addr line_addr) const
{
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (entryValid[idx] && entries[idx].blockAddr == line_addr) {
            return true;
        }
    }

    return false;
}

bool
LSQUnit::MergeBuffer::completeSQEarlyPrelock(InstSeqNum seq_num,
                                              Addr line_addr,
                                              Cycles ready_cycle)
{
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        auto &entry = entries[idx];
        if (entry.seqNum != seq_num || entry.blockAddr != line_addr ||
            !entry.zfLockReqInFlight) {
            continue;
        }
        entry.zfLockReqInFlight = false;
        entry.zfLockReadyCycle = ready_cycle;
        return true;
    }
    return false;
}

void
LSQUnit::MergeBuffer::updateRetiredEntries(Cycles now)
{
    const size_t free_entries =
        std::count(entryValid.begin(), entryValid.end(), false);
    const bool mb_capacity_pressure = lsqPtr && free_entries <= 2;
    const bool sq_capacity_pressure =
        lsqPtr && !lsqPtr->storeQueue.empty() &&
        lsqPtr->storeQueue.size() * 100 >
            lsqPtr->storeQueue.capacity() *
                lsqPtr->tsoTagCompleteSqPressureThreshold;
    const bool sq_low_mb_free_pressure =
        sq_capacity_pressure &&
        free_entries < lsqPtr->mbSqPressureFreeEntryThreshold;
    const size_t merging_entries = std::count_if(
        entries.begin(), entries.end(),
        [this](const MergeBufferEntry &entry) {
            const size_t idx = indexOf(&entry);
            return idx < entryValid.size() && entryValid[idx] &&
                entry.state == EntryState::MERGING;
        });
    const bool merging_capacity_pressure =
        lsqPtr && merging_entries * 100 >
            entries.size() * lsqPtr->mbMergingPressureThreshold;

    // SQ pressure must not leave an unfrozen lock candidate overriding
    // baseline retirement. Cancel the candidate first; an acquired or
    // in-flight early prelock remains attached to the ordinary write and
    // therefore cannot delay its drain.
    if (sq_capacity_pressure) {
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx]) {
                continue;
            }
            auto &entry = entries[idx];
            if (entry.tagCompleteCandidate == 0 ||
                entry.tagCompleteGroup != 0) {
                continue;
            }
            const InstSeqNum owner = entry.tagCompleteCandidate;
            entry.tagCompleteCandidate = 0;
            entry.zfLockReqPending = false;
            entry.zfInitialPrefetchNeeded = false;
            ++lsqPtr->stats.mbSqPressureCandidateEntriesCanceled;
            if (tagCompleteWaitStart.erase(owner) != 0) {
                ++lsqPtr->stats.mbTagCompleteFallbacks;
                ++lsqPtr->stats.mbTagCompleteSqPressureFallbacks;
            }
        }
    }

    // Pressure retirement is common to RC, conventional TSO, and
    // tag-complete TSO. The existing low-free-entry policy remains active,
    // while SQ pressure may force MB retirement only when free MB entries
    // fall below the configured threshold. Independently, excessive valid
    // MERGING occupancy makes the same four oldest entries drain-eligible.
    // A formed atomic group is irrevocable and already force-retired by group
    // formation.
    const bool force_retire_for_pressure =
        mb_capacity_pressure || sq_low_mb_free_pressure ||
        merging_capacity_pressure;
    std::vector<size_t> pressure_victims;
    if (force_retire_for_pressure) {
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx]) {
                continue;
            }
            const auto &entry = entries[idx];
            if (entry.tagCompleteGroup == 0 &&
                (entry.state == EntryState::MERGING ||
                 entry.state == EntryState::RETIRED)) {
                pressure_victims.push_back(idx);
            }
        }
        std::sort(
            pressure_victims.begin(), pressure_victims.end(),
            [this](size_t lhs, size_t rhs) {
                const auto &a = entries[lhs];
                const auto &b = entries[rhs];
                if (a.version != b.version) {
                    return a.version < b.version;
                }
                if (a.retireCycle != b.retireCycle) {
                    return a.retireCycle < b.retireCycle;
                }
                return lhs < rhs;
            });
        if (pressure_victims.size() > 4) {
            pressure_victims.resize(4);
        }
    }

    const bool abort_unfrozen_batch = std::any_of(
        entries.begin(), entries.end(),
        [this](const MergeBufferEntry &entry) {
            const size_t idx = indexOf(&entry);
            return idx < entryValid.size() && entryValid[idx] &&
                entry.tagCompleteGroup == 0 &&
                entry.zfPrelockConflictRevoked;
        });

    if (abort_unfrozen_batch) {
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx]) {
                continue;
            }
            auto &entry = entries[idx];
            if (entry.tagCompleteGroup != 0 ||
                entry.state == EntryState::DRAINING ||
                entry.zfPrelockConflictRevoked) {
                continue;
            }
            const bool lock_transaction_started =
                entry.zfLockAcquired || entry.zfLockReqInFlight ||
                entry.zfLockReadyCycle != Cycles(0);
            entry.zfPrelockConflictRevoked = true;
            entry.zfPublicationReady = false;
            entry.zfEligibleForRelaxedRetire = false;
            entry.zfLockReqPending = false;
            if (lock_transaction_started) {
                entry.zfConflictRefetchPending = true;
            }
        }
    }

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        auto &entry = entries[idx];
        if (std::find(
                pressure_victims.begin(), pressure_victims.end(), idx) !=
            pressure_victims.end()) {
            entry.state = EntryState::FORCE_RETIRED;
            entry.retireCycle = Cycles(0);
            if (sq_low_mb_free_pressure) {
                ++lsqPtr->stats.mbSqPressureForceRetires;
            }
            if (merging_capacity_pressure) {
                ++lsqPtr->stats.mbMergingPressureForceRetires;
            }
            DPRINTF(LSQUnit,
                    "Force retiring one of four oldest MB entries under "
                    "%s pressure block:%#x idx:%d free entries:%u "
                    "SQ occupancy:%u/%u merging entries:%u/%u\n",
                    sq_low_mb_free_pressure ?
                        "SQ+low-free-MB" :
                        (merging_capacity_pressure ?
                            "merging occupancy" : "MB capacity"),
                    entry.blockAddr, idx,
                    static_cast<unsigned>(free_entries),
                    static_cast<unsigned>(lsqPtr->storeQueue.size()),
                    static_cast<unsigned>(lsqPtr->storeQueue.capacity()),
                    static_cast<unsigned>(merging_entries),
                    static_cast<unsigned>(entries.size()));
        }
        if (entry.state == EntryState::DRAINING) {
            // The issued drain consumes the entry's acquire-only prelock and
            // releases its write-lifetime lock when the response returns.
            // Lease maintenance here can race that coherence transaction:
            // an unlock followed by a fresh acquire can indefinitely defer
            // the already-issued write.  A draining entry is irrevocable and
            // needs no further speculative prelock maintenance.
            continue;
        }
        if (lsqPtr && entry.zfLockAcquired &&
            entry.tagCompleteGroup == 0 &&
            entry.zfPrelockConflictRevoked) {
            requestZFLineUnlock(entry, 0);
        } else if (lsqPtr && entry.zfLockAcquired &&
                   entry.tagCompleteGroup == 0 &&
                   entry.zfLeaseExpireCycle != Cycles(0) &&
                   now >= entry.zfLeaseExpireCycle) {
            requestZFLineUnlock(entry, 0);
        }
        if (lsqPtr && entry.zfLockReadyCycle != Cycles(0) &&
            !entry.zfLockAcquired && now >= entry.zfLockReadyCycle) {
            if (!entry.zfPermReady) {
                entry.zfPermReady = true;
                ++lsqPtr->stats.numPermReadySet;
            }
            entry.zfLockAcquired = true;
            entry.zfPublicationReady = true;
            entry.zfLeaseExpireCycle =
                now + lsqPtr->tsoTagCompleteLockLease;
            entry.zfEligibleForRelaxedRetire =
                !entry.zfPrelockConflictRevoked &&
                lsqPtr->zfenceRelaxRetire && entry.zfPermReady &&
                entry.zfLineAddrValid;
            entry.zfLockReqInFlight = false;
            entry.zfLockReadyCycle = Cycles(0);
        }
        if (entry.state == EntryState::MERGING) {

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
        // Retry the one-shot initial prefetch if its first port attempt was
        // dropped. A conflict-revoked entry may share an MSHR with the
        // observing snoop that caused revocation, so its distinct recovery
        // prefetch also waits for a real completion response.
        const bool tag_lock_candidate =
            !lsqPtr->tsoTagCompleteStoreMerging ||
            entry.tagCompleteCandidate != 0 ||
            entry.tagCompleteGroup != 0;
        const bool early_readiness_prefetch =
            lsqPtr->tsoTagCompleteStoreMerging &&
            lsqPtr->tsoTagCompleteEarlyReadinessPrefetch &&
            entry.tagCompleteGroup == 0 &&
            entry.zfInitialPrefetchNeeded;
        const bool early_mb_prelock =
            lsqPtr->tsoTagCompleteStoreMerging &&
            lsqPtr->tsoTagCompleteEarlyMbPrelock &&
            entry.tagCompleteGroup == 0;
        if (lsqPtr &&
            (((tag_lock_candidate || early_readiness_prefetch) &&
              entry.zfInitialPrefetchNeeded) ||
             entry.zfConflictRefetchPending) &&
            !entry.zfPrefetchInFlight) {
            lsqPtr->mergeBufferPrefetch(&entry);
        }
        if ((tag_lock_candidate || early_mb_prelock) &&
            entry.zfPublicationReady &&
            !entry.zfInitialPrefetchNeeded &&
            !entry.zfPrefetchInFlight) {
            requestZFLineLock(entry);
        }
    }

}

void
LSQUnit::MergeBuffer::requestZFLineLock(MergeBufferEntry &entry)
{
    const size_t idx = indexOf(&entry);
    const bool retired_for_lock =
        entry.state == EntryState::RETIRED ||
        entry.state == EntryState::FORCE_RETIRED;
    const bool matched_candidate =
        entry.tagCompleteCandidate != 0 ||
        entry.tagCompleteGroup != 0;
    const bool early_mb_prelock =
        lsqPtr && lsqPtr->tsoTagCompleteStoreMerging &&
        lsqPtr->tsoTagCompleteEarlyMbPrelock &&
        entry.tagCompleteGroup == 0;
    if (!lsqPtr || !lsqPtr->zfenceEnable || !lsqPtr->zfenceLockLines) {
        return;
    }
    const bool on_demand_tag_candidate =
        !lsqPtr->tsoTagCompleteStoreMerging ||
        entry.tagCompleteCandidate != 0 ||
        entry.tagCompleteGroup != 0 || early_mb_prelock;
    if (idx >= entries.size() || !entryValid[idx] || entry.isAtomic ||
        entry.state == EntryState::DRAINING ||
        !entry.baseReq ||
        (!retired_for_lock && !matched_candidate && !early_mb_prelock) ||
        !on_demand_tag_candidate ||
        !entry.zfLineAddrValid || entry.zfLockAcquired ||
        entry.zfLockReqInFlight || !entry.zfLockReqPending ||
        entry.zfPrefetchInFlight || entry.zfInitialPrefetchNeeded ||
        entry.zfPrelockConflictRevoked) {
        return;
    }
    if (!lsqPtr->tsoTagCompleteStoreMerging &&
        lsqPtr->cpu->versioningEnabled() && !versionCounts.empty() &&
        entry.version != versionCounts.front().version) {
        DPRINTF(LSQUnit,
                "Deferring zFence MB line-lock request block:%#x ver:%llu "
                "(head version ver:%llu)\n",
                entry.blockAddr, entry.version,
                versionCounts.front().version);
        return;
    }

    Request::Flags flags = entry.baseReq->getFlags();
    RequestorID rid = entry.baseReq->requestorId();
    RequestPtr lock_req =
        std::make_shared<Request>(entry.blockAddr, lineSize, flags, rid);
    lock_req->setFlags(Request::ZFENCE_LOCK_LINE);

    if (entry.baseReq->hasContextId()) {
        lock_req->setContext(entry.baseReq->contextId());
    }
    if (entry.baseReq->hasPC()) {
        lock_req->setPC(entry.baseReq->getPC());
    }
    lock_req->taskId(entry.baseReq->taskId());
    lock_req->setByteEnable(std::vector<bool>(lineSize, false));

    PacketPtr pkt = Packet::createWrite(lock_req);
    uint8_t *buf = new uint8_t[lineSize];
    std::memset(buf, 0, lineSize);
    pkt->dataDynamic(buf);
    pkt->senderState = new MergeBufferZFLineLockSenderState(
        &entry, lsqPtr, entry.version, entry.blockAddr);

    if (!lsqPtr->trySendPacket(false, pkt)) {
        if (auto *zf_state = dynamic_cast<MergeBufferZFLineLockSenderState *>(
                pkt->senderState)) {
            delete zf_state;
        }
        delete pkt;
        return;
    }

    entry.zfLockReqInFlight = true;
    entry.zfLockReqPending = false;
    if (early_mb_prelock && entry.tagCompleteCandidate == 0) {
        ++lsqPtr->stats.mbTagCompleteEarlyMbPrelocks;
    }
    DPRINTF(LSQUnit,
            "Issued early zFence MB line-lock request block:%#x ver:%llu\n",
            entry.blockAddr, entry.version);
}

bool
LSQUnit::MergeBuffer::requestZFLineUnlock(MergeBufferEntry &entry,
                                           uint64_t group_id)
{
    if (!lsqPtr || !entry.baseReq || !entry.zfLockAcquired ||
        entry.zfLockReqInFlight) {
        return false;
    }

    RequestPtr req = std::make_shared<Request>(
        entry.blockAddr, lineSize, entry.baseReq->getFlags(),
        entry.baseReq->requestorId());
    req->setFlags(Request::ZFENCE_LOCK_LINE | Request::ZFENCE_RETAIN_LINE);
    req->setByteEnable(std::vector<bool>(lineSize, false));
    if (entry.baseReq->hasContextId()) {
        req->setContext(entry.baseReq->contextId());
    }
    if (entry.baseReq->hasPC()) {
        req->setPC(entry.baseReq->getPC());
    }
    req->taskId(entry.baseReq->taskId());

    PacketPtr pkt = Packet::createWrite(req);
    auto *buf = new uint8_t[lineSize];
    std::memset(buf, 0, lineSize);
    pkt->dataDynamic(buf);
    pkt->senderState = new MergeBufferZFLineLockSenderState(
        &entry, lsqPtr, entry.version, entry.blockAddr, true, group_id);
    if (!lsqPtr->trySendPacket(false, pkt)) {
        delete pkt->senderState;
        delete pkt;
        return false;
    }
    entry.zfLockReqInFlight = true;
    return true;
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
{ std::memset(_data, 0, DataSize); }

void
SQEntry::set(const DynInstPtr &inst)
{ LSQEntry::set(inst); }

void
SQEntry::clear()
{
    LSQEntry::clear();
    _canWB = _completed = _committed = _isAllZeros = false;
    _permReady = false;
    _lockAcquired = false;
    _eligibleForRelaxedRetire = false;
    _zfLineAddrValid = false;
    _zfLineAddr = 0;
    _earlyPrelockPending = false;
    _earlyPrelockInFlight = false;
    _earlyPrelockReadyCycle = Cycles(0);
    _earlyPrelockConflict = false;
}

LSQUnit::LSQUnit(const LSQUnit &l) : stats(nullptr)
{ panic("LSQUnit is not copy-able"); }

void
LSQUnit::MergeBuffer::forceRetireVersionsBefore(uint64_t version)
{
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        auto &entry = entries[idx];
        if (entry.version >= version) {
            continue;
        }
        if (entry.state == EntryState::MERGING ||
            entry.state == EntryState::RETIRED) {
            entry.state = EntryState::FORCE_RETIRED;
            entry.retireCycle = Cycles(0);

            DPRINTF(LSQUnit, "Force retiring MB entry block addr:%#x idx:%d\n",
                    entry.blockAddr, idx);
        }
    }
}

void
LSQUnit::MergeBuffer::forceRetireStoresBefore(InstSeqNum seq_num)
{
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        auto &entry = entries[idx];
        if (entry.seqNum >= seq_num) {
            continue;
        }
        if (entry.state == EntryState::MERGING ||
            entry.state == EntryState::RETIRED) {
            entry.state = EntryState::FORCE_RETIRED;
            entry.retireCycle = Cycles(0);
            DPRINTF(LSQUnit,
                    "Force retiring older MB store block:%#x idx:%d "
                    "store-sn:%llu recovery-sn:%llu\n",
                    entry.blockAddr, idx, entry.seqNum, seq_num);
        }
    }
}

std::optional<uint64_t>
LSQUnit::MergeBuffer::youngestVersion() const
{
    if (versionCounts.empty()) {
        return std::nullopt;
    }
    return versionCounts.front().version;
}

std::optional<uint64_t>
LSQUnit::MergeBuffer::oldestVersion() const
{
    if (versionCounts.empty()) {
        return std::nullopt;
    }
    return versionCounts.back().version;
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
LSQUnit::loadBlockedByMBVersion(uint64_t version)
{
    if (!mergeBufferEnabled || !cpu->versioningEnabled()) {
        return false;
    }

    auto youngest = mergeBuffer.youngestVersion();
    if (!youngest) {
        return false;
    }

    if (version > *youngest) {
        return true;
    }

    return false;
}

bool
LSQUnit::hasStoreToLine(Addr line_addr) const
{
    for (const auto &entry : storeQueue) {
        if (!entry.valid() || entry.completed()) {
            continue;
        }

        if (entry.zfLineAddrValid() && entry.zfLineAddr() == line_addr) {
            return true;
        }

        auto *request = const_cast<LSQRequest *>(entry.request());
        if (entry.hasRequest() && request &&
            request->isCacheBlockHit(line_addr, cacheBlockMask)) {
            return true;
        }
    }

    return mergeBuffer.hasEntryForLine(line_addr);
}

bool
LSQUnit::hasUnprotectedStoresToWB(bool *has_protected_mb) const
{
    if (has_protected_mb) {
        *has_protected_mb = false;
    }

    if (!zfenceEnable || !zfenceRelaxRetire || !zfenceLockLines) {
        if (has_protected_mb) {
            *has_protected_mb = !mergeBuffer.isEmpty();
        }
        return !mergeBuffer.isEmpty() || (storesToWB > 0);
    }

    if (storesToWB > 0) {
        return true;
    }

    if (!mergeBufferEnabled) {
        return false;
    }

    bool has_relevant = false;
    if (!mergeBuffer.validateAllMBZFLocked(has_relevant)) {
        if (has_protected_mb) {
            *has_protected_mb = has_relevant;
        }
        return true;
    }

    if (has_protected_mb) {
        *has_protected_mb = has_relevant;
    }

    return false;
}

bool
LSQUnit::canRelaxFenceRetire(uint64_t, InstSeqNum)
{
    if (!zfenceEnable || !zfenceRelaxRetire || !zfenceLockLines) {
        return false;
    }

    bool has_relevant_mb = false;
    if (hasUnprotectedStoresToWB(&has_relevant_mb)) {
        ++stats.numFallbacks;
        return false;
    }

    if (!mergeBufferEnabled) {
        return false;
    }
    if (!has_relevant_mb) {
        ++stats.numFallbacks;
        return false;
    }

    ++stats.numFenceWaitCyclesSaved;
    return true;
}

bool
LSQUnit::canRelaxUnsafeLoadRetire(uint64_t version, InstSeqNum)
{
    if (!zfenceEnable || !zfenceRelaxRetire || !zfenceLockLines) {
        return false;
    }

    if (!mergeBufferEnabled || !cpu->versioningEnabled()) {
        return false;
    }

    bool has_relevant_mb = false;
    if (!mergeBuffer.validateLowerVersionMBZFLocked(version, has_relevant_mb)) {
        ++stats.numFallbacks;
        return false;
    }

    if (!has_relevant_mb) {
        return false;
    }

    ++stats.numUnsafeLoadWaitCyclesSaved;
    return true;
}

bool
LSQUnit::MergeBuffer::hasReleaseOlderThan(uint64_t version) const
{
    size_t release_count = 0;
    bool versioned = lsqPtr && lsqPtr->cpu->versioningEnabled();
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        const auto &entry = entries[idx];
        if (entry.isRelease) {
            release_count++;
        }
        if (entry.isRelease && (!versioned || entry.version <= version)) {
            if (lsqPtr) {
                lsqPtr->stats.mbReleaseOlderThanLoadHits++;
            }
            return true;
        }
    }

    return false;
}

unsigned
LSQUnit::MergeBuffer::invalidateZFLine(Addr line_addr)
{
    unsigned conflicts = 0;

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        auto &entry = entries[idx];
        if (!entry.zfLineAddrValid || entry.zfLineAddr != line_addr) {
            continue;
        }

        if (entry.tagCompleteGroup != 0 ||
            entry.state == EntryState::DRAINING) {
            // A frozen group or an issued ordinary drain is irrevocable. The
            // cache hierarchy defers the conflict until publication finishes.
            DPRINTF(LSQUnit,
                    "Deferring zf invalidate for irrevocable MB entry "
                    "block:%#x ver:%llu group:%llu\n",
                    entry.blockAddr, entry.version,
                    entry.tagCompleteGroup);
            continue;
        }

        const bool participating = entry.zfEligibleForRelaxedRetire ||
            entry.zfLockAcquired || entry.zfLockReqPending ||
            entry.zfLockReqInFlight ||
            entry.zfLockReadyCycle != Cycles(0);
        if (participating) {
            // Before freeze, a coherence conflict aborts this entry's
            // speculative participation. Preserve in-flight/ready state so a
            // late grant is recognized and then explicitly released.
            if (!entry.zfPrelockConflictRevoked) {
                ++conflicts;
            }
            entry.zfPrelockConflictRevoked = true;
            entry.zfPublicationReady = false;
            entry.zfEligibleForRelaxedRetire = false;
            entry.zfLockReqPending = false;
            entry.zfConflictRefetchPending = true;
            DPRINTF(LSQUnit,
                    "Revoking speculative MB prelock after conflict "
                    "block:%#x ver:%llu acquired:%u inflight:%u\n",
                    entry.blockAddr, entry.version,
                    entry.zfLockAcquired ? 1 : 0,
                    entry.zfLockReqInFlight ? 1 : 0);
        }
    }

    if (conflicts != 0) {
        // A prelock conflict aborts the whole unfrozen speculative batch.
        // Keeping younger prelocks on other lines can otherwise block the
        // loads and coherence resources needed by the older revoked entry's
        // ordinary drain, forming a cross-line cycle. Frozen tag-complete
        // groups remain irrevocable and are deliberately excluded.
        for (size_t idx = 0; idx < entries.size(); ++idx) {
            if (!entryValid[idx]) {
                continue;
            }
            auto &entry = entries[idx];
            if (entry.tagCompleteGroup != 0 ||
                entry.state == EntryState::DRAINING ||
                entry.zfPrelockConflictRevoked) {
                continue;
            }
            const bool participating =
                entry.zfEligibleForRelaxedRetire ||
                entry.zfLockAcquired || entry.zfLockReqPending ||
                entry.zfLockReqInFlight ||
                entry.zfLockReadyCycle != Cycles(0);
            if (!participating) {
                continue;
            }
            entry.zfPrelockConflictRevoked = true;
            entry.zfPublicationReady = false;
            entry.zfEligibleForRelaxedRetire = false;
            entry.zfLockReqPending = false;
            entry.zfConflictRefetchPending = true;
            ++conflicts;
            DPRINTF(LSQUnit,
                    "Revoking unfrozen MB prelock with conflicted batch "
                    "block:%#x ver:%llu acquired:%u inflight:%u\n",
                    entry.blockAddr, entry.version,
                    entry.zfLockAcquired ? 1 : 0,
                    entry.zfLockReqInFlight ? 1 : 0);
        }
    }

    return conflicts;
}

void
LSQUnit::MergeBuffer::invalidatePublicationReady(Addr line_addr)
{
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx] || entries[idx].blockAddr != line_addr) {
            continue;
        }
        auto &entry = entries[idx];
        if (!entry.zfPublicationReady &&
            entry.tagCompleteCandidate == 0 &&
            entry.tagCompleteGroup == 0) {
            continue;
        }
        // A member whose packet has already left the MB is no longer present
        // here. For every still-resident member, require a fresh exclusive
        // completion before it can freeze or issue.
        entry.zfPublicationReady = false;
        if (entry.tagCompleteCandidate != 0 ||
            entry.tagCompleteGroup != 0) {
            entry.zfInitialPrefetchNeeded = true;
        } else {
            // Allocation-time readiness is opportunistic and one-shot.
            // Candidate discovery will request a fresh exclusive completion
            // if this line is later needed for an atomic group.
            entry.zfInitialPrefetchNeeded = false;
            ++lsqPtr->stats.mbTagCompleteEarlyReadinessInvalidations;
        }
    }
}

bool
LSQUnit::MergeBuffer::validateAllMBZFLocked(bool &has_relevant) const
{
    // Cache-owned publications retain every group lock through completion.
    has_relevant = outstandingAtomicPublications != 0;

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        const auto &entry = entries[idx];
        has_relevant = true;
        if (entry.isAtomic) {
            return false;
        }
        if (!(entry.zfPermReady && entry.zfLockAcquired &&
              entry.zfEligibleForRelaxedRetire && entry.zfLineAddrValid &&
              !entry.zfLockReqPending && !entry.zfLockReqInFlight &&
              entry.zfLockReadyCycle == Cycles(0))) {
            return false;
        }
    }

    return true;
}

bool
LSQUnit::MergeBuffer::validateLowerVersionMBZFLocked(
    uint64_t version, bool &has_relevant) const
{
    // The CPU intentionally retains no per-group tag records after cache
    // acceptance. Conservatively count any cache-owned publication as an
    // older protected operation.
    has_relevant = outstandingAtomicPublications != 0;

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        const auto &entry = entries[idx];
        if (entry.version >= version) {
            continue;
        }

        has_relevant = true;
        if (entry.isAtomic) {
            return false;
        }
        if (!(entry.zfPermReady && entry.zfLockAcquired &&
              entry.zfEligibleForRelaxedRetire && entry.zfLineAddrValid &&
              !entry.zfLockReqPending && !entry.zfLockReqInFlight &&
              entry.zfLockReadyCycle == Cycles(0))) {
            return false;
        }
    }

    return true;
}

bool
LSQUnit::loadBlockedByReleaseMB(uint64_t version)
{
    if (!mergeBufferEnabled) {
        return false;
    }

    return mergeBuffer.hasReleaseOlderThan(version);
}

bool
LSQUnit::loadBlockedByReleaseSQ(uint64_t version, InstSeqNum load_seq) const
{
    bool versioned = cpu->versioningEnabled();
    for (const auto &entry : storeQueue) {
        if (!entry.valid()) {
            continue;
        }
        const auto &inst = entry.instruction();
        if (!inst || !inst->staticInst || !inst->staticInst->isRelease()) {
            continue;
        }
        if (inst->seqNum >= load_seq) {
            continue;
        }
        if (!versioned || inst->getMemOrderVersion() <= version) {
            return true;
        }
    }

    return false;
}

void
LSQUnit::markRequestZFLineLock(LSQRequest *request) const
{
    if (!request) {
        return;
    }

    auto mark_one = [](const RequestPtr &r) {
        if (r && r->hasPaddr()) {
            r->setFlags(Request::ZFENCE_LOCK_LINE);
        }
    };

    mark_one(request->mainReq());
    if (request->isSplit()) {
        mark_one(request->req(0));
        mark_one(request->req(1));
    }
}

bool
LSQUnit::MergeBuffer::issueAtomicPublication(
    uint64_t publication_id, LSQUnit *lsq_ptr)
{
    size_t member_idx = entries.size();
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx] ||
            entries[idx].tagCompleteGroup != publication_id) {
            continue;
        }
        const auto &candidate = entries[idx];
        bool older_same_line_pending = false;
        for (size_t older_idx = 0; older_idx < entries.size(); ++older_idx) {
            if (!entryValid[older_idx] ||
                entries[older_idx].tagCompleteGroup != publication_id) {
                continue;
            }
            const auto &older = entries[older_idx];
            if (older.blockAddr == candidate.blockAddr &&
                older.version < candidate.version) {
                older_same_line_pending = true;
                break;
            }
        }
        if (older_same_line_pending) {
            continue;
        }
        if (member_idx == entries.size() ||
            (lsq_ptr->tsoTagCompleteOutOfOrderDrain
                 ? candidate.version > entries[member_idx].version
                 : candidate.version < entries[member_idx].version)) {
            member_idx = idx;
        }
    }
    if (member_idx == entries.size()) {
        publishingAtomicPublication = 0;
        return false;
    }

    auto &entry = entries[member_idx];
    assert(entry.baseReq && entry.zfLockAcquired &&
           entry.zfPublicationReady);
    const uint64_t member_version = entry.version;
    const Addr published_block = entry.blockAddr;
    const uint32_t group_size = entry.tagCompleteGroupSize;
    assert(group_size != 0);
    RequestPtr req = std::make_shared<Request>(
        entry.blockAddr, lineSize, entry.baseReq->getFlags(),
        entry.baseReq->requestorId());
    req->setByteEnable(entry.byteValids);
    req->setFlags(Request::ZFENCE_LOCK_LINE |
                  Request::ZFENCE_RETAIN_LINE);
    req->setZFPublication(publication_id, group_size);
    if (entry.baseReq->hasContextId()) {
        req->setContext(entry.baseReq->contextId());
    }
    if (entry.baseReq->hasPC()) {
        req->setPC(entry.baseReq->getPC());
    }
    req->taskId(entry.baseReq->taskId());

    const bool full_line = lineSize == lsq_ptr->cacheLineSize() &&
        std::find(entry.byteValids.begin(), entry.byteValids.end(),
                  false) == entry.byteValids.end();
    PacketPtr pkt = full_line ? new Packet(req, MemCmd::WriteLineReq)
                              : Packet::createWrite(req);
    auto *buf = new uint8_t[lineSize];
    std::memcpy(buf, entry.blockData.data(), lineSize);
    pkt->dataDynamic(buf);
    auto *sender_state = new MergeBufferDrainSenderState(
        &entry, lsq_ptr, publication_id);
    pkt->senderState = sender_state;
    if (!lsq_ptr->trySendPacket(false, pkt)) {
        delete pkt->senderState;
        delete pkt;
        return false;
    }
    // sendTimingReq() has accepted the request and the normal cache
    // transaction owns its private data copy. The MB slot and ordering
    // records can be recycled immediately; the cache retains group locks
    // until all requests carrying this publication ID install.
    sender_state->entry = nullptr;
    entry.state = EntryState::DRAINING;
    bool bypassed_older = false;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (entryValid[idx] &&
            entries[idx].tagCompleteGroup == publication_id &&
            entries[idx].version < member_version) {
            bypassed_older = true;
            break;
        }
    }
    invalidateEntry(member_idx);
    ++lsq_ptr->stats.mbAtomicPublicationEarlyDeallocs;
    // Loads that previously waited on a partial MB match may now replay
    // against the accepted cache transaction.
    lsq_ptr->handleMBDrain(published_block);
    if (bypassed_older) {
        ++lsq_ptr->stats.mbTagCompleteOutOfOrderDrains;
    }
    ++lsq_ptr->stats.mbDrains;
    DPRINTF(LSQUnit,
            "Issued frozen tag-complete group %llu member ver:%llu "
            "members:%u%s\n",
            publication_id, member_version, group_size,
            bypassed_older ? " ahead of older member" : "");

    bool has_resident_members = false;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (entryValid[idx] &&
            entries[idx].tagCompleteGroup == publication_id) {
            has_resident_members = true;
            break;
        }
    }
    if (!has_resident_members) {
        publishingAtomicPublication = 0;
    }
    return true;
}

bool
LSQUnit::MergeBuffer::drainOne(LSQUnit *lsq_ptr)
{
    if (publishingAtomicPublication != 0) {
        return issueAtomicPublication(publishingAtomicPublication, lsq_ptr);
    }
    // Cache acceptance transfers ownership of every group member and lets
    // its MB slot be recycled, but the group is not globally visible until
    // the cache has installed every member and released the retained line
    // locks.  Do not let a younger store publication overtake that point.
    // This is a single busy condition, not an APQ: all write data and
    // per-line miss state live in the ordinary cache packets/MSHRs.
    if (outstandingAtomicPublications != 0) {
        return false;
    }

    // Find a ready entry regardless of index; prefer lowest version then retire time.
    size_t idx = entries.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entryValid[i]) {
            continue;
        }
        const auto &e = entries[i];
        bool ready = (e.state == EntryState::RETIRED ||
                      e.state == EntryState::FORCE_RETIRED);
        if (ready && e.tagCompleteCandidate != 0) {
            // Only a real same-line match may freeze otherwise baseline-ready
            // entries while its complete interval acquires publication locks.
            ready = false;
        }
        const bool atomic_group_lock_transaction =
            e.tagCompleteCandidate != 0 || e.tagCompleteGroup != 0;
        if (ready && atomic_group_lock_transaction && lsq_ptr &&
            lsq_ptr->zfenceEnable && lsq_ptr->zfenceLockLines &&
            (e.zfLockReqInFlight || e.zfLockReadyCycle != Cycles(0) ||
             e.zfPrefetchInFlight || e.zfInitialPrefetchNeeded ||
             !e.zfPublicationReady || e.zfConflictRefetchPending)) {
            // A real atomic group must not publish until all of its line
            // locks are complete.  An ungrouped entry, including one carrying
            // an early SQ prelock, retains baseline drain timing; its ordinary
            // zFence-tagged write consumes the speculative lock.
            ready = false;
        }
        if (ready && e.isRelease) {
            bool deps_clear = std::none_of(
                e.waitBits.begin(), e.waitBits.end(),
                [](bool v) { return v; });
            if (!deps_clear) {
                ready = false;
                if (lsq_ptr) {
                    lsq_ptr->stats.mbReleaseWaitCycles++;
                    DPRINTF(LSQUnit,
                            "Release MB entry not ready to drain "
                            "idx:%zu block:%#x ver:%llu (deps pending)\n",
                            i, e.blockAddr, e.version);
                }
            }
        }
        if (!ready) {
            continue;
        }
        if (idx == entries.size()) {
            idx = i;
            continue;
        }
        const auto &cur = entries[idx];
        if (e.version < cur.version ||
            (e.version == cur.version && e.retireCycle < cur.retireCycle)) {
            idx = i;
        }
    }

    if (idx == entries.size()) {
        DPRINTF(LSQUnit, "No MB entry ready to drain now:%lli.\n",
                lsq_ptr->cpu->curCycle());
        dumpWaitBits();
        if (!versionCounts.empty()) {
            DPRINTF(LSQUnit, "MB head version:%llu count:%u\n",
                    versionCounts.front().version,
                    versionCounts.front().count);
        }
        return false;
    }

    MergeBufferEntry &entry = entries[idx];

    // Ensure we only drain the lowest-version entries (head of version queue),
    // except for store-release entries which may drain once their dependencies
    // are cleared.
    if (!versionCounts.empty() &&
        entry.version != versionCounts.front().version) {
        DPRINTF(LSQUnit,
                "Deferring drain for block %#x with version ver:%llu "
                "(head version ver:%llu)\n",
                entry.blockAddr, entry.version, versionCounts.front().version);
        return false;
    }

    if (entry.tagCompleteGroup != 0) {
        publishingAtomicPublication = entry.tagCompleteGroup;
        assert(outstandingAtomicPublications == 0);
        assert(!atomicPublicationTimerActive);
        atomicPublicationStartCycle = lsq_ptr->cpu->curCycle();
        atomicPublicationTimerActive = true;
        ++outstandingAtomicPublications;
        return issueAtomicPublication(publishingAtomicPublication, lsq_ptr);
    }

    assert(entry.baseReq);
    RequestPtr base = entry.baseReq;

    if (entry.isAtomic) {
        RequestPtr atomic_req = std::make_shared<Request>(*base);
        PacketPtr pkt = Packet::createWrite(atomic_req);
        if (atomic_req->getSize() > 0) {
            uint8_t *buf = new uint8_t[atomic_req->getSize()];
            std::memset(buf, 0, atomic_req->getSize());
            pkt->dataDynamic(buf);
        }
        pkt->senderState = new MergeBufferDrainSenderState(&entry, lsq_ptr);

        if (!lsq_ptr->trySendPacket(false, pkt)) {
            if (auto *mb_state = dynamic_cast<MergeBufferDrainSenderState *>(
                    pkt->senderState)) {
                delete mb_state;
            }
            delete pkt;
            return false;
        }

        DPRINTF(LSQUnit,
                "Sending an atomic MB drain request for addr %#x "
                "version ver:%llu now:%lli\n",
                base->getPaddr(), entry.version, lsqPtr->cpu->curCycle());

        if (lsqPtr) {
            lsqPtr->stats.mbDrains++;
        }

        if (lsq_ptr->needsTSO) {
            lsq_ptr->storeInFlight = true;
        }

        entry.state = EntryState::DRAINING;
        return true;
    }

    Request::Flags flags = base->getFlags();
    RequestorID rid = base->requestorId();
    RequestPtr merged_req =
        std::make_shared<Request>(entry.blockAddr, lineSize, flags, rid);
    std::vector<bool> byte_enable = entry.byteValids;
    bool full_line = lineSize == lsq_ptr->cacheLineSize() &&
                     std::find(byte_enable.begin(), byte_enable.end(),
                               false) == byte_enable.end();

    if (base->hasContextId()) {
        merged_req->setContext(base->contextId());
    }
    if (base->hasPC()) {
        merged_req->setPC(base->getPC());
    }
    merged_req->taskId(base->taskId());
    merged_req->setByteEnable(byte_enable);
    const bool has_installed_prelock =
        entry.zfLockAcquired || entry.zfLockReqInFlight ||
        entry.zfLockReadyCycle != Cycles(0);
    if (lsqPtr && lsqPtr->zfenceEnable && lsqPtr->zfenceLockLines &&
        has_installed_prelock) {
        // A revoked entry can be forced to drain after the cache has granted
        // its prelock but before the modeled grant latency marks the MB entry
        // acquired.  Keep the recovery write zFence-tagged so it consumes
        // that installed prelock; MB_REVOKED_DRAIN below still prevents a
        // conflicting sibling MSHR from intercepting the recovery request.
        merged_req->setFlags(Request::ZFENCE_LOCK_LINE);
    }
    if (entry.zfPrelockConflictRevoked) {
        merged_req->setFlags(Request::MB_REVOKED_DRAIN);
    }

    PacketPtr pkt = full_line ? new Packet(merged_req, MemCmd::WriteLineReq)
                              : Packet::createWrite(merged_req);
    uint8_t *buf = new uint8_t[lineSize];
    std::memcpy(buf, entry.blockData.data(), lineSize);
    pkt->dataDynamic(buf);
    pkt->senderState = new MergeBufferDrainSenderState(&entry, lsq_ptr);

    if (!lsq_ptr->trySendPacket(false, pkt)) {
        if (auto *mb_state = dynamic_cast<MergeBufferDrainSenderState *>(
                pkt->senderState)) {
            delete mb_state;
        }
        delete pkt;
        return false;
    }

    DPRINTF(LSQUnit,
            "Sending a memory request for merge buffer entry block addr %#x "
            "version ver:%llu now:%lli\n",
            entry.blockAddr, entry.version, lsqPtr->cpu->curCycle());

    if (lsqPtr) {
        lsqPtr->stats.mbDrains++;
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

    std::string valid_bits;
    valid_bits.reserve(entryValid.size());
    for (bool v : entryValid) {
        valid_bits.push_back(v ? '1' : '0');
    }
    DPRINTF(LSQUnit, "MB valid vector: %s\n",
            valid_bits.empty() ? "-" : valid_bits.c_str());

    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        const auto &e = entries[idx];
        std::string bits;
        bits.reserve(e.waitBits.size());
        for (bool b : e.waitBits) {
            bits.push_back(b ? '1' : '0');
        }

        DPRINTF(LSQUnit,
                "MB[%llu] ver:%llu state:%s release:%d waitBits:%s "
                "alloc:%llu retire:%llu now:%llu\n",
                (unsigned long long)idx,
                (unsigned long long)e.version, stateStr(e.state),
                e.isRelease, bits.empty() ? "-" : bits.c_str(),
                (unsigned long long)e.allocCycle,
                (unsigned long long)e.retireCycle,
                (unsigned long long)lsqPtr->cpu->curCycle());
    }
}

void
LSQUnit::MergeBuffer::dumpState() const
{
    cprintf("Merge buffer: valid:%u/%u versions:%u publications:%llu "
            "publishing:%llu\n",
            static_cast<unsigned>(
                std::count(entryValid.begin(), entryValid.end(), true)),
            static_cast<unsigned>(entryValid.size()),
            static_cast<unsigned>(versionCounts.size()),
            static_cast<unsigned long long>(outstandingAtomicPublications),
            static_cast<unsigned long long>(publishingAtomicPublication));
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        if (!entryValid[idx]) {
            continue;
        }
        const auto &entry = entries[idx];
        cprintf("MB[%u] addr:%#x ver:%llu range:%llu-%llu state:%u "
                "candidate:%llu group:%llu lock(acq:%u pending:%u "
                "flight:%u ready:%llu revoked:%u prefetch:%u/%u) "
                "seq:%llu\n",
                static_cast<unsigned>(idx), entry.blockAddr,
                static_cast<unsigned long long>(entry.version),
                static_cast<unsigned long long>(entry.firstVersion),
                static_cast<unsigned long long>(entry.lastVersion),
                static_cast<unsigned>(entry.state),
                static_cast<unsigned long long>(entry.tagCompleteCandidate),
                static_cast<unsigned long long>(entry.tagCompleteGroup),
                entry.zfLockAcquired, entry.zfLockReqPending,
                entry.zfLockReqInFlight,
                static_cast<unsigned long long>(entry.zfLockReadyCycle),
                entry.zfPrelockConflictRevoked,
                entry.zfPrefetchInFlight,
                entry.zfConflictRefetchPending,
                static_cast<unsigned long long>(entry.seqNum));
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
            "block addr:%#x ver:%llu cycle:%lu\n",
            entry->blockAddr, entry->version, lsq_ptr->cpu->curCycle());

    lsq_ptr->handleMBDrain(entry);

    auto it = std::find_if(entries.begin(), entries.end(),
                           [entry](const MergeBufferEntry &e) {
                               return &e == entry;
                           });
    if (it != entries.end()) {
        size_t idx = std::distance(entries.begin(), it);
        invalidateEntry(idx);
    }

    if (lsq_ptr->needsTSO) {
        lsq_ptr->storeInFlight = false;
    }
}

void
LSQUnit::MergeBuffer::handleAtomicPublicationResp(
    bool publication_complete)
{
    if (!publication_complete) {
        return;
    }
    assert(outstandingAtomicPublications != 0);
    assert(atomicPublicationTimerActive);
    const Cycles latency =
        lsqPtr->cpu->curCycle() - atomicPublicationStartCycle;
    lsqPtr->stats.mbAtomicPublicationLatencyCycles += latency;
    ++lsqPtr->stats.mbAtomicPublicationsCompleted;
    atomicPublicationTimerActive = false;
    --outstandingAtomicPublications;
}

void
LSQUnit::MergeBuffer::invalidateEntry(size_t idx, bool retire_versions)
{
    if (idx >= entries.size() || idx >= entryValid.size()) {
        DPRINTF(LSQUnit, "Merge buffer invalidate idx out of bounds\n");
        return;
    }

    // Clear dependency bits in any in-flight release entries.
    for (auto &other : entries) {
        if (!other.isRelease || other.waitBits.empty() ||
            idx >= other.waitBits.size()) {
            continue;
        }
        other.waitBits[idx] = false;
    }

    auto &entry = entries[idx];
    assert(entry.state == EntryState::DRAINING);
    if (lsqPtr && entryValid[idx]) {
        lsqPtr->stats.mbResidencyCycles +=
            (lsqPtr->cpu->curCycle() - entry.allocCycle);
    }
    entryValid[idx] = false;
    if (idx == lastAllocatedIdx) {
        lastAllocatedIdx = numEntries;
    }
    if (retire_versions) {
        recordInvalidateVersion(entry.version);
        for (uint64_t absorbed : entry.absorbedVersions) {
            recordInvalidateVersion(absorbed);
        }
    }
    entry.baseReq = nullptr;
    entry.byteValids.assign(lineSize, false);
    entry.blockData.assign(lineSize, 0);
    entry.state = EntryState::MERGING;
    entry.retireCycle = Cycles(0);
    entry.unretireCount = 0;
    entry.blockAddr = 0;
    entry.firstVersion = 0;
    entry.version = 0;
    entry.lastVersion = 0;
    entry.tagCompleteRangeEligible = true;
    entry.absorbedVersions.clear();
    entry.tagCompleteGroup = 0;
    entry.tagCompleteGroupSize = 0;
    entry.tagCompleteGroupLastVersion = 0;
    entry.tagCompleteCandidate = 0;
    entry.seqNum = 0;
    entry.isRelease = false;
    entry.isAtomic = false;
    entry.atomicReq = nullptr;
    entry.waitBits.clear();
    // Clear zBit permission only at MB-entry teardown after drain.
    entry.zfPermReady = false;
    entry.zfLockAcquired = false;
    entry.zfEligibleForRelaxedRetire = false;
    entry.zfLineAddrValid = false;
    entry.zfLineAddr = 0;
    entry.zfLockReqPending = false;
    entry.zfLockReqInFlight = false;
    entry.zfLockReadyCycle = Cycles(0);
    entry.zfLeaseExpireCycle = Cycles(0);
    entry.zfPrelockConflictRevoked = false;
    entry.zfPrefetchInFlight = false;
    entry.zfInitialPrefetchNeeded = true;
    entry.zfPublicationReady = false;
    entry.zfConflictRefetchPending = false;
    if (lsqPtr) {
        const size_t valid_entries =
            std::count(entryValid.begin(), entryValid.end(), true);
        lsqPtr->stats.mbAvgOccupancy =
            (double)valid_entries / (double)numEntries;
    }
}

void
LSQUnit::MergeBuffer::recordAllocVersion(uint64_t version,
                                         bool is_release_store)
{
    if (versionCounts.empty()) {
        DPRINTF(LSQUnit, "Inserting a new version entry ver:%llu total:%llu\n",
                version, versionCounts.size());
        versionCounts.push_back({version, 1, false});
        return;
    }

    if (version >= versionCounts.back().version) {
        if (version == versionCounts.back().version) {
            DPRINTF(LSQUnit,
                    "Incrementing the version entry ver:%llu count %llu\n",
                    version, versionCounts.back().count);
            versionCounts.back().count++;
        } else {
            versionCounts.back().tailAllocByRelease = is_release_store;
            DPRINTF(LSQUnit, "Inserting a new version entry ver:%llu\n",
                    version);
            versionCounts.push_back({version, 1, false});
            // A normal allocation has not formed an atomic group, so preserve
            // the baseline version-retirement behavior. Tag-complete may hold
            // entries only after a concrete same-line recurrence has matched
            // and the complete interval has been frozen as a group.
            forceRetireVersionsBefore(version);
        }
        return;
    }

    auto it = std::find_if(versionCounts.begin(), versionCounts.end(),
                           [version](const VersionCountEntry &entry) {
                               return entry.version >= version;
                           });

    if (it != versionCounts.end() && it->version == version) {
        it->count++;
    } else {
        versionCounts.insert(it, VersionCountEntry{version, 1, false});
    }
}

void
LSQUnit::MergeBuffer::recordMergedVersion(uint64_t old_version,
                                          uint64_t new_version,
                                          bool is_release_store)
{
    assert(new_version > old_version);
    assert(new_version - old_version == 1);

    auto old_it = std::find_if(versionCounts.begin(), versionCounts.end(),
                               [old_version](const VersionCountEntry &entry) {
                                   return entry.version == old_version;
                               });
    assert(old_it != versionCounts.end());
    assert(old_it->count > 0);

    DPRINTF(LSQUnit,
            "Moving merged MB entry count from ver:%llu to ver:%llu\n",
            old_version, new_version);
    old_it->count--;
    if (old_it->count == 0) {
        versionCounts.erase(old_it);
    }

    recordAllocVersion(new_version, is_release_store);
}

void
LSQUnit::MergeBuffer::recordInvalidateVersion(uint64_t version)
{
    auto it = std::find_if(versionCounts.begin(), versionCounts.end(),
                           [version](const VersionCountEntry &entry) {
                               return entry.version == version;
                           });

    if (it == versionCounts.end()) {
        return;
    }

    assert(it->count > 0);

    DPRINTF(LSQUnit, "Decrementing the version entry ver:%llu count %llu\n",
            version, it->count);

    it->count--;
    if (it->count == 0) {
        if (lsqPtr && lsqPtr->cpu->versioningEnabled() &&
            !it->tailAllocByRelease) {
            const unsigned marked = lsqPtr->markLoadsHitExternalSnoop(version);
            if (marked) {
                DPRINTF(LSQUnit,
                        "Marked %u load(s) for re-exec due to external "
                        "snoops at version drain\n",
                        marked);
            }
        }
        DPRINTF(LSQUnit, "Deallocating the version entry ver:%llu\n",
                it->version);
        versionCounts.erase(it);
    }
}

size_t
LSQUnit::MergeBuffer::indexOf(const MergeBufferEntry *entry) const
{
    auto it = std::find_if(entries.begin(), entries.end(),
                           [entry](const MergeBufferEntry &e) {
                               return &e == entry;
                           });
    if (it == entries.end()) {
        return std::numeric_limits<size_t>::max();
    }
    return std::distance(entries.begin(), it);
}

} // namespace o3
} // namespace gem5
