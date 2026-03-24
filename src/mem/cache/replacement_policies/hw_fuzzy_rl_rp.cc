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

#include "mem/cache/replacement_policies/hw_fuzzy_rl_rp.hh"

#include <algorithm>
#include <cmath>

#include "debug/FuzzyRepl.hh"

namespace gem5
{

namespace replacement_policy
{

HwFuzzyRLRP::HwRLStats::HwRLStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(rewardWindows, statistics::units::Count::get(),
               "Number of per-set reward windows evaluated"),
      ADD_STAT(qUpdates, statistics::units::Count::get(),
               "Number of fixed-point Q-value updates performed"),
      ADD_STAT(increaseActions, statistics::units::Count::get(),
               "Number of alpha increment actions selected"),
      ADD_STAT(decreaseActions, statistics::units::Count::get(),
               "Number of alpha decrement actions selected"),
      ADD_STAT(holdActions, statistics::units::Count::get(),
               "Number of alpha hold actions selected"),
      ADD_STAT(exploratoryActions, statistics::units::Count::get(),
               "Number of exploratory actions selected"),
      ADD_STAT(strongHitUpgrades, statistics::units::Count::get(),
               "Number of strong fuzzy hit promotions to RRPV zero"),
      ADD_STAT(weakHitUpgrades, statistics::units::Count::get(),
               "Number of weak hit promotions using baseline BRRIP touch"),
      ADD_STAT(immediateInsertions, statistics::units::Count::get(),
               "Number of immediate insertions (rrpv=0)"),
      ADD_STAT(nearInsertions, statistics::units::Count::get(),
               "Number of near insertions (rrpv=max-1)"),
      ADD_STAT(distantInsertions, statistics::units::Count::get(),
               "Number of distant insertions (rrpv=max)"),
      ADD_STAT(positiveRewards, statistics::units::Count::get(),
               "Number of positive fixed-point rewards"),
      ADD_STAT(negativeRewards, statistics::units::Count::get(),
               "Number of negative fixed-point rewards"),
      ADD_STAT(neutralRewards, statistics::units::Count::get(),
               "Number of neutral fixed-point rewards"),
      ADD_STAT(reusePredictorHits, statistics::units::Count::get(),
               "Number of sampled reuse predictor hits"),
      ADD_STAT(reusePredictorMisses, statistics::units::Count::get(),
               "Number of sampled reuse predictor misses"),
      ADD_STAT(reusePredictorAllocs, statistics::units::Count::get(),
               "Number of sampled reuse predictor allocations")
{}

HwFuzzyRLRP::HwFuzzyRLRP(const Params &p)
    : FuzzyDRRIPRP(p),
      reuseTable(std::max<size_t>(1, p.reuse_table_entries)),
      lfsrState(static_cast<uint8_t>(p.lfsr_seed & 0x1f)),
      qLearningRateQ8(
          static_cast<uint8_t>(std::min<unsigned>(p.q_learning_rate_q8, 255))),
      qDiscountQ8(
          static_cast<uint8_t>(std::min<unsigned>(p.q_discount_q8, 255))),
      qEpsilonQ5(static_cast<uint8_t>(std::min<unsigned>(p.q_epsilon_q5, 31))),
      alphaStepQ6(static_cast<uint8_t>(
          std::min<unsigned>(p.alpha_step_q6, alphaScale))),
      rewardWindowSize(static_cast<uint8_t>(std::max<unsigned>(
          1, std::min<unsigned>(p.reward_window_size, 63)))),
      rewardDeadbandQ6(static_cast<uint8_t>(
          std::min<unsigned>(p.reward_deadband_q6, alphaScale))),
      holdActionInitQ(static_cast<int8_t>(std::clamp(p.hold_action_init_q,
                                                     static_cast<int>(qMin),
                                                     static_cast<int>(qMax)))),
      immediateReuseThreshold(static_cast<uint8_t>(
          std::min<unsigned>(p.immediate_reuse_threshold, maxReuseScore))),
      reuseTagBits(std::clamp(p.reuse_tag_bits, 1u,
                              static_cast<unsigned>(sizeof(Addr) * 8))),
      hwRlStats(this)
{
    if (lfsrState == 0) {
        lfsrState = 1;
    }

    for (int g = 0; g < 8; ++g) {
        for (int l = 0; l < 8; ++l) {
            alphaTableQ6[g][l] = static_cast<uint8_t>(std::clamp<int>(
                static_cast<int>(std::lround(fuzzyLut[g][l] * alphaScale)), 0,
                alphaScale));
            qTable[g][l][DecreaseAlpha] = 0;
            qTable[g][l][HoldAlpha] = holdActionInitQ;
            qTable[g][l][IncreaseAlpha] = 0;
        }
    }
}

HwFuzzyRLRP::SetState &
HwFuzzyRLRP::getSetState(uint32_t set) const
{
    if (setStates.size() <= set) {
        setStates.resize(set + 1);
    }
    return setStates[set];
}

uint8_t
HwFuzzyRLRP::nextRandom5() const
{
    const uint8_t feedback = ((lfsrState >> 4) ^ (lfsrState >> 2)) & 0x1;
    lfsrState = static_cast<uint8_t>(((lfsrState << 1) & 0x1f) | feedback);
    if (lfsrState == 0) {
        lfsrState = 1;
    }
    return lfsrState & 0x1f;
}

int16_t
HwFuzzyRLRP::clampQ(int32_t value)
{
    return static_cast<int16_t>(std::clamp(value, static_cast<int32_t>(qMin),
                                           static_cast<int32_t>(qMax)));
}

uint8_t
HwFuzzyRLRP::clampAlpha(int32_t value)
{
    return static_cast<uint8_t>(
        std::clamp(value, 0, static_cast<int32_t>(alphaScale)));
}

Addr
HwFuzzyRLRP::reuseLineAddr(const PacketPtr pkt) const
{
    if (!pkt) {
        return 0;
    }

    return pkt->getAddr() >> reuseLineOffsetBits;
}

size_t
HwFuzzyRLRP::reuseIndex(Addr line_addr) const
{
    return line_addr % reuseTable.size();
}

Addr
HwFuzzyRLRP::reuseTag(Addr line_addr) const
{
    const Addr full_tag = line_addr / reuseTable.size();
    if (reuseTagBits >= sizeof(Addr) * 8) {
        return full_tag;
    }
    return full_tag & ((static_cast<Addr>(1) << reuseTagBits) - 1);
}

uint8_t
HwFuzzyRLRP::lookupReuseScore(const PacketPtr pkt) const
{
    if (!pkt) {
        return 0;
    }

    const Addr line_addr = reuseLineAddr(pkt);
    const ReuseEntry &entry = reuseTable[reuseIndex(line_addr)];
    const Addr tag = reuseTag(line_addr);
    if (entry.valid && entry.tag == tag) {
        hwRlStats.reusePredictorHits++;
        return entry.score;
    }

    hwRlStats.reusePredictorMisses++;
    return 0;
}

void
HwFuzzyRLRP::recordReuseHit(const PacketPtr pkt) const
{
    if (!pkt) {
        return;
    }

    const Addr line_addr = reuseLineAddr(pkt);
    ReuseEntry &entry = reuseTable[reuseIndex(line_addr)];
    const Addr tag = reuseTag(line_addr);
    if (!entry.valid || entry.tag != tag) {
        entry.valid = true;
        entry.tag = tag;
        entry.score = 1;
        hwRlStats.reusePredictorAllocs++;
        return;
    }

    if (entry.score < maxReuseScore) {
        entry.score++;
    }
}

void
HwFuzzyRLRP::ageReuseScore(const PacketPtr pkt) const
{
    if (!pkt) {
        return;
    }

    const Addr line_addr = reuseLineAddr(pkt);
    ReuseEntry &entry = reuseTable[reuseIndex(line_addr)];
    const Addr tag = reuseTag(line_addr);
    if (entry.valid && entry.tag == tag && entry.score > 0) {
        entry.score--;
    }
}

HwFuzzyRLRP::RLAction
HwFuzzyRLRP::selectAction(int global_idx, int local_idx) const
{
    if (nextRandom5() < qEpsilonQ5) {
        hwRlStats.exploratoryActions++;
        return static_cast<RLAction>(nextRandom5() % NumActions);
    }

    const auto &qValues = qTable[global_idx][local_idx];
    RLAction bestAction = HoldAlpha;
    int16_t bestQ = qValues[HoldAlpha];
    if (qValues[IncreaseAlpha] > bestQ) {
        bestAction = IncreaseAlpha;
        bestQ = qValues[IncreaseAlpha];
    }
    if (qValues[DecreaseAlpha] > bestQ) {
        bestAction = DecreaseAlpha;
    }

    return bestAction;
}

void
HwFuzzyRLRP::applyAction(int global_idx, int local_idx, RLAction action) const
{
    uint8_t &alpha = alphaTableQ6[global_idx][local_idx];
    if (action == IncreaseAlpha) {
        alpha = clampAlpha(static_cast<int32_t>(alpha) + alphaStepQ6);
        hwRlStats.increaseActions++;
    } else if (action == DecreaseAlpha) {
        alpha = clampAlpha(static_cast<int32_t>(alpha) - alphaStepQ6);
        hwRlStats.decreaseActions++;
    } else {
        hwRlStats.holdActions++;
    }
}

void
HwFuzzyRLRP::applyHitUpgrade(
    const std::shared_ptr<FuzzyReplData> &replacement_data, int global_idx,
    int local_idx) const
{
    if (nextRandom5() < hitUpgradeLutQ5[global_idx][local_idx]) {
        replacement_data->rrpv.reset();
        hwRlStats.strongHitUpgrades++;
    } else {
        if (hitPriority) {
            replacement_data->rrpv.reset();
        } else {
            replacement_data->rrpv--;
        }
        hwRlStats.weakHitUpgrades++;
    }
}

void
HwFuzzyRLRP::observeReward(uint32_t set, bool is_hit, int global_idx,
                           int local_idx) const
{
    SetState &state = getSetState(set);
    if (state.accesses < 63) {
        state.accesses++;
    }
    if (is_hit && state.hits < 63) {
        state.hits++;
    }

    if (state.accesses < rewardWindowSize) {
        return;
    }

    hwRlStats.rewardWindows++;
    const uint8_t currentHitRateQ6 = static_cast<uint8_t>(std::min<int>(
        alphaScale,
        (state.hits * alphaScale + (state.accesses / 2)) / state.accesses));
    int16_t reward = static_cast<int16_t>(currentHitRateQ6) -
                     static_cast<int16_t>(state.previousHitRateQ6);
    if (std::abs(reward) < rewardDeadbandQ6) {
        reward = 0;
    }

    if (reward > 0) {
        hwRlStats.positiveRewards++;
    } else if (reward < 0) {
        hwRlStats.negativeRewards++;
    } else {
        hwRlStats.neutralRewards++;
    }

    if (state.hasPendingAction) {
        int16_t &oldQ = qTable[state.pendingGlobalIdx][state.pendingLocalIdx]
                              [state.pendingAction];
        const int16_t bestNext =
            std::max(qTable[global_idx][local_idx][DecreaseAlpha],
                     std::max(qTable[global_idx][local_idx][HoldAlpha],
                              qTable[global_idx][local_idx][IncreaseAlpha]));
        const int16_t discountedNext = static_cast<int16_t>(
            (static_cast<int32_t>(qDiscountQ8) * bestNext + 128) >> 8);
        const int16_t target = static_cast<int16_t>(reward + discountedNext);
        const int16_t delta = static_cast<int16_t>(target - oldQ);
        oldQ = clampQ(oldQ + ((static_cast<int32_t>(qLearningRateQ8) * delta +
                               (delta >= 0 ? 128 : -128)) >>
                              8));
        hwRlStats.qUpdates++;
    }

    const RLAction action = selectAction(global_idx, local_idx);
    applyAction(global_idx, local_idx, action);

    DPRINTF(FuzzyRepl,
            "HwFuzzyRLRP train: set=%u state=(%d,%d) reward_q6=%d "
            "hit_rate_q6=%u prev_q6=%u action=%d alpha_q6=%u q=[%d,%d,%d]\n",
            set, global_idx, local_idx, reward, currentHitRateQ6,
            state.previousHitRateQ6, action,
            alphaTableQ6[global_idx][local_idx],
            qTable[global_idx][local_idx][DecreaseAlpha],
            qTable[global_idx][local_idx][HoldAlpha],
            qTable[global_idx][local_idx][IncreaseAlpha]);

    state.previousHitRateQ6 = currentHitRateQ6;
    state.accesses = 0;
    state.hits = 0;
    state.hasPendingAction = true;
    state.pendingGlobalIdx = global_idx;
    state.pendingLocalIdx = local_idx;
    state.pendingAction = action;
}

void
HwFuzzyRLRP::touch(
    const std::shared_ptr<ReplacementData> &replacement_data) const
{
    std::shared_ptr<FuzzyReplData> replData =
        std::static_pointer_cast<FuzzyReplData>(replacement_data);

    if (replData->hasSet) {
        SatCounter8 &urgency = getSetUrgencyCounter(replData->set);
        const unsigned oldUrgency = static_cast<unsigned>(urgency);
        urgency++;
        const int globalIdx = globalStateIdx();
        const int localIdx = localStateIdx(replData->set);
        DPRINTF(FuzzyRepl,
                "HwFuzzyRLRP touch: set=%u urgency=%u->%u state=(%d,%d) "
                "rrpv_before=%d\n",
                replData->set, oldUrgency, static_cast<unsigned>(urgency),
                globalIdx, localIdx, static_cast<int>(replData->rrpv));
        observeReward(replData->set, true, globalIdx, localIdx);
        applyHitUpgrade(replData, globalIdx, localIdx);
    } else {
        BRRIP::touch(replacement_data);
    }

    fuzzyStats.rrpvSetByTouch[static_cast<unsigned>(replData->rrpv)]++;
}

void
HwFuzzyRLRP::touch(const std::shared_ptr<ReplacementData> &replacement_data,
                   const PacketPtr pkt)
{
    recordReuseHit(pkt);
    touch(replacement_data);
}

void
HwFuzzyRLRP::doReset(const std::shared_ptr<FuzzyReplData> &replData,
                     const PacketPtr pkt) const
{
    const int globalIdx = globalStateIdx();
    const unsigned urgencyBefore =
        replData->hasSet
            ? static_cast<unsigned>(getSetUrgencyCounter(replData->set))
            : urgencyCounterInit;
    const int localIdx = replData->hasSet ? localStateIdx(replData->set) : 3;
    const uint8_t alphaQ6 = alphaTableQ6[globalIdx][localIdx];
    const uint8_t reuseScore = lookupReuseScore(pkt);
    const bool allowImmediate = alphaQ6 >= alphaImmediateThresholdQ6 &&
                                reuseScore >= immediateReuseThreshold;
    const int oldPsel = psel;

    if (allowImmediate) {
        replData->rrpv.reset();
        hwRlStats.immediateInsertions++;
    } else if (alphaQ6 >= alphaNearThresholdQ6) {
        replData->rrpv.saturate();
        replData->rrpv--;
        hwRlStats.nearInsertions++;
    } else {
        replData->rrpv.saturate();
        hwRlStats.distantInsertions++;
    }
    replData->valid = true;
    fuzzyStats.rrpvSetByReset[static_cast<unsigned>(replData->rrpv)]++;

    unsigned urgencyAfter = urgencyBefore;
    if (replData->hasSet) {
        observeReward(replData->set, false, globalIdx, localIdx);
        SatCounter8 &urgency = getSetUrgencyCounter(replData->set);
        urgency--;
        urgencyAfter = static_cast<unsigned>(urgency);
    }

    if (useInternalPsel) {
        if (localIdx <= 3) {
            psel = std::max(0, psel - 1);
        } else {
            psel = std::min(pselMax, psel + 1);
        }
    }

    ageReuseScore(pkt);

    DPRINTF(
        FuzzyRepl,
        "HwFuzzyRLRP reset: set=%u has_set=%d state=(%d,%d) urgency=%u->%u "
        "psel=%d->%d alpha_q6=%u reuse=%u insert=%s rrpv=%d\n",
        replData->set, replData->hasSet, globalIdx, localIdx, urgencyBefore,
        urgencyAfter, oldPsel, psel, alphaQ6, reuseScore,
        allowImmediate ? "immediate(0)"
                       : (alphaQ6 >= alphaNearThresholdQ6 ? "near(max-1)"
                                                          : "distant(max)"),
        static_cast<int>(replData->rrpv));
}

void
HwFuzzyRLRP::reset(
    const std::shared_ptr<ReplacementData> &replacement_data) const
{
    doReset(std::static_pointer_cast<FuzzyReplData>(replacement_data),
            nullptr);
}

void
HwFuzzyRLRP::reset(const std::shared_ptr<ReplacementData> &replacement_data,
                   const PacketPtr pkt)
{
    doReset(std::static_pointer_cast<FuzzyReplData>(replacement_data), pkt);
}

std::shared_ptr<ReplacementData>
HwFuzzyRLRP::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new FuzzyReplData(numRRPVBits));
}

} // namespace replacement_policy
} // namespace gem5
