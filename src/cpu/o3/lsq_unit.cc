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

#include "arch/generic/debugfaults.hh"
#include "base/str.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "debug/Activity.hh"
#include "debug/HtmCpu.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

namespace o3
{

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
    if (auto *pf_state =
            dynamic_cast<MergeBufferPrefetchSenderState*>(pkt->senderState)) {
        assert(mergeBufferPfInFlight > 0);
        --mergeBufferPfInFlight;
        delete pf_state;
        delete pkt;
        return true;
    } else if (auto *mb_state =
            dynamic_cast<MergeBufferDrainSenderState *>(pkt->senderState)) {
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

    mergeBufferEnabled = params.useMergeBuffer;
    mergeBufferPrefetchEnabled = params.mergeBufferPrefetch;
    mergeBufferPfInFlight = 0;
    mbRetireWhenFullValid = params.mbRetireWhenFullValid;

    storeDeallocateWidth = params.storeDeallocateWidth;
    storeDeallocsThisCycle = 0;
    lastStoreDeallocCycle = cpu->curCycle();

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
    mergeBuffer.reset();

    // hardware transactional memory
    // nesting depth
    htmStarts = htmStops = 0;

    storeWBIt = storeQueue.begin();

    retryPkt = NULL;
    memDepViolator = NULL;

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
      ADD_STAT(mbRetired, statistics::units::Count::get(),
               "Number of merge buffer entries retired"),
      ADD_STAT(mbDrains, statistics::units::Count::get(),
               "Number of merge buffer entries drained to cache"),
      ADD_STAT(mbUnretire, statistics::units::Count::get(),
               "Number of merge buffer entries unretired from RETIRED state"),
      ADD_STAT(mbForwards, statistics::units::Count::get(),
               "Number of loads forwarded from merge buffer")
{
    loadToUse
        .init(0, 299, 10)
        .flags(statistics::nozero);

    lqAvgOccupancy.precision(2);

    sqAvgOccupancy.precision(2);
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

    if (loadQueue.empty())
        return;

    auto iter = loadQueue.begin();

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;

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

    while (++iter != loadQueue.end()) {
        ld_inst = iter->instruction();
        assert(ld_inst);
        request = iter->request();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered() || !request)
            continue;

        DPRINTF(LSQUnit, "-- inst [sn:%lli] to pktAddr:%#x\n",
                    ld_inst->seqNum, invalidate_addr);

        if (force_squash ||
            request->isCacheBlockHit(invalidate_addr, cacheBlockMask)) {
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
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
                    ld_inst->tcBase()->getIsaPtr()->
                        handleLockedSnoopHit(ld_inst.get());
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

Fault
LSQUnit::checkViolations(typename LoadQueue::iterator& loadIt,
        const DynInstPtr& inst)
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

        Addr ld_eff_addr1 = ld_inst->effAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> depCheckShift;

        if (inst_eff_addr2 >= ld_eff_addr1 && inst_eff_addr1 <= ld_eff_addr2) {
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

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // Otherwise, mark the load has a possible load violation and
                // if we see a snoop before it's commited, we need to squash
                ld_inst->possibleLoadViolation(true);
                DPRINTF(LSQUnit, "Found possible load violation at addr: %#x"
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

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli]\n",
            inst->pcState(), inst->seqNum);

    assert(!inst->isSquashed());

    if (inst->isExecuted()) {
        DPRINTF(LSQUnit, "Load [sn:%lli] already executed\n", inst->seqNum);
        return NoFault;
    }

    load_fault = inst->initiateAcc();

    if (load_fault == NoFault && !inst->readMemAccPredicate()) {
        assert(inst->readPredicate());
        inst->setExecuted();
        inst->completeAcc(nullptr);
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return NoFault;
    }

    if (inst->isTranslationDelayed() && load_fault == NoFault)
        return load_fault;

    if (load_fault != NoFault && inst->translationCompleted() &&
            inst->savedRequest->isPartialFault()
            && !inst->savedRequest->isComplete()) {
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
        if (!inst->readPredicate())
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n",
                inst->seqNum,
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

            if (checkLoads)
                return checkViolations(it, inst);
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

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = store_inst->lqIt;

    Fault store_fault = store_inst->initiateAcc();

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

    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Writing back  blocked store\n");
        writebackBlockedStore();
    }

    if (mergeBufferEnabled) {
        if (!mergeBuffer.isEmpty()) {
            mergeBuffer.updateRetiredEntries(now);
            iewStage->activityThisCycle();
        }

        if (((!needsTSO) || (!storeInFlight)) &&
            lsq->cachePortAvailable(false)) {
            mergeBuffer.drainOne(this);
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

        DPRINTF(LSQUnit, "Trying to drain store at idx:%i PC:%s [sn:%lu]\n",
                storeWBIt.idx(), storeWBIt->instruction()->pcState(),
                storeWBIt->instruction()->seqNum);

        if (!mergeBufferEnabled && isStoreBlocked) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, cache"
                    " is blocked!\n");
            break;
        }

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
        assert(!storeWBIt->committed());

        DynInstPtr inst = storeWBIt->instruction();
        LSQRequest* request = storeWBIt->request();

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

        if (mergeBufferEnabled && !request->mainReq()->isLocalAccess() &&
            !request->mainReq()->isLLSC() && !request->mainReq()->isAtomic()) {

            bool merged_ok = true;
            MergeBuffer::MergeBufferEntry *mb_entry = nullptr;

            if (request->isSplit()) {
                // For split stores, make sure both fragments can be merged
                // before releasing the SQ entry.
                auto req0 = request->req(0);
                auto req1 = request->req(1);
                const size_t size0 = req0->getSize();
                const size_t size1 = req1->getSize();

                bool can_merge_both = mergeBuffer.canAcceptSplitStore(request);

                if (can_merge_both) {
                    mb_entry = mergeBuffer.addStore(
                        now, req0->getPaddr(),
                        reinterpret_cast<uint8_t *>(storeWBIt->data()),
                        size0, storeWBIt, storeWBIt->isAllZeros());

                    auto *mb_entry2 = mergeBuffer.addStore(
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
            }

            DPRINTF(LSQUnit,
                    "Merge for store idx:%i PC:%s "
                    "to Addr:%#x, data:%#x [sn:%lli] %s\n",
                    storeWBIt.idx(), inst->pcState(),
                    request->mainReq()->getPaddr(), (int)*(storeWBIt->data()),
                    inst->seqNum, (mb_entry && merged_ok) ?
                        "accepted" : "blocked");

            if (mb_entry && merged_ok) {
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
        }
        assert(storesToWB >= 0);
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

    if (!inst->isExecuted()) {
        inst->setExecuted();

        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
        } else {
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

    if (needsTSO) {
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

    for (const auto& e: loadQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", storeQueue.size());
    cprintf("Store queue: ");

    for (const auto& e: storeQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }

    cprintf("\n");
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

    DPRINTF(LSQUnit, "[sn:%lli] Read called, load idx: %i, store idx: %i, "
            "storeHead: %i addr: %#x%s\n", load_inst->seqNum,
            load_idx - 1, load_inst->sqIt._idx, storeQueue.head() - 1,
            request->mainReq()->getPaddr(), request->isSplit() ? " split" :
            "");

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

                PacketPtr data_pkt = new Packet(request->mainReq(),
                        MemCmd::ReadReq);
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
            coverage = mergeBuffer.forwardCoverage(request->mainReq()->getPaddr(),
                                                   request->mainReq()->getSize());
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
                    stallingMBAddr =  request->mainReq()->getPaddr() & cacheBlockMask;
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
    // an exta cycle to re-issue and execute
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

            if (lsqPtr && lsqPtr->needsTSO && &(*it) != &entries.back()) {
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
                        : (it->state == EntryState::DRAINING) ? "DRAINING"
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
                // Use a soft prefetch so it can go through cache/MSHR normally.
                pf_pkt->cmd = MemCmd::SoftPFReq;
                // Give the packet a data buffer to satisfy downstream asserts.
                pf_pkt->allocate();
                pf_pkt->senderState = new MergeBufferPrefetchSenderState(lsqPtr);

                if (lsqPtr->trySendPacket(false, pf_pkt)) {
                    ++lsqPtr->mergeBufferPfInFlight;
                } else {
                    delete static_cast<MergeBufferPrefetchSenderState*>(
                        pf_pkt->senderState);
                    delete pf_pkt;
                }
            }

            if (lsqPtr) {
                lsqPtr->stats.mbAllocations++;
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

bool
LSQUnit::MergeBuffer::canAcceptSplitStore(LSQRequest *request) const
{
    auto req0 = request->req(0);
    auto req1 = request->req(1);

    auto addr0 = req0->getPaddr() & ~(lineSize - 1);
    auto addr1 = req1->getPaddr() & ~(lineSize - 1);

    bool addr0_exists = false;
    bool addr1_exists = false;

    bool merge_req0 = false;
    bool merge_req1 = false;

    auto checkAddrMerge = [this](const Addr addr, bool &addr_exists, bool &can_merge) {
        return [this, addr, &addr_exists, &can_merge](const MergeBufferEntry &e) {
            if (e.blockAddr == addr) {
                addr_exists = true;

                if (e.state == EntryState::MERGING ||
                    (e.state == EntryState::RETIRED && e.unretireCount < maxUnretire)) {
                    can_merge = true;
                }
            }

            return false;
        };
    };

    std::for_each(entries.begin(), entries.end(),
                 checkAddrMerge(addr0, addr0_exists, merge_req0));
    std::for_each(entries.begin(), entries.end(),
                 checkAddrMerge(addr1, addr1_exists, merge_req1));

    int num_allocs = !addr0_exists + !addr1_exists;

    if (addr0_exists && !merge_req0)
        return false;

    if (addr1_exists && !merge_req1)
        return false;

    if (entries.size() > (numEntries - num_allocs))
        return false;

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

bool
LSQUnit::MergeBuffer::drainOne(LSQUnit *lsq_ptr)
{
    // Oldest entry is at the front of the list; enforce FIFO draining in TSO.
    auto it = entries.begin();
    while (it != entries.end() && !it->valid) {
        it = entries.erase(it);
    }

    if (lsq_ptr->needsTSO) {
        if (it == entries.end() || (it->state != EntryState::RETIRED &&
                                    it->state != EntryState::FORCE_RETIRED)) {
            return false;
        }
    } else {
        it = std::find_if(it, entries.end(), [](MergeBufferEntry &e) {
            return e.valid &&
                   (e.state == EntryState::RETIRED ||
                    e.state == EntryState::FORCE_RETIRED);
        });
        if (it == entries.end()) {
            return false;
        }
    }

    MergeBufferEntry &entry = *it;
    assert(entry.baseReq);
    RequestPtr base = entry.baseReq;

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

    lsq_ptr->handleMBDrain(entry);

    entries.remove_if(
        [entry](const MergeBufferEntry &e) { return &e == entry; });
}

} // namespace o3
} // namespace gem5
