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

#include "mem/cache/replacement_policies/hw_fuzzy_drrip_rp.hh"

#include <algorithm>

#include "debug/FuzzyRepl.hh"

namespace gem5
{

namespace replacement_policy
{

HwFuzzyDRRIPRP::HwFuzzyStats::HwFuzzyStats(statistics::Group *parent,
                                           unsigned num_rrpv_bits)
    : statistics::Group(parent),
      maxRRPV((1u << num_rrpv_bits) - 1),
      ADD_STAT(rrpvSetByReset, statistics::units::Count::get(),
               "Number of reset() writes per RRPV value"),
      ADD_STAT(rrpvSetByTouch, statistics::units::Count::get(),
               "Number of touch() writes per RRPV value")
{}

void
HwFuzzyDRRIPRP::HwFuzzyStats::regStats()
{
    statistics::Group::regStats();

    rrpvSetByReset.init(maxRRPV + 1);
    rrpvSetByTouch.init(maxRRPV + 1);
}

HwFuzzyDRRIPRP::HwFuzzyDRRIPRP(const Params &p)
    : BRRIP(p),
      psel(std::clamp(p.initial_psel, 0, pselMax)),
      lfsrState(static_cast<uint8_t>(p.lfsr_seed & 0x1f)),
      useInternalPsel(p.use_internal_psel),
      fixedGlobalStateIdx(std::clamp(p.fixed_global_state_idx, 0, 7)),
      dynamicInternalPsel(p.dynamic_internal_psel),
      pselControlEpochMisses(
          std::max<uint64_t>(1, p.psel_control_epoch_misses)),
      pselDisableLowRail(std::clamp(p.psel_disable_low_rail, 0, pselMax)),
      pselDisableHighRail(std::clamp(p.psel_disable_high_rail, 0, pselMax)),
      pselDisableNearHigh(std::clamp(p.psel_disable_near_high, 0.0, 1.0)),
      pselDisableNearLow(std::clamp(p.psel_disable_near_low, 0.0, 1.0)),
      pselDisableEpochs(std::max<uint32_t>(1, p.psel_disable_epochs)),
      pselReenableCooldownEpochs(p.psel_reenable_cooldown_epochs),
      pselReenableNearLow(std::clamp(p.psel_reenable_near_low, 0.0, 1.0)),
      pselReenableNearHigh(std::clamp(p.psel_reenable_near_high, 0.0, 1.0)),
      enableDuelingHotSetOverride(p.enable_dueling_hot_set_override),
      duelingHotSetOverrideThreshold(static_cast<uint8_t>(
          std::clamp(p.dueling_hot_set_override_threshold, 0, 7))),
      internalPselEnabled(p.use_internal_psel),
      epochMisses(0),
      epochNearInserts(0),
      disableSignalEpochs(0),
      cooldownEpochsLeft(0),
      hwFuzzyStats(this, numRRPVBits)
{
    if (lfsrState == 0) {
        lfsrState = 1;
    }
}

int
HwFuzzyDRRIPRP::globalStateIdx() const
{
    if (!useInternalPsel || !internalPselEnabled) {
        return fixedGlobalStateIdx;
    }
    // Quantize 10-bit PSEL to 3 MSBs.
    return (psel >> 7) & 0x7;
}

uint8_t
HwFuzzyDRRIPRP::nextLfsrNibble() const
{
    // x^5 + x^3 + 1, 5-bit Fibonacci LFSR.
    const uint8_t feedback = ((lfsrState >> 4) ^ (lfsrState >> 2)) & 0x1;
    lfsrState = static_cast<uint8_t>(((lfsrState << 1) & 0x1f) | feedback);
    if (lfsrState == 0) {
        lfsrState = 1;
    }
    return lfsrState & 0x0f;
}

uint8_t
HwFuzzyDRRIPRP::quantizeSetHeat(unsigned hitCount, unsigned ways)
{
    if (ways == 0) {
        return 0;
    }

    const unsigned scaled = (hitCount * 8) / ways;
    return static_cast<uint8_t>(std::min(scaled, 7u));
}

void
HwFuzzyDRRIPRP::updateDynamicPselState(bool insertedNear) const
{
    if (!dynamicInternalPsel) {
        return;
    }

    epochMisses++;
    if (insertedNear) {
        epochNearInserts++;
    }
    if (epochMisses < pselControlEpochMisses) {
        return;
    }

    const double nearRatio = static_cast<double>(epochNearInserts) /
                             static_cast<double>(epochMisses);
    const bool pselAtRail =
        (psel <= pselDisableLowRail) || (psel >= pselDisableHighRail);
    const bool nearIsExtreme = (nearRatio >= pselDisableNearHigh) ||
                               (nearRatio <= pselDisableNearLow);

    if (internalPselEnabled) {
        if (pselAtRail && nearIsExtreme) {
            disableSignalEpochs++;
        } else {
            disableSignalEpochs = 0;
        }

        if (disableSignalEpochs >= pselDisableEpochs) {
            internalPselEnabled = false;
            cooldownEpochsLeft = pselReenableCooldownEpochs;
            disableSignalEpochs = 0;
            DPRINTF(FuzzyRepl,
                    "HwFuzzyDRRIP dynamic PSEL: disabled "
                    "(psel=%d near_ratio=%.3f)\n",
                    psel, nearRatio);
        }
    } else {
        if (cooldownEpochsLeft > 0) {
            cooldownEpochsLeft--;
        } else {
            const bool nearInRecoveryBand =
                (nearRatio >= pselReenableNearLow) &&
                (nearRatio <= pselReenableNearHigh);
            if (!pselAtRail || nearInRecoveryBand) {
                internalPselEnabled = true;
                DPRINTF(FuzzyRepl,
                        "HwFuzzyDRRIP dynamic PSEL: re-enabled "
                        "(psel=%d near_ratio=%.3f)\n",
                        psel, nearRatio);
            }
        }
    }

    epochMisses = 0;
    epochNearInserts = 0;
}

void
HwFuzzyDRRIPRP::touch(
    const std::shared_ptr<ReplacementData> &replacement_data) const
{
    std::shared_ptr<HwFuzzyReplData> replData =
        std::static_pointer_cast<HwFuzzyReplData>(replacement_data);
    const bool oldRecentlyHit = replData->recentlyHit;
    replData->recentlyHit = true;
    DPRINTF(
        FuzzyRepl,
        "HwFuzzyDRRIP touch: set_heat=%u recently_hit=%d->%d rrpv_before=%d\n",
        replData->setHeat, oldRecentlyHit, replData->recentlyHit,
        static_cast<int>(replData->rrpv));
    BRRIP::touch(replacement_data);
    hwFuzzyStats.rrpvSetByTouch[static_cast<unsigned>(replData->rrpv)]++;
    DPRINTF(FuzzyRepl, "HwFuzzyDRRIP touch: rrpv_after=%d\n",
            static_cast<int>(replData->rrpv));
}

void
HwFuzzyDRRIPRP::reset(
    const std::shared_ptr<ReplacementData> &replacement_data) const
{
    std::shared_ptr<HwFuzzyReplData> replData =
        std::static_pointer_cast<HwFuzzyReplData>(replacement_data);

    const int gIdx = globalStateIdx();
    const int lIdx = std::min<int>(replData->setHeat, 7);
    const int lutIdx = (gIdx << 3) | lIdx;

    const uint8_t nearQ4 = nearProbQ4Lut[lutIdx];
    const int oldPsel = psel;
    const uint8_t oldLfsrState = lfsrState;
    const uint8_t rnd = nextLfsrNibble();

    // SRRIP-like insertion on success, BRRIP-like distant insertion otherwise.
    replData->rrpv.saturate();
    const bool insertedNear = rnd < nearQ4;
    if (insertedNear) {
        replData->rrpv--;
    }
    replData->valid = true;
    hwFuzzyStats.rrpvSetByReset[static_cast<unsigned>(replData->rrpv)]++;

    // Fresh insertion starts as not recently hit.
    replData->recentlyHit = false;
    replData->setHeat = 0;
    replData->setHeatValid = false;

    if (useInternalPsel && internalPselEnabled) {
        // Hot sets bias toward cache-friendly global states.
        if (lIdx >= 4) {
            psel = std::max(0, psel - 1);
        } else {
            psel = std::min(pselMax, psel + 1);
        }
    }

    DPRINTF(FuzzyRepl,
            "HwFuzzyDRRIP reset: g_idx=%d l_idx=%d lut_idx=%d near_q4=%u "
            "lfsr_state=%u->%u rnd=%u psel=%d->%d psel_enabled=%d "
            "insert=%s rrpv=%d\n",
            gIdx, lIdx, lutIdx, nearQ4, oldLfsrState, lfsrState, rnd, oldPsel,
            psel, internalPselEnabled,
            insertedNear ? "near(max-1)" : "distant(max)",
            static_cast<int>(replData->rrpv));

    updateDynamicPselState(insertedNear);
}

ReplaceableEntry *
HwFuzzyDRRIPRP::getVictim(const ReplacementCandidates &candidates) const
{
    const uint32_t set = candidates[0]->getSet();
    const std::shared_ptr<HwFuzzyReplData> firstReplData =
        std::static_pointer_cast<HwFuzzyReplData>(
            candidates[0]->replacementData);

    bool usedSnapshot = firstReplData->setHeatValid;
    uint8_t setHeat = 0;
    unsigned hitCount = 0;
    if (usedSnapshot) {
        setHeat = std::min<uint8_t>(firstReplData->setHeat, 7);
    } else {
        for (const auto &candidate : candidates) {
            std::shared_ptr<HwFuzzyReplData> replData =
                std::static_pointer_cast<HwFuzzyReplData>(
                    candidate->replacementData);
            if (replData->recentlyHit) {
                hitCount++;
            }
        }
        setHeat = quantizeSetHeat(hitCount, candidates.size());
    }

    for (const auto &candidate : candidates) {
        std::shared_ptr<HwFuzzyReplData> replData =
            std::static_pointer_cast<HwFuzzyReplData>(
                candidate->replacementData);
        replData->setHeat = setHeat;
        replData->setHeatValid = false;
        // Clear hit marks every miss so set heat tracks a short, recent
        // window.
        replData->recentlyHit = false;
    }

    DPRINTF(FuzzyRepl,
            "HwFuzzyDRRIP getVictim: set=%u ways=%zu hit_count=%u set_heat=%u "
            "heat_source=%s global_idx=%d\n",
            set, candidates.size(), hitCount, setHeat,
            usedSnapshot ? "snapshot" : "live", globalStateIdx());

    return BRRIP::getVictim(candidates);
}

bool
HwFuzzyDRRIPRP::shouldOverrideDuelingOnFollowers(
    const std::vector<std::shared_ptr<ReplacementData>> &candidates) const
{
    if (!enableDuelingHotSetOverride || candidates.empty()) {
        return false;
    }

    unsigned hitCount = 0;
    for (const auto &candidate : candidates) {
        std::shared_ptr<HwFuzzyReplData> replData =
            std::static_pointer_cast<HwFuzzyReplData>(candidate);
        if (replData->recentlyHit) {
            hitCount++;
        }
    }

    const uint8_t setHeat = quantizeSetHeat(hitCount, candidates.size());
    for (const auto &candidate : candidates) {
        std::shared_ptr<HwFuzzyReplData> replData =
            std::static_pointer_cast<HwFuzzyReplData>(candidate);
        replData->setHeat = setHeat;
        replData->setHeatValid = true;
        // Start a fresh hit window for the next miss decision.
        replData->recentlyHit = false;
    }
    return setHeat >= duelingHotSetOverrideThreshold;
}

std::shared_ptr<ReplacementData>
HwFuzzyDRRIPRP::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new HwFuzzyReplData(numRRPVBits));
}

} // namespace replacement_policy
} // namespace gem5
