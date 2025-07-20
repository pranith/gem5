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

#ifndef __CPU_PRED_ITTAGE_HH__
#define __CPU_PRED_ITTAGE_HH__

#include <deque>

#include "base/cache/associative_cache.hh"
#include "base/cache/cache_entry.hh"
#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/indirect.hh"
#include "params/ITTAGE.hh"

class BaseIndexingPolicy;

namespace replacement_policy {
class Base;
}

namespace gem5 {

namespace branch_prediction {

class ITTAGE : public IndirectPredictor
{
  public:
    ITTAGE(const ITTAGEParams& params);

    /** Indirect predictor interface */
    void
    reset() override;

    const PCStateBase*
    lookup(ThreadID tid, InstSeqNum sn, Addr pc, void*& iHistory) override;
    void
    update(ThreadID tid, InstSeqNum sn, Addr pc, bool squash, bool taken,
           const PCStateBase& target, BranchType br_type,
           void*& iHistory) override;
    void
    squash(ThreadID tid, InstSeqNum sn, void*& iHistory) override;
    void
    commit(ThreadID tid, InstSeqNum sn, bool mispredict, void*& iHistory,
           const Addr target) override;

    /** ------------------
     * The actual predictor
     * -------------------
     * */
  private:
    const unsigned numPredTables;
    const unsigned predTableEntries;
    // const unsigned predTableTagBits;
    // const unsigned predTableAssociativity;
    const std::vector<unsigned> predTableHistLengths;
    const unsigned pathLength;
    const unsigned speculativePathLength;
    const unsigned instShift;

    replacement_policy::Base* replPolicy;
    BaseIndexingPolicy* indexingPolicy;

    SatCounter8 useAltOnNA;

    const bool hashGHR;
    const bool hashTargets;
    const unsigned numSets;
    // const unsigned numWays;
    const unsigned tagBits;
    const unsigned ghrNumBits;
    const unsigned ghrMask;
    const unsigned tableCtrBits;
    const unsigned tableCtrInit;
    const unsigned tableUsefulBits;

    struct IPredEntry {
        Addr tag = 0;
        std::unique_ptr<PCStateBase> target;
    };

    class NewIPredEntry : public CacheEntry
    {
      public:
        using TagExtractor = std::function<Addr(Addr)>;

        Addr tag;
        PCStateBase* target;

        SatCounter8 ctr;
        bool useful;

        NewIPredEntry(TagExtractor ext, unsigned ctr_bits, unsigned ctr_init,
                      unsigned useful_bits)
            : CacheEntry(ext), tag(0), target(nullptr),
              ctr(ctr_bits, ctr_init), useful(false)
        {
        }

        void
        resetCtr(void)
        {
            ctr.reset();
        }
    };

    std::vector<AssociativeCache<NewIPredEntry>*> predTables;

    struct HistoryEntry {
        HistoryEntry(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num)
            : pcAddr(br_addr), targetAddr(tgt_addr), seqNum(seq_num)
        {
        }
        HistoryEntry() : pcAddr(0), targetAddr(0), seqNum(0) {}
        Addr pcAddr;
        Addr targetAddr;

        InstSeqNum seqNum;
    };

    /** Indirect branch history information
     * Used for prediction, update and recovery
     */
    struct IndirectHistory {
        /* data */
        Addr pcAddr;
        Addr targetAddr;
        Addr altTargetAddr;
        InstSeqNum seqNum;

        uint32_t entry_table_idx;
        uint32_t alt_entry_table_idx;

        bool hit;
        Addr table_tag;
        Addr alt_table_tag;

        uint64_t ghr;
        uint64_t pathHist;

        bool was_indirect;
        bool using_alt_pred;

        IndirectHistory()
            : pcAddr(MaxAddr), targetAddr(MaxAddr), altTargetAddr(MaxAddr),
              entry_table_idx(0), alt_entry_table_idx(0), hit(false),
              was_indirect(false), using_alt_pred(false)
        {
        }
    };

    /** Per thread path and global history registers*/
    struct ThreadInfo {
        // Path history register
        std::deque<HistoryEntry> pathHist;
        // Global direction history register
        uint64_t ghr = 0;
    };

    std::vector<ThreadInfo> threadInfo;

    // ---- Internal functions ----- //
    bool
    lookup(ThreadID tid, Addr br_addr, PCStateBase*& target,
           IndirectHistory*& history);
    void
    recordTarget(ThreadID tid, InstSeqNum sn, const PCStateBase& target,
                 IndirectHistory*& history);

    // Helper functions to generate and modify the
    // direction info
    void
    genIndirectInfo(ThreadID tid, void*& iHistory);
    void
    updateDirectionInfo(ThreadID tid, bool taken, Addr pc, Addr target);

    // Helper to compute set and tag
    inline Addr
    getSetIndex(Addr br_addr, ThreadID tid);
    inline Addr
    getTag(Addr br_addr);
    inline Addr
    getTableTag(Addr pc, uint64_t ghr, unsigned table_idx);

    inline bool
    isIndirectNoReturn(BranchType type)
    {
        return (type == BranchType::CallIndirect) ||
               (type == BranchType::IndirectUncond) ||
               (type == BranchType::IndirectCond);
    }

  protected:
    struct IndirectStats : public statistics::Group {
        IndirectStats(statistics::Group* parent);
        // Main Predictor stats
        statistics::Vector tableHits;
        statistics::Vector tableInserts;
        statistics::Scalar tableMisses;

        // Base Predictor Stats
        statistics::Scalar lookups;
        statistics::Scalar hits;
        statistics::Scalar misses;
        statistics::Scalar targetRecords;
        statistics::Scalar indirectRecords;
        statistics::Scalar speculativeOverflows;

    } stats;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_ITTAGE_HH__
