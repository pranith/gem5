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

#include "cpu/po3/segmented_counting_bloom_filter.hh"

#include <algorithm>

#include "base/cprintf.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/MemDepUnit.hh"

namespace gem5
{

namespace po3
{

SegmentedCountingBloomFilter::SegmentedCountingBloomFilter(
    std::string_view name, uint64_t clear_period, unsigned num_segments,
    unsigned entries_per_segment, unsigned history_entries)
    : MemDepPredictor(name),
      clearPeriod(clear_period),
      numSegments(num_segments),
      entriesPerSegment(entries_per_segment),
      historyEntries(history_entries),
      counters(numSegments, std::vector<uint32_t>(entriesPerSegment, 0)),
      presenceBits(numSegments, std::vector<bool>(entriesPerSegment, false))
{
    fatal_if(numSegments == 0, "SCBF must have at least one segment");
    fatal_if(!isPowerOf2(entriesPerSegment),
             "SCBF entries per segment must be a power of two");
    fatal_if(historyEntries == 0,
             "SCBF violation history must contain at least one entry");
    fatal_if(clearPeriod == 0, "SCBF clear period must be nonzero");
}

uint64_t
SegmentedCountingBloomFilter::mix(uint64_t value)
{
    // SplitMix64's finalizer provides inexpensive, independent-looking bits.
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

uint64_t
SegmentedCountingBloomFilter::pairSignature(Addr store_pc, Addr load_pc)
{
    const uint64_t store = store_pc >> 2;
    const uint64_t load = load_pc >> 2;
    return mix(store ^
               (load + 0x9e3779b97f4a7c15ULL + (store << 6) + (store >> 2)));
}

unsigned
SegmentedCountingBloomFilter::index(uint64_t signature, unsigned segment) const
{
    const uint64_t seed =
        0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(segment + 1);
    return mix(signature ^ seed) & (entriesPerSegment - 1);
}

bool
SegmentedCountingBloomFilter::contains(uint64_t signature) const
{
    for (unsigned segment = 0; segment < numSegments; ++segment) {
        if (!presenceBits[segment][index(signature, segment)]) {
            return false;
        }
    }
    return true;
}

void
SegmentedCountingBloomFilter::add(uint64_t signature)
{
    for (unsigned segment = 0; segment < numSegments; ++segment) {
        const unsigned idx = index(signature, segment);
        ++counters[segment][idx];
        presenceBits[segment][idx] = true;
    }
}

void
SegmentedCountingBloomFilter::remove(uint64_t signature)
{
    for (unsigned segment = 0; segment < numSegments; ++segment) {
        const unsigned idx = index(signature, segment);
        panic_if(counters[segment][idx] == 0,
                 "SCBF counter underflow in segment %u, entry %u", segment,
                 idx);
        --counters[segment][idx];
        if (counters[segment][idx] == 0) {
            presenceBits[segment][idx] = false;
        }
    }
}

void
SegmentedCountingBloomFilter::violation(Addr store_pc, Addr load_pc)
{
    const uint64_t signature = pairSignature(store_pc, load_pc);
    if (history.size() == historyEntries) {
        remove(history.front());
        history.pop_front();
    }
    add(signature);
    history.push_back(signature);

    DPRINTF(MemDepUnit,
            "SCBF trained store PC %#x -> load PC %#x; history=%u\n", store_pc,
            load_pc, history.size());
}

void
SegmentedCountingBloomFilter::checkClear()
{
    ++memOpsPred;
    if (memOpsPred > clearPeriod) {
        DPRINTF(MemDepUnit,
                "SCBF clearing after %llu predicted memory operations\n",
                clearPeriod);
        clear();
    }
}

void
SegmentedCountingBloomFilter::insertLoad(Addr load_pc, InstSeqNum load_seq_num)
{
    checkClear();
}

void
SegmentedCountingBloomFilter::insertStore(Addr store_pc,
                                          InstSeqNum store_seq_num,
                                          ThreadID tid)
{
    checkClear();
    inFlightStores[store_seq_num] = store_pc;
}

InstSeqNum
SegmentedCountingBloomFilter::checkInst(Addr load_pc)
{
    candidateChecks = 0;
    filterPositivePairs = 0;

    for (const auto &[seq_num, store_pc] : inFlightStores) {
        ++candidateChecks;
        if (contains(pairSignature(store_pc, load_pc))) {
            ++filterPositivePairs;
            DPRINTF(MemDepUnit,
                    "SCBF predicts load PC %#x depends on store PC %#x "
                    "[sn:%llu]\n",
                    load_pc, store_pc, seq_num);
            return seq_num;
        }
    }

    return 0;
}

void
SegmentedCountingBloomFilter::issued(Addr issued_pc, InstSeqNum issued_seq_num,
                                     bool is_store)
{
    if (is_store) {
        inFlightStores.erase(issued_seq_num);
    }
}

void
SegmentedCountingBloomFilter::squash(InstSeqNum squashed_num, ThreadID tid)
{
    auto it = inFlightStores.begin();
    while (it != inFlightStores.end() && it->first > squashed_num) {
        it = inFlightStores.erase(it);
    }
}

void
SegmentedCountingBloomFilter::clear()
{
    for (auto &segment : counters) {
        std::fill(segment.begin(), segment.end(), 0);
    }
    for (auto &segment : presenceBits) {
        std::fill(segment.begin(), segment.end(), false);
    }
    history.clear();
    inFlightStores.clear();
    memOpsPred = 0;
    candidateChecks = 0;
    filterPositivePairs = 0;
}

void
SegmentedCountingBloomFilter::dump()
{
    cprintf("SCBF history entries: %u/%u\n", history.size(), historyEntries);
    cprintf("SCBF in-flight stores: %u\n", inFlightStores.size());
}

} // namespace po3
} // namespace gem5
