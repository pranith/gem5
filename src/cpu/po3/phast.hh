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

#ifndef __CPU_PO3_PHAST_HH__
#define __CPU_PO3_PHAST_HH__

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
 * PatH-Aware STore-distance (PHAST) memory-dependence predictor.
 *
 * This follows Kim and Ros, HPCA 2024: a load and a folded divergent-branch
 * history access one set-associative table per selected history length. The
 * longest confident match supplies a store distance. A violation trains only
 * the table whose geometric history length is the largest not exceeding the
 * path from the conflicting store to the load, including the divergent branch
 * immediately preceding the store.
 */
class PHAST : public MemDepPredictor
{
  public:
    PHAST(std::string_view name, const std::vector<unsigned> &history_lengths,
          unsigned num_sets, unsigned associativity, unsigned tag_bits,
          unsigned distance_bits, unsigned confidence_bits);

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
        return confidenceUpdates;
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
    struct BranchValue
    {
        bool indirect = false;
        bool taken = false;
        uint8_t target = 0;
    };

    struct BranchRecord : public BranchValue
    {
        InstSeqNum seqNum = 0;
    };

    struct Entry
    {
        bool valid = false;
        uint32_t tag = 0;
        uint8_t distance = 0;
        uint8_t confidence = 0;
        unsigned lru = 0;
    };

    struct Table
    {
        unsigned historyLength = 0;
        std::vector<std::vector<Entry>> sets;
    };

    struct Prediction
    {
        bool valid = false;
        unsigned table = 0;
        unsigned set = 0;
        unsigned way = 0;
        uint32_t tag = 0;
        uint8_t distance = 0;
        InstSeqNum storeSeqNum = 0;
    };

    struct MemoryContext
    {
        Addr pc = 0;
        uint64_t branchCount = 0;
        std::vector<BranchValue> history;
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

    static uint64_t mix(uint64_t value);
    uint64_t foldedHistory(const MemoryContext &context,
                           unsigned history_length) const;
    unsigned index(Addr load_pc, uint64_t history) const;
    uint32_t tag(Addr load_pc, uint64_t history) const;
    unsigned selectedTable(unsigned path_length) const;
    int findWay(const Table &table, unsigned set, uint32_t wanted_tag) const;
    unsigned victimWay(const Table &table, unsigned set) const;
    void touch(Table &table, unsigned set, unsigned way);
    void penalizePrediction(MemoryContext &context);
    void train(MemoryContext &store_context, MemoryContext &load_context);
    void pruneCommittedBranches(InstSeqNum committed_seq_num);
    void resetLastOperation();

    std::vector<unsigned> historyLengths;
    unsigned numSets;
    unsigned associativity;
    unsigned tagBits;
    unsigned distanceBits;
    unsigned confidenceBits;
    unsigned maxHistory;
    unsigned indexBits;
    unsigned foldedBits;
    uint32_t tagMask;
    uint8_t maxDistance;
    uint8_t maxConfidence;

    std::vector<Table> tables;
    std::deque<BranchRecord> branchHistory;
    uint64_t branchCount = 0;
    uint64_t nextStoreOrdinal = 0;
    std::map<InstSeqNum, MemoryContext> memoryContexts;
    std::map<Addr, PendingViolation> pendingViolations;
    std::map<uint64_t, InstSeqNum> storesByOrdinal;
    std::map<InstSeqNum, uint64_t> storeOrdinals;

    uint64_t tableLookups = 0;
    uint64_t allocations = 0;
    uint64_t confidenceUpdates = 0;
    uint64_t distanceOutOfRange = 0;
    uint64_t noTrackedStores = 0;
    uint64_t distanceUnderflows = 0;
    uint64_t targetOrdinalMissing = 0;
    uint64_t trainingDistanceOverflows = 0;
    std::optional<uint64_t> trainingStoreIdDelta;
};

} // namespace po3
} // namespace gem5

#endif // __CPU_PO3_PHAST_HH__
