/**
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

#include "mem/cache/replacement_policies/fuzzy_drrip_rp.hh"

#include <algorithm>
#include <tuple>

#include "debug/FuzzyRepl.hh"

namespace gem5
{

namespace replacement_policy
{

FuzzyDRRIPRP::FuzzyStats::FuzzyStats(
    statistics::Group* parent, unsigned num_rrpv_bits)
  : statistics::Group(parent),
    maxRRPV((1u << num_rrpv_bits) - 1),
    ADD_STAT(rrpvSetByReset, statistics::units::Count::get(),
        "Number of reset() writes per RRPV value"),
    ADD_STAT(rrpvSetByTouch, statistics::units::Count::get(),
        "Number of touch() writes per RRPV value")
{
}

void
FuzzyDRRIPRP::FuzzyStats::regStats()
{
    statistics::Group::regStats();

    rrpvSetByReset.init(maxRRPV + 1);
    rrpvSetByTouch.init(maxRRPV + 1);
}

FuzzyDRRIPRP::FuzzyDRRIPRP(const Params &p)
  : BRRIP(p),
    psel(std::clamp(p.initial_psel, 0, pselMax)),
    useInternalPsel(p.use_internal_psel),
    fixedGlobalStateIdx(std::clamp(p.fixed_global_state_idx, 0, 7)),
    enableDuelingHotSetOverride(p.enable_dueling_hot_set_override),
    duelingHotSetOverrideThreshold(
        std::clamp(p.dueling_hot_set_override_threshold, 0, 7)),
    fuzzyStats(this, numRRPVBits)
{
}

SatCounter8&
FuzzyDRRIPRP::getSetUrgencyCounter(uint32_t set) const
{
    auto it = setUrgency.find(set);
    if (it == setUrgency.end()) {
        it = setUrgency.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(set),
            std::forward_as_tuple(urgencyCounterBits, urgencyCounterInit)).first;
    }
    return it->second;
}

int
FuzzyDRRIPRP::globalStateIdx() const
{
    if (!useInternalPsel) {
        return fixedGlobalStateIdx;
    }
    return (psel * 7) / pselMax;
}

int
FuzzyDRRIPRP::localStateIdx(uint32_t set) const
{
    const int urgency = static_cast<int>(getSetUrgencyCounter(set));
    return (urgency * 7) / urgencyCounterMax;
}

void
FuzzyDRRIPRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<FuzzyReplData> replData =
        std::static_pointer_cast<FuzzyReplData>(replacement_data);

    if (replData->hasSet) {
        SatCounter8& urgency = getSetUrgencyCounter(replData->set);
        const unsigned oldUrgency = static_cast<unsigned>(urgency);
        urgency++;
        DPRINTF(FuzzyRepl,
            "FuzzyDRRIP touch: set=%u urgency=%u->%u rrpv_before=%d\n",
            replData->set, oldUrgency, static_cast<unsigned>(urgency),
            static_cast<int>(replData->rrpv));
    } else {
        DPRINTF(FuzzyRepl,
            "FuzzyDRRIP touch: set=unknown rrpv_before=%d\n",
            static_cast<int>(replData->rrpv));
    }

    BRRIP::touch(replacement_data);
    fuzzyStats.rrpvSetByTouch[static_cast<unsigned>(replData->rrpv)]++;

    DPRINTF(FuzzyRepl, "FuzzyDRRIP touch: rrpv_after=%d\n",
        static_cast<int>(replData->rrpv));
}

void
FuzzyDRRIPRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<FuzzyReplData> replData =
        std::static_pointer_cast<FuzzyReplData>(replacement_data);

    const int gIdx = globalStateIdx();
    const unsigned urgencyBefore = replData->hasSet ?
        static_cast<unsigned>(getSetUrgencyCounter(replData->set)) :
        urgencyCounterInit;
    const int lIdx = replData->hasSet ? localStateIdx(replData->set) : 3;
    const double longInsertProb = fuzzyLut[gIdx][lIdx];
    const int oldPsel = psel;
    const double randomValue = rng->random<double>();

    // Use fuzzy probability to decide whether the insertion is "long"
    // (max-1 RRPV) or "distant" (max RRPV).
    replData->rrpv.saturate();
    const bool insertedNear = randomValue < longInsertProb;
    if (insertedNear) {
        replData->rrpv--;
    }
    replData->valid = true;
    fuzzyStats.rrpvSetByReset[static_cast<unsigned>(replData->rrpv)]++;

    // Leaky-bucket behavior: every insertion slightly leaks urgency.
    unsigned urgencyAfter = urgencyBefore;
    if (replData->hasSet) {
        SatCounter8& urgency = getSetUrgencyCounter(replData->set);
        urgency--;
        urgencyAfter = static_cast<unsigned>(urgency);
    }

    // In dueling mode, the global chooser should come from DuelingRP itself.
    if (useInternalPsel) {
        // Bias global state toward cache-friendly behavior when local state is
        // favorable, and toward thrashing otherwise.
        if (lIdx <= 3) {
            psel = std::max(0, psel - 1);
        } else {
            psel = std::min(pselMax, psel + 1);
        }
    }

    DPRINTF(FuzzyRepl,
        "FuzzyDRRIP reset: set=%u has_set=%d g_idx=%d l_idx=%d "
        "urgency=%u->%u psel=%d->%d prob=%.3f rand=%.6f insert=%s rrpv=%d\n",
        replData->set, replData->hasSet, gIdx, lIdx,
        urgencyBefore, urgencyAfter, oldPsel, psel, longInsertProb, randomValue,
        insertedNear ? "near(max-1)" : "distant(max)",
        static_cast<int>(replData->rrpv));
}

ReplaceableEntry*
FuzzyDRRIPRP::getVictim(const ReplacementCandidates& candidates) const
{
    const uint32_t set = candidates[0]->getSet();

    for (const auto& candidate : candidates) {
        std::shared_ptr<FuzzyReplData> replData =
            std::static_pointer_cast<FuzzyReplData>(
                candidate->replacementData);
        replData->set = candidate->getSet();
        replData->hasSet = true;
    }

    const unsigned urgency = static_cast<unsigned>(getSetUrgencyCounter(set));
    DPRINTF(FuzzyRepl,
        "FuzzyDRRIP getVictim: set=%u ways=%zu urgency=%u l_idx=%d g_idx=%d\n",
        set, candidates.size(), urgency, localStateIdx(set), globalStateIdx());

    return BRRIP::getVictim(candidates);
}

bool
FuzzyDRRIPRP::shouldOverrideDuelingOnFollowers(
    const std::vector<std::shared_ptr<ReplacementData>>& candidates) const
{
    if (!enableDuelingHotSetOverride || candidates.empty()) {
        return false;
    }

    std::shared_ptr<FuzzyReplData> replData =
        std::static_pointer_cast<FuzzyReplData>(candidates[0]);
    if (!replData->hasSet) {
        return false;
    }

    return localStateIdx(replData->set) >= duelingHotSetOverrideThreshold;
}

std::shared_ptr<ReplacementData>
FuzzyDRRIPRP::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new FuzzyReplData(numRRPVBits));
}

} // namespace replacement_policy
} // namespace gem5
