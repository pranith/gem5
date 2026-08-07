/*
 * Copyright (c) 2026
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

#ifndef __CPU_PO3_MDP_TAGE_HH__
#define __CPU_PO3_MDP_TAGE_HH__

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "cpu/po3/mem_dep_predictor.hh"

namespace gem5
{

namespace po3
{

/**
 * Standalone MDP-TAGE memory-dependence predictor.
 *
 * This implements the standalone configuration evaluated by Kim and Ros
 * (HPCA 2024), based on Perais and Seznec's MDP-TAGE: tagged components use
 * geometrically increasing global branch-history lengths and predict one
 * dynamic store distance.  A violation with no prediction allocates at the
 * shortest history; a violation after an incorrect prediction allocates in
 * the next available longer-history component.  The longest useful tag match
 * provides the prediction.
 */
class MDPTage : public MemDepPredictor
{
  public:
    MDPTage(std::string_view name,
            const std::vector<unsigned> &history_lengths,
            const std::vector<unsigned> &table_entries,
            const std::vector<unsigned> &tag_bits, unsigned distance_bits,
            uint64_t useful_reset_period, unsigned false_decay_log2);

    void observeInstruction(const MemDepPredInstruction &inst) override;
    void violation(Addr store_pc, Addr load_pc, InstSeqNum store_seq_num,
                   InstSeqNum load_seq_num) override;
    void insertLoad(Addr load_pc, InstSeqNum load_seq_num) override;
    void insertStore(Addr store_pc, InstSeqNum store_seq_num,
                     ThreadID tid) override;
    InstSeqNum checkInst(Addr pc, InstSeqNum seq_num, bool is_load) override;
    void issued(Addr issued_pc, InstSeqNum issued_seq_num,
                bool is_store) override;
    void commitInstruction(InstSeqNum seq_num, bool is_load,
                           bool stlf_forwarded,
                           InstSeqNum forwarding_store_seq,
                           bool prediction_validated,
                           bool prediction_correct) override;
    void resolveBranch(InstSeqNum seq_num, bool taken, Addr target) override;
    void squash(InstSeqNum squashed_num, ThreadID tid) override;
    void clear() override;
    void dump() override;

    uint64_t
    lastTableLookups() const override
    {
        return tableLookups;
    }
    uint64_t
    lastAllocations() const override
    {
        return allocations;
    }
    uint64_t
    lastConfidenceUpdates() const override
    {
        return usefulUpdates;
    }
    uint64_t
    lastDistanceOutOfRange() const override
    {
        return distanceOutOfRange;
    }
    uint64_t
    lastNoTrackedStores() const override
    {
        return noTrackedStores;
    }
    uint64_t
    lastDistanceUnderflows() const override
    {
        return distanceUnderflows;
    }
    uint64_t
    lastTargetOrdinalMissing() const override
    {
        return targetOrdinalMissing;
    }
    uint64_t
    lastTrainingDistanceOverflows() const override
    {
        return trainingDistanceOverflows;
    }
    bool
    hasLastStoreIdDelta() const override
    {
        return trainingStoreIdDelta.has_value();
    }
    uint64_t
    lastStoreIdDelta() const override
    {
        return trainingStoreIdDelta.value_or(0);
    }

  private:
    struct BranchRecord
    {
        InstSeqNum seqNum = 0;
        bool indirect = false;
        bool taken = false;
        Addr target = 0;
    };

    struct Entry
    {
        bool valid = false;
        bool useful = false;
        uint32_t tag = 0;
        uint8_t distance = 0;
    };

    struct Table
    {
        unsigned historyLength = 0;
        std::vector<Entry> entries;
    };

    struct Prediction
    {
        bool valid = false;
        unsigned table = 0;
        unsigned index = 0;
        uint32_t tag = 0;
        uint8_t distance = 0;
        InstSeqNum storeSeqNum = 0;
    };

    struct MemoryContext
    {
        Addr pc = 0;
        std::vector<uint64_t> histories;
        std::optional<uint64_t> storeOrdinal;
        std::optional<uint64_t> lastStoreOrdinal;
        Prediction prediction;
    };

    struct PendingViolation
    {
        InstSeqNum originalLoadSeqNum = 0;
        std::optional<InstSeqNum> replaySeqNum;
        MemoryContext storeContext;
        MemoryContext loadContext;
    };

    static constexpr unsigned FoldedHistoryBits = 63;

    uint64_t branchValue(const BranchRecord &branch) const;
    void appendBranch(const BranchRecord &branch);
    void rebuildHistories();
    unsigned index(unsigned table, Addr pc, uint64_t history) const;
    uint32_t tag(unsigned table, Addr pc, uint64_t history) const;
    bool allocate(MemoryContext &load_context, uint64_t distance,
                  unsigned first_table);
    void train(MemoryContext &store_context, MemoryContext &load_context);
    void ageUsefulBits();
    bool decaySelected();
    uint64_t nextRandom();
    void resetLastOperation();

    std::vector<unsigned> historyLengths;
    std::vector<unsigned> tableEntries;
    std::vector<unsigned> tagBits;
    unsigned distanceBits;
    uint64_t usefulResetPeriod;
    unsigned falseDecayLog2;
    unsigned maxHistory;
    std::vector<uint32_t> tagMasks;
    uint8_t maxDistance;

    std::vector<Table> tables;
    std::vector<uint64_t> currentHistories;
    std::deque<BranchRecord> branchHistory;
    uint64_t predictorAccesses = 0;
    uint64_t randomState = 0x9e3779b97f4a7c15ULL;
    uint64_t nextStoreOrdinal = 0;
    std::map<InstSeqNum, MemoryContext> memoryContexts;
    std::map<Addr, PendingViolation> pendingViolations;
    std::map<uint64_t, InstSeqNum> storesByOrdinal;
    std::map<InstSeqNum, uint64_t> storeOrdinals;

    uint64_t tableLookups = 0;
    uint64_t allocations = 0;
    uint64_t usefulUpdates = 0;
    uint64_t distanceOutOfRange = 0;
    uint64_t noTrackedStores = 0;
    uint64_t distanceUnderflows = 0;
    uint64_t targetOrdinalMissing = 0;
    uint64_t trainingDistanceOverflows = 0;
    std::optional<uint64_t> trainingStoreIdDelta;
};

} // namespace po3
} // namespace gem5

#endif // __CPU_PO3_MDP_TAGE_HH__
