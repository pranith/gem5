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

#include "cpu/po3/mdp_tage.hh"

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

MDPTage::MDPTage(std::string_view name,
                 const std::vector<unsigned> &history_lengths,
                 const std::vector<unsigned> &table_entries,
                 const std::vector<unsigned> &tag_bits, unsigned distance_bits,
                 uint64_t useful_reset_period, unsigned false_decay_log2)
    : MemDepPredictor(name),
      historyLengths(history_lengths),
      tableEntries(table_entries),
      tagBits(tag_bits),
      distanceBits(distance_bits),
      usefulResetPeriod(useful_reset_period),
      falseDecayLog2(false_decay_log2),
      maxHistory(historyLengths.empty() ? 0 : historyLengths.back()),
      maxDistance(distance_bits >= 8 ? UINT8_MAX : (1U << distance_bits) - 1)
{
    fatal_if(historyLengths.empty(),
             "MDP-TAGE needs at least one tagged component");
    fatal_if(historyLengths.size() != tableEntries.size(),
             "MDP-TAGE history and table-size vectors must have equal size");
    fatal_if(historyLengths.size() != tagBits.size(),
             "MDP-TAGE history and tag-width vectors must have equal size");
    fatal_if(historyLengths.front() == 0,
             "MDP-TAGE's shortest tagged history must be nonzero");
    fatal_if(
        !std::is_sorted(historyLengths.begin(), historyLengths.end()) ||
            std::adjacent_find(historyLengths.begin(), historyLengths.end()) !=
                historyLengths.end(),
        "MDP-TAGE history lengths must be strictly increasing");
    fatal_if(distanceBits == 0 || distanceBits > 8,
             "MDP-TAGE distance width must be between 1 and 8 bits");
    fatal_if(falseDecayLog2 >= 64,
             "MDP-TAGE false-decay log2 must be less than 64");

    for (unsigned i = 0; i < historyLengths.size(); ++i) {
        fatal_if(!isPowerOf2(tableEntries[i]),
                 "MDP-TAGE component sizes must be powers of two");
        fatal_if(tagBits[i] == 0 || tagBits[i] > 31,
                 "MDP-TAGE tag widths must be between 1 and 31 bits");
        Table table;
        table.historyLength = historyLengths[i];
        table.entries.resize(tableEntries[i]);
        tables.push_back(std::move(table));
        tagMasks.push_back((1U << tagBits[i]) - 1);
    }
    currentHistories.assign(tables.size(), 0);
}

uint64_t
MDPTage::branchValue(const BranchRecord &branch) const
{
    if (branch.indirect) {
        return ((branch.target & 0x1f) << 2) |
               (static_cast<uint64_t>(branch.taken) << 1) | 1;
    }
    return static_cast<uint64_t>(branch.taken) << 1;
}

void
MDPTage::appendBranch(const BranchRecord &branch)
{
    constexpr uint64_t history_mask = (1ULL << FoldedHistoryBits) - 1;
    const uint64_t incoming = branchValue(branch) & history_mask;
    for (unsigned i = 0; i < tables.size(); ++i) {
        const unsigned length = tables[i].historyLength;
        const uint64_t outgoing =
            branchHistory.size() >= length
                ? branchValue(branchHistory[branchHistory.size() - length]) &
                      history_mask
                : 0;
        uint64_t folded = (currentHistories[i] << 1) ^ incoming;
        const unsigned rotation = length % FoldedHistoryBits;
        const uint64_t rotated_outgoing =
            ((outgoing << rotation) |
             (outgoing >> (FoldedHistoryBits - rotation))) &
            history_mask;
        folded ^= rotated_outgoing;
        folded ^= folded >> FoldedHistoryBits;
        currentHistories[i] = folded & history_mask;
    }

    branchHistory.push_back(branch);
    if (branchHistory.size() > maxHistory) {
        branchHistory.pop_front();
    }
}

void
MDPTage::rebuildHistories()
{
    std::deque<BranchRecord> records = branchHistory;
    branchHistory.clear();
    std::fill(currentHistories.begin(), currentHistories.end(), 0);
    for (const BranchRecord &record : records) {
        appendBranch(record);
    }
}

unsigned
MDPTage::index(unsigned table, Addr pc, uint64_t history) const
{
    const unsigned index_bits = floorLog2(tableEntries[table]);
    const unsigned folded_bits = index_bits + tagBits[table];
    const uint64_t folded_mask = (1ULL << folded_bits) - 1;
    while (history >> folded_bits) {
        history = (history & folded_mask) ^ (history >> folded_bits);
    }
    const uint64_t pc_hash = pc ^ (pc >> 2) ^ (pc >> 5);
    return (pc_hash ^ history) & (tableEntries[table] - 1);
}

uint32_t
MDPTage::tag(unsigned table, Addr pc, uint64_t history) const
{
    const unsigned index_bits = floorLog2(tableEntries[table]);
    const unsigned folded_bits = index_bits + tagBits[table];
    const uint64_t folded_mask = (1ULL << folded_bits) - 1;
    while (history >> folded_bits) {
        history = (history & folded_mask) ^ (history >> folded_bits);
    }
    const uint64_t pc_hash = pc ^ (pc >> 3) ^ (pc >> 7);
    return (pc_hash ^ (history >> index_bits)) & tagMasks[table];
}

void
MDPTage::resetLastOperation()
{
    tableLookups = 0;
    allocations = 0;
    usefulUpdates = 0;
    distanceOutOfRange = 0;
    noTrackedStores = 0;
    distanceUnderflows = 0;
    targetOrdinalMissing = 0;
    trainingDistanceOverflows = 0;
    trainingStoreIdDelta.reset();
}

uint64_t
MDPTage::nextRandom()
{
    randomState ^= randomState >> 12;
    randomState ^= randomState << 25;
    randomState ^= randomState >> 27;
    return randomState * 0x2545f4914f6cdd1dULL;
}

bool
MDPTage::decaySelected()
{
    if (falseDecayLog2 == 0) {
        return true;
    }
    const uint64_t mask = (1ULL << falseDecayLog2) - 1;
    return (nextRandom() & mask) == 0;
}

void
MDPTage::ageUsefulBits()
{
    for (Table &table : tables) {
        for (Entry &entry : table.entries) {
            if (entry.useful) {
                entry.useful = false;
                ++usefulUpdates;
            }
        }
    }
}

void
MDPTage::observeInstruction(const MemDepPredInstruction &inst)
{
    if (inst.isLoad || inst.isStore) {
        MemoryContext context;
        context.pc = inst.pc;
        context.histories = currentHistories;
        context.lastStoreOrdinal =
            storesByOrdinal.empty() ? std::optional<uint64_t>()
                                    : std::prev(storesByOrdinal.end())->first;

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
        appendBranch({inst.seqNum, inst.isIndirect, inst.predictedTaken,
                      inst.predictedTarget});
    }
}

InstSeqNum
MDPTage::checkInst(Addr pc, InstSeqNum seq_num, bool is_load)
{
    resetLastOperation();
    if (!is_load) {
        return 0;
    }

    ++predictorAccesses;
    if (usefulResetPeriod != 0 && predictorAccesses % usefulResetPeriod == 0) {
        ageUsefulBits();
    }

    auto context_it = memoryContexts.find(seq_num);
    if (context_it == memoryContexts.end()) {
        return 0;
    }
    MemoryContext &context = context_it->second;

    for (int table_idx = static_cast<int>(tables.size()) - 1; table_idx >= 0;
         --table_idx) {
        const unsigned table = table_idx;
        const unsigned entry_idx = index(table, pc, context.histories[table]);
        const uint32_t wanted_tag = tag(table, pc, context.histories[table]);
        ++tableLookups;
        Entry &entry = tables[table].entries[entry_idx];
        if (!entry.valid || !entry.useful || entry.tag != wanted_tag) {
            continue;
        }

        context.prediction = {true,       table,          entry_idx,
                              wanted_tag, entry.distance, 0};
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
                "MDP-TAGE predicts load PC %#x [sn:%llu] depends on store "
                "[sn:%llu], distance=%u, history=%u\n",
                pc, seq_num, store_it->second, entry.distance,
                tables[table].historyLength);
        return store_it->second;
    }

    return 0;
}

bool
MDPTage::allocate(MemoryContext &load_context, uint64_t distance,
                  unsigned first_table)
{
    if (first_table >= tables.size()) {
        return false;
    }

    for (unsigned table = first_table; table < tables.size(); ++table) {
        const unsigned entry_idx =
            index(table, load_context.pc, load_context.histories[table]);
        const uint32_t wanted_tag =
            tag(table, load_context.pc, load_context.histories[table]);
        Entry &entry = tables[table].entries[entry_idx];
        if (entry.valid && entry.useful) {
            continue;
        }

        entry.valid = true;
        entry.useful = true;
        entry.tag = wanted_tag;
        entry.distance = static_cast<uint8_t>(distance);
        ++allocations;
        ++usefulUpdates;
        DPRINTF(MemDepUnit,
                "MDP-TAGE trains load PC %#x: distance=%u, history=%u\n",
                load_context.pc, distance, tables[table].historyLength);
        return true;
    }

    // Standard TAGE allocation pressure handling: make the indexed longer
    // history candidates replaceable, then let a later violation allocate.
    for (unsigned table = first_table; table < tables.size(); ++table) {
        const unsigned entry_idx =
            index(table, load_context.pc, load_context.histories[table]);
        Entry &entry = tables[table].entries[entry_idx];
        if (entry.useful) {
            entry.useful = false;
            ++usefulUpdates;
        }
    }
    return false;
}

void
MDPTage::train(MemoryContext &store_context, MemoryContext &load_context)
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

    const unsigned first_table =
        load_context.prediction.valid ? load_context.prediction.table + 1 : 0;
    allocate(load_context, distance, first_table);
}

void
MDPTage::violation(Addr store_pc, Addr load_pc, InstSeqNum store_seq_num,
                   InstSeqNum load_seq_num)
{
    resetLastOperation();
    auto store_it = memoryContexts.find(store_seq_num);
    auto load_it = memoryContexts.find(load_seq_num);
    if (store_it == memoryContexts.end() || load_it == memoryContexts.end()) {
        return;
    }

    PendingViolation pending;
    pending.originalLoadSeqNum = load_seq_num;
    pending.storeContext = store_it->second;
    pending.loadContext = load_it->second;
    pendingViolations[load_pc] = std::move(pending);
}

void
MDPTage::insertLoad(Addr load_pc, InstSeqNum load_seq_num)
{}

void
MDPTage::insertStore(Addr store_pc, InstSeqNum store_seq_num, ThreadID tid)
{}

void
MDPTage::issued(Addr issued_pc, InstSeqNum issued_seq_num, bool is_store)
{}

void
MDPTage::commitInstruction(InstSeqNum seq_num, bool is_load,
                           bool stlf_forwarded,
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
            const Prediction &prediction = context.prediction;
            Entry &entry = tables[prediction.table].entries[prediction.index];
            if (entry.valid && entry.tag == prediction.tag) {
                const bool forwarded_from_prediction =
                    stlf_forwarded && prediction.storeSeqNum != 0 &&
                    prediction.storeSeqNum == forwarding_store_seq;
                const bool forwarded_from_different_store =
                    stlf_forwarded && prediction.storeSeqNum != 0 &&
                    prediction.storeSeqNum != forwarding_store_seq;
                if (prediction_correct || forwarded_from_prediction) {
                    if (!entry.useful) {
                        entry.useful = true;
                        ++usefulUpdates;
                    }
                } else if ((prediction_validated ||
                            forwarded_from_different_store) &&
                           entry.useful && decaySelected()) {
                    // Perais and Seznec probabilistically forget a false
                    // dependency with probability 1/256 by clearing u.
                    entry.useful = false;
                    ++usefulUpdates;
                }
            }
        }
        memoryContexts.erase(context_it);
    }

    if (pending_to_train != pendingViolations.end()) {
        PendingViolation &pending = pending_to_train->second;
        train(pending.storeContext, pending.loadContext);
        pendingViolations.erase(pending_to_train);
    }

    auto ordinal_it = storeOrdinals.find(seq_num);
    if (ordinal_it != storeOrdinals.end()) {
        storesByOrdinal.erase(ordinal_it->second);
        storeOrdinals.erase(ordinal_it);
    }
}

void
MDPTage::resolveBranch(InstSeqNum seq_num, bool taken, Addr target)
{
    for (auto it = branchHistory.rbegin(); it != branchHistory.rend(); ++it) {
        if (it->seqNum == seq_num) {
            if (it->taken != taken || (it->indirect && it->target != target)) {
                it->taken = taken;
                it->target = target;
                rebuildHistories();
            }
            return;
        }
        if (it->seqNum < seq_num) {
            return;
        }
    }
}

void
MDPTage::squash(InstSeqNum squashed_num, ThreadID tid)
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
        if (waiting_seq == squashed_num + 1) {
            pending.replaySeqNum.reset();
            ++pending_it;
        } else {
            pending_it = pendingViolations.erase(pending_it);
        }
    }

    bool removed_branch = false;
    while (!branchHistory.empty() &&
           branchHistory.back().seqNum > squashed_num) {
        branchHistory.pop_back();
        removed_branch = true;
    }
    if (removed_branch) {
        rebuildHistories();
    }

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
             "MDP-TAGE store ordinal underflow");
    nextStoreOrdinal -= removed_stores;
}

void
MDPTage::clear()
{
    for (Table &table : tables) {
        std::fill(table.entries.begin(), table.entries.end(), Entry{});
    }
    std::fill(currentHistories.begin(), currentHistories.end(), 0);
    branchHistory.clear();
    predictorAccesses = 0;
    randomState = 0x9e3779b97f4a7c15ULL;
    nextStoreOrdinal = 0;
    memoryContexts.clear();
    pendingViolations.clear();
    storesByOrdinal.clear();
    storeOrdinals.clear();
    resetLastOperation();
}

void
MDPTage::dump()
{
    uint64_t valid_entries = 0;
    uint64_t useful_entries = 0;
    uint64_t total_entries = 0;
    for (const Table &table : tables) {
        total_entries += table.entries.size();
        for (const Entry &entry : table.entries) {
            valid_entries += entry.valid;
            useful_entries += entry.valid && entry.useful;
        }
    }
    cprintf("MDP-TAGE valid entries: %u/%u, useful: %u\n", valid_entries,
            total_entries, useful_entries);
    cprintf("MDP-TAGE branch history: %u, memory contexts: %u, stores: %u\n",
            branchHistory.size(), memoryContexts.size(),
            storesByOrdinal.size());
}

} // namespace po3
} // namespace gem5
