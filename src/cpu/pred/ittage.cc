/*
 * Copyright (c) 2025 - Pranith Kumar
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

#include "cpu/pred/ittage.hh"

#include "base/intmath.hh"
#include "debug/Indirect.hh"
#include "debug/ITTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

ITTAGE::ITTAGE(const ITTAGEParams &params)
    : IndirectPredictor(params),
      numPredTables(params.numPredTables),
      predTableEntries(params.predTableEntries),
      //predTableTagBits(params.predTableTagBits),
      //predTableAssociativity(params.predTableAssociativity),
      predTableHistLengths(params.predTableHistLengths),
      pathLength(params.indirectPathLength),
      speculativePathLength(params.speculativePathLength),
      instShift(params.instShiftAmt),
      replPolicy(params.tableReplPolicy),
      useAltOnNA(4, 0),
      hashGHR(params.indirectHashGHR),
      hashTargets(params.indirectHashTargets),
      numSets(params.indirectSets),
      //numWays(params.indirectWays),
      tagBits(params.indirectTagSize),
      ghrNumBits(params.indirectGHRBits),
      ghrMask(mask(params.indirectGHRBits)),
      tableCtrBits(params.tableCtrBits),
      tableCtrInit(params.tableCtrInit),
      tableUsefulBits(params.tableUsefulBits),
      stats(this)
{
    if (!isPowerOf2(numSets)) {
        panic("Indirect predictor requires power of 2 number of sets");
    }

    // we reuse the same tableIndexingPolicy, so create a new
    // indexing policy object by copying it
    indexingPolicy = params.tableIndexingPolicy->clone();
    threadInfo.resize(params.numThreads);

    std::string table_name_prefix(".table_");

    for (unsigned i = 0; i < numPredTables; i++) {
        std::string table_name = name() + table_name_prefix + std::to_string(i);
        auto predTable =
            new AssociativeCache<NewIPredEntry>(
                table_name.c_str(), predTableEntries,
                predTableEntries, replPolicy,
                indexingPolicy->clone(),
                NewIPredEntry(genTagExtractor(indexingPolicy),
                              tableCtrBits, tableCtrInit,
                              tableUsefulBits));

        predTable->setDebugFlag(::gem5::debug::ITTAGE);
        predTables.push_back(predTable);
    }

    fatal_if(ghrNumBits > (sizeof(ThreadInfo::ghr)*8), "ghr_size is too big");

    stats.tableHits.init(numPredTables);
    stats.tableInserts.init(numPredTables);
}

void
ITTAGE::reset()
{
    DPRINTF(Indirect, "ITTAGE: Reset Indirect predictor\n");

    for (auto& ti : threadInfo) {
        ti.ghr = 0;
        ti.pathHist.clear();
    }

    for (int table_idx = numPredTables - 1; table_idx >= 0; table_idx--) {
        predTables[table_idx]->clear();
    }
}

void
ITTAGE::genIndirectInfo(ThreadID tid, void* &i_history)
{
    // Record the GHR as it was before this prediction
    // It will be used to recover the history in case this prediction is
    // wrong or belongs to bad path
    IndirectHistory* history = new IndirectHistory;
    history->ghr = threadInfo[tid].ghr;
    i_history = static_cast<void*>(history);
}

void
ITTAGE::updateDirectionInfo(ThreadID tid, bool taken,
                            Addr pc, Addr target)
{
    // Direction history
    threadInfo[tid].ghr <<= 1;
    threadInfo[tid].ghr |= taken;
    threadInfo[tid].ghr &= ghrMask;
}

// Interface methods ------------------------------
const PCStateBase *
ITTAGE::lookup(ThreadID tid, InstSeqNum sn, Addr pc, void * &i_history)
{
    assert(i_history==nullptr);

    genIndirectInfo(tid, i_history);
    IndirectHistory *history = static_cast<IndirectHistory*>(i_history);

    history->pcAddr = pc;
    history->was_indirect = true;

    /** Do the prediction for indirect branches (no returns) */
    PCStateBase* target = nullptr;
    history->hit = lookup(tid, pc, target, history);
    return target;
}

uint64_t
ITTAGE::getTableTag(Addr pc, uint64_t ghr, unsigned table_idx)
{
    return (pc >> instShift) ^ (ghr & mask(predTableHistLengths[table_idx]));
}

bool
ITTAGE::lookup(ThreadID tid, Addr br_addr,
	       PCStateBase * &target,
	       IndirectHistory * &history)
{
    history->hit = false;
    NewIPredEntry *pred_entry = nullptr;
    NewIPredEntry *alt_pred_entry = nullptr;

    for (int table_idx = numPredTables - 1; table_idx >= 0; table_idx--) {
        uint64_t table_tag = getTableTag(br_addr, history->ghr, table_idx);

        DPRINTF(Indirect, "ITTAGE: Looking up tag: %#x in table %d\n", table_tag, table_idx);

        auto entry = predTables[table_idx]->findEntry(table_tag);
        if (entry && entry->ctr) {
            history->hit = true;
            stats.tableHits[table_idx]++;
            if (pred_entry == nullptr) {
                pred_entry = entry;
                history->targetAddr = entry->target->instAddr();
                history->entry_table_idx = table_idx;
                history->table_tag = table_tag;
                DPRINTF(Indirect, "ITTAGE: Hit PC:%#x found in main table %d entry ctr %d\n", br_addr, table_idx, (int)entry->ctr);
            } else {
                alt_pred_entry = entry;
                history->altTargetAddr = entry->target->instAddr();
                history->alt_entry_table_idx = table_idx;
                history->alt_table_tag = table_tag;
                DPRINTF(Indirect, "ITTAGE: Hit PC:%#x found in alt table %d entry ctr %d\n", br_addr, table_idx, (int)entry->ctr);
                break;
            }
        }
    }

    stats.lookups++;

    DPRINTF(Indirect, "ITTAGE: Looking up PC:%#x, "
                    "ghr:%#x, pathHist sz:%d\n",
                    history->pcAddr,
                    history->ghr, threadInfo[tid].pathHist.size());

    auto entry = pred_entry;

    if (alt_pred_entry && !pred_entry->useful) {
        entry = alt_pred_entry;
        history->using_alt_pred = true;
        DPRINTF(Indirect, "ITTAGE: Alt entry is useful while main entry is not\n");
    } else if (useAltOnNA && alt_pred_entry) {
        entry = alt_pred_entry;
        history->using_alt_pred = true;
        DPRINTF(Indirect, "ITTAGE: Forcing alt entry due to USE_ALT_ON_NA\n");
    }

    if (entry) {
        set(target, entry->target);
    }

    return history->hit;
}

void
ITTAGE::commit(ThreadID tid, InstSeqNum sn, bool mispredict, void * &i_history,
               const Addr target_addr)
{
    if (i_history == nullptr) return;
    // we do not need to recover the GHR, so delete the information
    IndirectHistory *history = static_cast<IndirectHistory*>(i_history);

    DPRINTF(Indirect, "ITTAGE: Committing [sn:%lu], PC:%#x, ghr:%#x, pathHist sz:%lu base_pred:%s\n",
	    sn, history->pcAddr, history->ghr,
	    threadInfo[tid].pathHist.size(), history->using_alt_pred ? "true" : "false");

    if (history->was_indirect && history->hit) {
        // update confidence of the entry in the highest table for this PC
        auto table_idx = history->entry_table_idx;
        auto alt_table_idx = history->alt_entry_table_idx;
        assert(table_idx < numPredTables);
        assert(alt_table_idx < numPredTables);

        auto entry = predTables[table_idx]->findEntry(history->table_tag);
        auto alt_entry = predTables[alt_table_idx]->findEntry(history->alt_table_tag);

        if (entry) {
            if (!mispredict) {
                // correct prediction, increase confidence
                DPRINTF(Indirect, "ITTAGE: CORR prediction Branch PC:%#x predicted target:%#x actual target:%#x\n",
                        history->pcAddr, history->targetAddr, target_addr);
                if (history->using_alt_pred) {
                    alt_entry->ctr++;
                    DPRINTF(Indirect, "ITTAGE: Incrementing ALT entry confidence PC: %#x ctr: %d table:%d\n",
                            history->pcAddr, (int)alt_entry->ctr, alt_table_idx);
                    alt_entry->useful = true;
                } else {
                    entry->ctr++;
                    DPRINTF(Indirect, "ITTAGE: Incrementing MAIN entry confidence PC: %#x ctr: %d table:%d\n",
                            history->pcAddr, (int)entry->ctr, table_idx);
                    entry->useful = true;
                }
            } else {
                // wrong prediction, decrease confidence
                DPRINTF(Indirect, "ITTAGE: MISPRED Branch PC:%#x predicted target:%#x actual target:%#x\n",
                        history->pcAddr, history->targetAddr, target_addr);
                if (history->using_alt_pred && alt_entry->target->instAddr() != target_addr) {
                    alt_entry->ctr--;
                    DPRINTF(Indirect, "ITTAGE: Decrementing ALT entry confidence PC: %#x ctr: %d table:%d\n",
                            history->pcAddr, (int)alt_entry->ctr, alt_table_idx);
                } else if (entry->target->instAddr() != target_addr){
                    entry->ctr--;
                    DPRINTF(Indirect, "ITTAGE: Decrementing MAIN entry confidence PC: %#x ctr: %d table:%d\n",
                            history->pcAddr, (int)entry->ctr, table_idx);
                }

                if (history->using_alt_pred && alt_entry->target->instAddr() == target_addr) {
                    // if the main prediction mispredicted but alternate prediction is correct
                    // then mark the main entry as not useful 
                    DPRINTF(Indirect, "ITTAGE: Marking MAIN entry as not useful PC: %#x ctr: %d table:%d\n",
                            history->pcAddr, (int)entry->ctr, table_idx);
                    entry->useful = false;
                }
            }


            if (entry->ctr == 0) {
                // invalidate the older entry
                if (!entry->target) {
                    delete entry->target;
                }
                predTables[table_idx]->invalidate(entry);
                DPRINTF(Indirect, "ITTAGE: Invalidating entry PC: %#x ctr: %d table:%d\n",
                        history->pcAddr, (int)entry->ctr, table_idx);
            }
            if (alt_entry && alt_entry->ctr == 0) {
                // invalidate the older entry
                if (!alt_entry->target) {
                    delete alt_entry->target;
                }
                predTables[table_idx]->invalidate(alt_entry);
                DPRINTF(Indirect, "ITTAGE: Invalidating entry PC: %#x ctr: %d table:%d\n",
                        history->pcAddr, (int)alt_entry->ctr, table_idx);
            }
        }
    }


    delete history;
    i_history = nullptr;

    /** Delete histories if the history grows to much */
    while (threadInfo[tid].pathHist.size()
            >= (pathLength + speculativePathLength)) {
        threadInfo[tid].pathHist.pop_front();
    }
}

void
ITTAGE::update(ThreadID tid, InstSeqNum sn, Addr pc,
               bool squash, bool taken, const PCStateBase& target,
               BranchType br_type, void * &i_history)
{
    // If there is no history we did not use the indirect predictor yet.
    // Create one
    if (i_history==nullptr) {
        genIndirectInfo(tid, i_history);
    }

    IndirectHistory *history = static_cast<IndirectHistory*>(i_history);
    assert(history!=nullptr);

    DPRINTF(Indirect, "ITTAGE: Update [sn:%lu] PC:%#x, squash:%i, ghr:%#x,path sz:%i\n",
               sn, pc, squash, history->ghr, threadInfo[tid].pathHist.size());

    /** If update was called during squash we need to fix the indirect
     * path history and the global path history.
     * We restore the state before this branch incorrectly updated it
     * and perform the update afterwards again.
     */
    history->was_indirect = isIndirectNoReturn(br_type);
    if (squash) {

        /** restore global history */
        threadInfo[tid].ghr = history->ghr;

        /** For indirect branches recalculate index and tag */
        if (history->was_indirect) {
            if (!threadInfo[tid].pathHist.empty()) {
                threadInfo[tid].pathHist.pop_back();
            }

            DPRINTF(Indirect, "ITTAGE: Record Target [sn:%lu], PC:%#x, TGT:%#x, "
                        "ghr:%#x\n",
                    sn, history->pcAddr, target, history->ghr);
        }
    }

    // Only indirect branches are recorded in the path history
    if (history->was_indirect) {
        DPRINTF(Indirect, "ITTAGE: Recording PC:%#x [sn:%lu] in path history\n",
                history->pcAddr, sn);
        threadInfo[tid].pathHist.emplace_back(
                                    history->pcAddr, target.instAddr(), sn);

        stats.indirectRecords++;
    }

    // All branches update the global history
    updateDirectionInfo(tid,taken, history->pcAddr, target.instAddr());

    // Finally if update is called during at squash we know the target
    // we predicted was wrong therefore we update the target.
    // We only record the target if the branch was indirect and taken
    if (squash && history->was_indirect && taken) {
        recordTarget(tid, sn, target, history);
    }
}

void
ITTAGE::squash(ThreadID tid, InstSeqNum sn, void * &i_history)
{
    if (i_history == nullptr) return;

    // we do not need to recover the GHR, so delete the information
    IndirectHistory *history = static_cast<IndirectHistory*>(i_history);

    DPRINTF(Indirect, "ITTAGE: Squashing [sn:%lu], PC:%#x, indirect:%i, "
                    "ghr:%#x, pathHist sz:%#x\n",
                    sn, history->pcAddr, history->was_indirect,
                    history->ghr,
                    threadInfo[tid].pathHist.size());


    // Revert the global history register.
    threadInfo[tid].ghr = history->ghr;

    // If we record this branch as indirect branch
    // remove it from the history.
    // Restore the old head in the history.
    if (history->was_indirect) {

        // Should not be empty
        if (threadInfo[tid].pathHist.size() < pathLength) {
            stats.speculativeOverflows++;
        }

        if (!threadInfo[tid].pathHist.empty()) {
            threadInfo[tid].pathHist.pop_back();
        }
    }

    delete history;
    i_history = nullptr;
}

// Internal functions ------------------------------
void
ITTAGE::recordTarget(ThreadID tid, InstSeqNum sn,
                     const PCStateBase& target, IndirectHistory * &history)
{
    // Should have just squashed so this branch should be the oldest
    // and it should be predicted as indirect.
    assert(!threadInfo[tid].pathHist.empty());
    assert(history->was_indirect);

    if (threadInfo[tid].pathHist.rbegin()->pcAddr != history->pcAddr) {
        DPRINTF(Indirect, "ITTAGE: History seems to be corrupted. %#x != %#x\n",
                    history->pcAddr,
                    threadInfo[tid].pathHist.rbegin()->pcAddr);
	assert(0);
    }

    DPRINTF(Indirect, "ITTAGE: Record Target [sn:%lu], PC:%#x, TGT:%#x, "
                      "ghr:%#x\n",
            sn, history->pcAddr, target.instAddr(), history->ghr);

    stats.targetRecords++;

    // update the tables
    // if entry already exists, decrement the counter
    auto table_idx = history->entry_table_idx;
    // auto alt_table_idx = history->alt_entry_table_idx;
    auto nxt_table_idx = 0;
    bool alloc = true;
    if (history->hit) {
        // get the next table that we need to alloc into
        nxt_table_idx = (table_idx < (numPredTables - 1)) ? table_idx + 1 : table_idx;
        if (nxt_table_idx == table_idx) {
            // entry already exists in the highest table
            // we wait until confidence decrements to 0 at commit and invalidation
            alloc = false;
        }

    }

    if (alloc) {
        auto table_tag = getTableTag(history->pcAddr, history->ghr, nxt_table_idx);
        auto victim = predTables[nxt_table_idx]->findVictim(table_tag);

        DPRINTF(Indirect,
                "ITTAGE: Inserting Target ([sn:%lu] PC:%#x target:%#x tag:%#x) in table %d\n",
                sn, history->pcAddr, target.instAddr(), table_tag, nxt_table_idx);

        victim->tag = table_tag;
        victim->useful = true;
        victim->resetCtr();
        set(victim->target, target);
        predTables[nxt_table_idx]->insertEntry({table_tag}, victim);

        // does this get used at commit?
        history->table_tag = table_tag;
        history->entry_table_idx = nxt_table_idx;
    }
}

inline Addr
ITTAGE::getSetIndex(Addr br_addr, ThreadID tid)
{
    Addr hash = br_addr >> instShift;
    if (hashGHR) {
        hash ^= threadInfo[tid].ghr;
    }
    if (hashTargets) {
        unsigned hash_shift = floorLog2(numSets) / pathLength;
        for (int i = threadInfo[tid].pathHist.size()-1, p = 0;
             i >= 0 && p < pathLength; i--, p++) {
            hash ^= (threadInfo[tid].pathHist[i].targetAddr >>
                     (instShift + p*hash_shift));
        }
    }
    return hash & (numSets-1);
}

inline Addr
ITTAGE::getTag(Addr br_addr)
{
    return (br_addr >> instShift) & ((0x1<<tagBits)-1);
}


ITTAGE::IndirectStats::IndirectStats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(tableHits,
             "Number of hits in the ITTAGE tables"),
    ADD_STAT(tableMisses, statistics::units::Count::get(),
             "Number of misses in the ITTAGE tables"),
    ADD_STAT(lookups, statistics::units::Count::get(),
             "Number of lookups"),
    ADD_STAT(hits, statistics::units::Count::get(),
             "Number of hits of a tag"),
    ADD_STAT(misses, statistics::units::Count::get(),
             "Number of misses"),
    ADD_STAT(targetRecords, statistics::units::Count::get(),
             "Number of targets that where recorded/installed in the cache"),
    ADD_STAT(indirectRecords, statistics::units::Count::get(),
             "Number of indirect branches/calls recorded in the"
             " indirect hist"),
    ADD_STAT(speculativeOverflows, statistics::units::Count::get(),
             "Number of times more than the allowed capacity for speculative "
             "branches/calls where in flight and destroy the path history")
{
}

} // namespace branch_prediction
} // namespace gem5
