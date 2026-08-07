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

#ifndef __CPU_PO3_SEGMENTED_COUNTING_BLOOM_FILTER_HH__
#define __CPU_PO3_SEGMENTED_COUNTING_BLOOM_FILTER_HH__

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string_view>
#include <vector>

#include "cpu/po3/mem_dep_predictor.hh"

namespace gem5
{

namespace po3
{

/**
 * A segmented counting Bloom-filter memory-dependence predictor.
 *
 * Each observed (older store PC, younger load PC) violation is inserted in a
 * rolling history. Each segment supplies one independent hash and maintains
 * a counter array plus a decoupled presence bit-vector. Removing the oldest
 * history item decrements all of its counters, so a bit is cleared exactly
 * when its counter reaches zero.
 *
 * A lookup tests the load PC against unissued stores from youngest to oldest.
 * A Bloom-positive pair conservatively predicts a dependence on that store.
 */
class SegmentedCountingBloomFilter : public MemDepPredictor
{
  public:
    SegmentedCountingBloomFilter(std::string_view name, uint64_t clear_period,
                                 unsigned num_segments,
                                 unsigned entries_per_segment,
                                 unsigned history_entries);

    void violation(Addr store_pc, Addr load_pc) override;
    void insertLoad(Addr load_pc, InstSeqNum load_seq_num) override;
    void insertStore(Addr store_pc, InstSeqNum store_seq_num,
                     ThreadID tid) override;
    InstSeqNum checkInst(Addr load_pc) override;
    void issued(Addr issued_pc, InstSeqNum issued_seq_num,
                bool is_store) override;
    void squash(InstSeqNum squashed_num, ThreadID tid) override;
    void clear() override;
    void dump() override;

    uint64_t
    lastCandidateChecks() const override
    {
        return candidateChecks;
    }

    uint64_t
    lastFilterPositivePairs() const override
    {
        return filterPositivePairs;
    }

  private:
    using StoreMap = std::map<InstSeqNum, Addr, std::greater<InstSeqNum>>;

    static uint64_t mix(uint64_t value);
    static uint64_t pairSignature(Addr store_pc, Addr load_pc);

    unsigned index(uint64_t signature, unsigned segment) const;
    bool contains(uint64_t signature) const;
    void add(uint64_t signature);
    void remove(uint64_t signature);
    void checkClear();

    uint64_t clearPeriod;
    uint64_t memOpsPred = 0;
    unsigned numSegments;
    unsigned entriesPerSegment;
    unsigned historyEntries;

    std::vector<std::vector<uint32_t>> counters;
    std::vector<std::vector<bool>> presenceBits;
    std::deque<uint64_t> history;
    StoreMap inFlightStores;

    uint64_t candidateChecks = 0;
    uint64_t filterPositivePairs = 0;
};

} // namespace po3
} // namespace gem5

#endif // __CPU_PO3_SEGMENTED_COUNTING_BLOOM_FILTER_HH__
