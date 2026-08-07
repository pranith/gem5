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

#include "cpu/po3/phast.hh"

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

PHAST::PHAST(std::string_view name,
             const std::vector<unsigned> &history_lengths, unsigned num_sets,
             unsigned associativity, unsigned tag_bits, unsigned distance_bits,
             unsigned confidence_bits)
    : MemDepPredictor(name),
      historyLengths(history_lengths),
      numSets(num_sets),
      associativity(associativity),
      tagBits(tag_bits),
      distanceBits(distance_bits),
      confidenceBits(confidence_bits),
      maxHistory(historyLengths.empty() ? 0 : historyLengths.back()),
      indexBits(num_sets > 0 ? floorLog2(num_sets) : 0),
      foldedBits(indexBits + tagBits),
      tagMask(tag_bits >= 32 ? UINT32_MAX : (1U << tag_bits) - 1),
      maxDistance(distance_bits >= 8 ? UINT8_MAX : (1U << distance_bits) - 1),
      maxConfidence(confidence_bits >= 8 ? UINT8_MAX
                                         : (1U << confidence_bits) - 1)
{
    fatal_if(historyLengths.empty(), "PHAST needs at least one history table");
    fatal_if(historyLengths.front() != 0,
             "PHAST history lengths must begin with zero");
    fatal_if(
        !std::is_sorted(historyLengths.begin(), historyLengths.end()) ||
            std::adjacent_find(historyLengths.begin(), historyLengths.end()) !=
                historyLengths.end(),
        "PHAST history lengths must be strictly increasing");
    fatal_if(!isPowerOf2(numSets),
             "PHAST number of sets must be a power of two");
    fatal_if(associativity == 0, "PHAST associativity must be nonzero");
    fatal_if(tagBits == 0 || tagBits > 31,
             "PHAST tag width must be between 1 and 31 bits");
    fatal_if(foldedBits >= 64, "PHAST folded history must fit in 63 bits");
    fatal_if(distanceBits == 0 || distanceBits > 8,
             "PHAST distance width must be between 1 and 8 bits");
    fatal_if(confidenceBits == 0 || confidenceBits > 8,
             "PHAST confidence width must be between 1 and 8 bits");

    for (unsigned length : historyLengths) {
        Table table;
        table.historyLength = length;
        table.sets.assign(numSets, std::vector<Entry>(associativity));
        for (auto &set : table.sets) {
            for (unsigned way = 0; way < associativity; ++way) {
                set[way].lru = way;
            }
        }
        tables.push_back(std::move(table));
    }
}

uint64_t
PHAST::mix(uint64_t value)
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

uint64_t
PHAST::foldedHistory(const MemoryContext &context,
                     unsigned history_length) const
{
    if (history_length == 0 || context.history.empty()) {
        return 0;
    }

    const unsigned length =
        std::min<unsigned>(history_length, context.history.size());
    const unsigned first = context.history.size() - length;
    uint64_t folded = 0;
    for (unsigned pos = 0; pos < length; ++pos) {
        const BranchValue &branch = context.history[first + pos];
        uint8_t value;
        if (pos == 0 || branch.indirect) {
            value = (branch.target << 2) |
                    (static_cast<uint8_t>(branch.taken) << 1) |
                    static_cast<uint8_t>(branch.indirect);
        } else {
            value = static_cast<uint8_t>(branch.taken) << 1;
        }
        folded ^= mix(static_cast<uint64_t>(value) |
                      (static_cast<uint64_t>(pos) << 8));
    }

    const uint64_t folded_mask = (1ULL << foldedBits) - 1;
    while (folded >> foldedBits) {
        folded = (folded & folded_mask) ^ (folded >> foldedBits);
    }
    return folded & folded_mask;
}

unsigned
PHAST::index(Addr load_pc, uint64_t history) const
{
    const uint64_t pc_hash = load_pc ^ (load_pc >> 2) ^ (load_pc >> 5);
    return (pc_hash ^ history) & (numSets - 1);
}

uint32_t
PHAST::tag(Addr load_pc, uint64_t history) const
{
    const uint64_t pc_hash = load_pc ^ (load_pc >> 3) ^ (load_pc >> 7);
    return (pc_hash ^ (history >> indexBits)) & tagMask;
}

unsigned
PHAST::selectedTable(unsigned path_length) const
{
    const unsigned truncated = std::min(path_length, maxHistory);
    auto upper = std::upper_bound(historyLengths.begin(), historyLengths.end(),
                                  truncated);
    return std::distance(historyLengths.begin(), upper) - 1;
}

int
PHAST::findWay(const Table &table, unsigned set, uint32_t wanted_tag) const
{
    for (unsigned way = 0; way < associativity; ++way) {
        const Entry &entry = table.sets[set][way];
        if (entry.valid && entry.tag == wanted_tag) {
            return way;
        }
    }
    return -1;
}

unsigned
PHAST::victimWay(const Table &table, unsigned set) const
{
    for (unsigned way = 0; way < associativity; ++way) {
        if (!table.sets[set][way].valid) {
            return way;
        }
    }

    unsigned victim = 0;
    for (unsigned way = 1; way < associativity; ++way) {
        if (table.sets[set][way].lru > table.sets[set][victim].lru) {
            victim = way;
        }
    }
    return victim;
}

void
PHAST::touch(Table &table, unsigned set, unsigned way)
{
    const unsigned old_lru = table.sets[set][way].lru;
    for (auto &entry : table.sets[set]) {
        if (entry.valid && entry.lru < old_lru) {
            ++entry.lru;
        }
    }
    table.sets[set][way].lru = 0;
}

void
PHAST::resetLastOperation()
{
    tableLookups = 0;
    allocations = 0;
    confidenceUpdates = 0;
    distanceOutOfRange = 0;
    noTrackedStores = 0;
    distanceUnderflows = 0;
    targetOrdinalMissing = 0;
    trainingDistanceOverflows = 0;
    trainingStoreIdDelta.reset();
}

void
PHAST::observeInstruction(const MemDepPredInstruction &inst)
{
    if (inst.isLoad || inst.isStore) {
        MemoryContext context;
        context.pc = inst.pc;
        context.branchCount = branchCount;
        context.lastStoreOrdinal =
            storesByOrdinal.empty() ? std::optional<uint64_t>()
                                    : std::prev(storesByOrdinal.end())->first;
        const unsigned retained =
            std::min<unsigned>(maxHistory, branchHistory.size());
        context.history.reserve(retained);
        auto begin = branchHistory.end() - retained;
        for (auto it = begin; it != branchHistory.end(); ++it) {
            context.history.push_back(*it);
        }

        if (inst.isStore) {
            context.storeOrdinal = nextStoreOrdinal;
            storesByOrdinal[nextStoreOrdinal] = inst.seqNum;
            storeOrdinals[inst.seqNum] = nextStoreOrdinal;
            ++nextStoreOrdinal;
        }
        memoryContexts[inst.seqNum] = std::move(context);

        if (inst.isLoad) {
            auto pending_it = pendingViolations.find(inst.pc);
            if (pending_it != pendingViolations.end() &&
                !pending_it->second.replaySeqNum) {
                pending_it->second.replaySeqNum = inst.seqNum;
            }
        }
    }

    if (inst.isControl && (inst.isConditional || inst.isIndirect)) {
        BranchRecord branch;
        branch.seqNum = inst.seqNum;
        branch.indirect = inst.isIndirect;
        branch.taken = inst.predictedTaken;
        branch.target = inst.predictedTarget & 0x1f;
        branchHistory.push_back(branch);
        ++branchCount;
    }
}

InstSeqNum
PHAST::checkInst(Addr pc, InstSeqNum seq_num, bool is_load)
{
    resetLastOperation();
    if (!is_load) {
        return 0;
    }

    auto context_it = memoryContexts.find(seq_num);
    if (context_it == memoryContexts.end()) {
        return 0;
    }
    MemoryContext &context = context_it->second;

    for (int table_idx = static_cast<int>(tables.size()) - 1; table_idx >= 0;
         --table_idx) {
        Table &table = tables[table_idx];
        const uint64_t history = foldedHistory(context, table.historyLength);
        const unsigned set = index(pc, history);
        const uint32_t wanted_tag = tag(pc, history);
        ++tableLookups;
        const int way = findWay(table, set, wanted_tag);
        if (way < 0 || table.sets[set][way].confidence == 0) {
            continue;
        }

        Entry &entry = table.sets[set][way];
        touch(table, set, way);
        context.prediction = {true,       static_cast<unsigned>(table_idx),
                              set,        static_cast<unsigned>(way),
                              wanted_tag, entry.distance,
                              0};

        if (!context.lastStoreOrdinal) {
            ++noTrackedStores;
            ++distanceOutOfRange;
            return 0;
        }
        if (entry.distance > *context.lastStoreOrdinal) {
            ++distanceUnderflows;
            ++distanceOutOfRange;
            return 0;
        }
        const uint64_t ordinal = *context.lastStoreOrdinal - entry.distance;
        auto store_it = storesByOrdinal.find(ordinal);
        if (store_it == storesByOrdinal.end()) {
            ++targetOrdinalMissing;
            ++distanceOutOfRange;
            return 0;
        }

        context.prediction.storeSeqNum = store_it->second;
        DPRINTF(MemDepUnit,
                "PHAST predicts load PC %#x [sn:%llu] depends on store "
                "[sn:%llu], distance=%u, history=%u\n",
                pc, seq_num, store_it->second, entry.distance,
                table.historyLength);
        return store_it->second;
    }

    return 0;
}

void
PHAST::penalizePrediction(MemoryContext &context)
{
    const Prediction &prediction = context.prediction;
    if (!prediction.valid) {
        return;
    }
    Table &table = tables[prediction.table];
    Entry &entry = table.sets[prediction.set][prediction.way];
    if (!entry.valid || entry.tag != prediction.tag) {
        return;
    }
    if (entry.confidence > 0) {
        --entry.confidence;
        ++confidenceUpdates;
    }
}

void
PHAST::train(MemoryContext &store_context, MemoryContext &load_context)
{
    if (!store_context.storeOrdinal || !load_context.lastStoreOrdinal ||
        *store_context.storeOrdinal > *load_context.lastStoreOrdinal) {
        return;
    }

    const uint64_t distance =
        *load_context.lastStoreOrdinal - *store_context.storeOrdinal;
    trainingStoreIdDelta = distance;
    if (distance > maxDistance) {
        ++trainingDistanceOverflows;
        ++distanceOutOfRange;
        return;
    }
    const uint64_t divergent_between =
        load_context.branchCount >= store_context.branchCount
            ? load_context.branchCount - store_context.branchCount
            : 0;
    const unsigned table_idx = selectedTable(divergent_between + 1);
    Table &table = tables[table_idx];
    const uint64_t history = foldedHistory(load_context, table.historyLength);
    const unsigned set = index(load_context.pc, history);
    const uint32_t wanted_tag = tag(load_context.pc, history);

    int way = findWay(table, set, wanted_tag);
    if (way < 0) {
        way = victimWay(table, set);
        ++allocations;
    }
    Entry &entry = table.sets[set][way];
    entry.valid = true;
    entry.tag = wanted_tag;
    entry.distance = static_cast<uint8_t>(distance);
    entry.confidence = maxConfidence;
    ++confidenceUpdates;
    touch(table, set, way);

    DPRINTF(MemDepUnit,
            "PHAST trains load PC %#x: distance=%u, path=%llu, history=%u\n",
            load_context.pc, distance, divergent_between + 1,
            table.historyLength);
}

void
PHAST::violation(Addr store_pc, Addr load_pc, InstSeqNum store_seq_num,
                 InstSeqNum load_seq_num)
{
    resetLastOperation();
    auto store_it = memoryContexts.find(store_seq_num);
    auto load_it = memoryContexts.find(load_seq_num);
    if (store_it == memoryContexts.end() || load_it == memoryContexts.end()) {
        return;
    }

    // PO3 eagerly squashes memory-order violations, whereas the paper's
    // preferred policy trains PHAST when a lazily squashed load reaches
    // commit. Preserve the failed contexts and train when the refetched load
    // commits, avoiding pollution from wrong-path violations.
    PendingViolation pending;
    pending.originalLoadSeqNum = load_seq_num;
    pending.storeContext = store_it->second;
    pending.loadContext = load_it->second;
    pendingViolations[load_pc] = std::move(pending);
}

void
PHAST::insertLoad(Addr load_pc, InstSeqNum load_seq_num)
{}

void
PHAST::insertStore(Addr store_pc, InstSeqNum store_seq_num, ThreadID tid)
{}

void
PHAST::issued(Addr issued_pc, InstSeqNum issued_seq_num, bool is_store)
{}

void
PHAST::commitInstruction(InstSeqNum seq_num, bool is_load, bool stlf_forwarded,
                         InstSeqNum forwarding_store_seq,
                         bool prediction_validated, bool prediction_correct)
{
    resetLastOperation();

    auto pending_to_train = pendingViolations.end();
    if (is_load) {
        for (auto pending_it = pendingViolations.begin();
             pending_it != pendingViolations.end(); ++pending_it) {
            PendingViolation &pending = pending_it->second;
            if (pending.replaySeqNum && *pending.replaySeqNum == seq_num) {
                pending_to_train = pending_it;
                break;
            }
        }
    }

    auto context_it = memoryContexts.find(seq_num);
    if (context_it != memoryContexts.end()) {
        MemoryContext &context = context_it->second;
        if (is_load && context.prediction.valid) {
            Prediction &prediction = context.prediction;
            Table &table = tables[prediction.table];
            Entry &entry = table.sets[prediction.set][prediction.way];
            if (entry.valid && entry.tag == prediction.tag) {
                const bool forwarded_from_prediction =
                    stlf_forwarded && prediction.storeSeqNum != 0 &&
                    prediction.storeSeqNum == forwarding_store_seq;
                const bool forwarded_from_different_store =
                    stlf_forwarded && prediction.storeSeqNum != 0 &&
                    prediction.storeSeqNum != forwarding_store_seq;
                if (prediction_correct || forwarded_from_prediction) {
                    entry.confidence = maxConfidence;
                    ++confidenceUpdates;
                } else if ((prediction_validated ||
                            forwarded_from_different_store) &&
                           entry.confidence > 0) {
                    // An unavailable target is not evidence that the learned
                    // distance is wrong. Only penalize a prediction whose
                    // address was checked or whose actual forwarding store is
                    // known to be different.
                    --entry.confidence;
                    ++confidenceUpdates;
                }
            }
        }
        memoryContexts.erase(context_it);
    }

    if (pending_to_train != pendingViolations.end()) {
        PendingViolation &pending = pending_to_train->second;
        penalizePrediction(pending.loadContext);
        train(pending.storeContext, pending.loadContext);
        pendingViolations.erase(pending_to_train);
    }

    auto ordinal_it = storeOrdinals.find(seq_num);
    if (ordinal_it != storeOrdinals.end()) {
        storesByOrdinal.erase(ordinal_it->second);
        storeOrdinals.erase(ordinal_it);
    }
    pruneCommittedBranches(seq_num);
}

void
PHAST::pruneCommittedBranches(InstSeqNum committed_seq_num)
{
    while (branchHistory.size() > maxHistory &&
           branchHistory.front().seqNum <= committed_seq_num) {
        branchHistory.pop_front();
    }
}

void
PHAST::resolveBranch(InstSeqNum seq_num, bool taken, Addr target)
{
    for (auto it = branchHistory.rbegin(); it != branchHistory.rend(); ++it) {
        if (it->seqNum == seq_num) {
            it->taken = taken;
            it->target = target & 0x1f;
            return;
        }
        if (it->seqNum < seq_num) {
            return;
        }
    }
}

void
PHAST::squash(InstSeqNum squashed_num, ThreadID tid)
{
    for (auto pending_it = pendingViolations.begin();
         pending_it != pendingViolations.end();) {
        PendingViolation &pending = pending_it->second;
        const InstSeqNum waiting_seq = pending.replaySeqNum
                                           ? *pending.replaySeqNum
                                           : pending.originalLoadSeqNum;
        if (waiting_seq <= squashed_num) {
            ++pending_it;
            continue;
        }

        // Retain a conflict only when the squash includes that exact load.
        // An older branch/trap squash proves the access was wrong-path.
        if (waiting_seq == squashed_num + 1) {
            pending.replaySeqNum.reset();
            ++pending_it;
        } else {
            pending_it = pendingViolations.erase(pending_it);
        }
    }

    uint64_t removed_branches = 0;
    while (!branchHistory.empty() &&
           branchHistory.back().seqNum > squashed_num) {
        branchHistory.pop_back();
        ++removed_branches;
    }
    panic_if(removed_branches > branchCount,
             "PHAST divergent branch counter underflow");
    branchCount -= removed_branches;

    auto context_it = memoryContexts.upper_bound(squashed_num);
    memoryContexts.erase(context_it, memoryContexts.end());

    uint64_t removed_stores = 0;
    auto store_it = storeOrdinals.upper_bound(squashed_num);
    while (store_it != storeOrdinals.end()) {
        storesByOrdinal.erase(store_it->second);
        store_it = storeOrdinals.erase(store_it);
        ++removed_stores;
    }
    panic_if(removed_stores > nextStoreOrdinal,
             "PHAST store ordinal underflow");
    nextStoreOrdinal -= removed_stores;
}

void
PHAST::clear()
{
    for (auto &table : tables) {
        for (auto &set : table.sets) {
            for (unsigned way = 0; way < associativity; ++way) {
                set[way] = Entry{};
                set[way].lru = way;
            }
        }
    }
    branchHistory.clear();
    branchCount = 0;
    nextStoreOrdinal = 0;
    memoryContexts.clear();
    pendingViolations.clear();
    storesByOrdinal.clear();
    storeOrdinals.clear();
    resetLastOperation();
}

void
PHAST::dump()
{
    uint64_t valid_entries = 0;
    for (const auto &table : tables) {
        for (const auto &set : table.sets) {
            valid_entries +=
                std::count_if(set.begin(), set.end(),
                              [](const Entry &entry) { return entry.valid; });
        }
    }
    cprintf("PHAST valid entries: %u/%u\n", valid_entries,
            tables.size() * numSets * associativity);
    cprintf("PHAST branch history: %u, memory contexts: %u, stores: %u\n",
            branchHistory.size(), memoryContexts.size(),
            storesByOrdinal.size());
}

} // namespace po3
} // namespace gem5
