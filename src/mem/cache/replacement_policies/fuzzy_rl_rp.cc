#include "mem/cache/replacement_policies/fuzzy_rl_rp.hh"

#include <algorithm>
#include <array>

#include "debug/FuzzyRepl.hh"

namespace gem5
{

namespace replacement_policy
{

FuzzyRLRP::RLStats::RLStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(rewardWindows, statistics::units::Count::get(),
               "Number of per-set reward windows evaluated"),
      ADD_STAT(qUpdates, statistics::units::Count::get(),
               "Number of Q-value updates performed"),
      ADD_STAT(increaseActions, statistics::units::Count::get(),
               "Number of alpha increment actions selected"),
      ADD_STAT(decreaseActions, statistics::units::Count::get(),
               "Number of alpha decrement actions selected"),
      ADD_STAT(holdActions, statistics::units::Count::get(),
               "Number of alpha hold actions selected"),
      ADD_STAT(exploratoryActions, statistics::units::Count::get(),
               "Number of epsilon-greedy exploratory actions selected"),
      ADD_STAT(strongHitUpgrades, statistics::units::Count::get(),
               "Number of strong fuzzy hit promotions to RRPV zero"),
      ADD_STAT(
          weakHitUpgrades, statistics::units::Count::get(),
          "Number of weak fuzzy hit promotions using baseline RRIP touch"),
      ADD_STAT(immediateInsertions, statistics::units::Count::get(),
               "Number of immediate insertions (rrpv=0)"),
      ADD_STAT(nearInsertions, statistics::units::Count::get(),
               "Number of near insertions (rrpv=max-1)"),
      ADD_STAT(distantInsertions, statistics::units::Count::get(),
               "Number of distant insertions (rrpv=max)"),
      ADD_STAT(positiveRewards, statistics::units::Count::get(),
               "Number of positive per-set hit-rate deltas"),
      ADD_STAT(negativeRewards, statistics::units::Count::get(),
               "Number of negative per-set hit-rate deltas"),
      ADD_STAT(neutralRewards, statistics::units::Count::get(),
               "Number of zero per-set hit-rate deltas")
{}

FuzzyRLRP::FuzzyRLRP(const Params &p)
    : FuzzyDRRIPRP(p),
      qLearningRate(std::clamp(p.q_learning_rate, 0.0, 1.0)),
      qDiscount(std::clamp(p.q_discount, 0.0, 1.0)),
      qEpsilon(std::clamp(p.q_epsilon, 0.0, 1.0)),
      alphaStep(std::max(0.0, p.alpha_step)),
      rewardWindowSize(std::max<uint64_t>(1, p.reward_window_size)),
      minAlpha(std::clamp(p.min_alpha, 0.0, 1.0)),
      maxAlpha(std::clamp(p.max_alpha, 0.0, 1.0)),
      rewardDeadband(std::max(0.0, p.reward_deadband)),
      holdActionInitQ(p.hold_action_init_q),
      rlStats(this)
{
    for (int g = 0; g < 8; ++g) {
        for (int l = 0; l < 8; ++l) {
            alphaTable[g][l] = std::clamp(fuzzyLut[g][l], minAlpha, maxAlpha);
            qTable[g][l][DecreaseAlpha] = 0.0;
            qTable[g][l][HoldAlpha] = holdActionInitQ;
            qTable[g][l][IncreaseAlpha] = 0.0;
        }
    }
}

FuzzyRLRP::RLSetState &
FuzzyRLRP::getSetState(uint32_t set) const
{
    return setState[set];
}

double
FuzzyRLRP::clampAlpha(double alpha) const
{
    return std::clamp(alpha, minAlpha, maxAlpha);
}

Addr
FuzzyRLRP::reuseKey(const PacketPtr pkt) const
{
    if (!pkt) {
        return 0;
    }

    // The current evaluation setup uses 64B lines. Use a line-aligned address
    // so all offsets within a cache line share the same reuse history entry.
    return pkt->getAddr() &
           ~((static_cast<Addr>(1) << reuseLineOffsetBits) - 1);
}

uint8_t
FuzzyRLRP::lookupReuseScore(const PacketPtr pkt) const
{
    if (!pkt) {
        return 0;
    }

    const Addr key = reuseKey(pkt);
    auto it = reuseHistory.find(key);
    return it == reuseHistory.end() ? 0 : it->second;
}

void
FuzzyRLRP::recordReuseHit(const PacketPtr pkt) const
{
    if (!pkt) {
        return;
    }

    const Addr key = reuseKey(pkt);
    uint8_t &score = reuseHistory[key];
    if (score < maxReuseScore) {
        ++score;
    }
}

void
FuzzyRLRP::ageReuseScore(const PacketPtr pkt) const
{
    if (!pkt) {
        return;
    }

    const Addr key = reuseKey(pkt);
    auto it = reuseHistory.find(key);
    if (it != reuseHistory.end() && it->second > 0) {
        --it->second;
    }
}

FuzzyRLRP::RLAction
FuzzyRLRP::selectAction(int global_idx, int local_idx) const
{
    const double random_value = rng->random<double>();
    if (random_value < qEpsilon) {
        rlStats.exploratoryActions++;
        return static_cast<RLAction>(rng->random<unsigned>(0, NumActions - 1));
    }

    const auto &q_values = qTable[global_idx][local_idx];
    RLAction best_action = HoldAlpha;
    double best_q = q_values[HoldAlpha];
    if (q_values[IncreaseAlpha] > best_q) {
        best_action = IncreaseAlpha;
        best_q = q_values[IncreaseAlpha];
    }
    if (q_values[DecreaseAlpha] > best_q) {
        best_action = DecreaseAlpha;
    }

    return best_action;
}

void
FuzzyRLRP::applyAction(int global_idx, int local_idx, RLAction action) const
{
    double &alpha = alphaTable[global_idx][local_idx];
    if (action == IncreaseAlpha) {
        alpha = clampAlpha(alpha + alphaStep);
        rlStats.increaseActions++;
    } else if (action == DecreaseAlpha) {
        alpha = clampAlpha(alpha - alphaStep);
        rlStats.decreaseActions++;
    } else {
        rlStats.holdActions++;
    }
}

void
FuzzyRLRP::applyHitUpgrade(
    const std::shared_ptr<FuzzyReplData> &replacement_data, int global_idx,
    int local_idx) const
{
    const double upgradeProb = hitUpgradeLut[global_idx][local_idx];
    if (rng->random<double>() < upgradeProb) {
        replacement_data->rrpv.reset();
        rlStats.strongHitUpgrades++;
    } else {
        if (hitPriority) {
            replacement_data->rrpv.reset();
        } else {
            replacement_data->rrpv--;
        }
        rlStats.weakHitUpgrades++;
    }
}

void
FuzzyRLRP::observeReward(uint32_t set, bool is_hit, int global_idx,
                         int local_idx) const
{
    RLSetState &state = getSetState(set);
    state.accesses++;
    if (is_hit) {
        state.hits++;
    }

    if (state.accesses < rewardWindowSize) {
        return;
    }

    rlStats.rewardWindows++;
    const double currentHitRate =
        static_cast<double>(state.hits) / static_cast<double>(state.accesses);
    const double rawReward = currentHitRate - state.previousHitRate;
    const double reward =
        std::abs(rawReward) < rewardDeadband ? 0.0 : rawReward;

    if (reward > 0.0) {
        rlStats.positiveRewards++;
    } else if (reward < 0.0) {
        rlStats.negativeRewards++;
    } else {
        rlStats.neutralRewards++;
    }

    if (state.hasPendingAction) {
        double &old_q = qTable[state.pendingGlobalIdx][state.pendingLocalIdx]
                              [state.pendingAction];
        const double best_next =
            std::max(qTable[global_idx][local_idx][DecreaseAlpha],
                     std::max(qTable[global_idx][local_idx][HoldAlpha],
                              qTable[global_idx][local_idx][IncreaseAlpha]));
        old_q += qLearningRate * (reward + qDiscount * best_next - old_q);
        rlStats.qUpdates++;
    }

    const RLAction action = selectAction(global_idx, local_idx);
    applyAction(global_idx, local_idx, action);

    DPRINTF(FuzzyRepl,
            "FuzzyRLRP train: set=%u state=(%d,%d) reward=%.6f hit_rate=%.6f "
            "prev_hit_rate=%.6f action=%s alpha=%.3f q_dec=%.6f q_hold=%.6f "
            "q_inc=%.6f\n",
            set, global_idx, local_idx, reward, currentHitRate,
            state.previousHitRate,
            action == DecreaseAlpha
                ? "dec"
                : (action == IncreaseAlpha ? "inc" : "hold"),
            alphaTable[global_idx][local_idx],
            qTable[global_idx][local_idx][DecreaseAlpha],
            qTable[global_idx][local_idx][HoldAlpha],
            qTable[global_idx][local_idx][IncreaseAlpha]);

    state.previousHitRate = currentHitRate;
    state.accesses = 0;
    state.hits = 0;
    state.hasPendingAction = true;
    state.pendingGlobalIdx = global_idx;
    state.pendingLocalIdx = local_idx;
    state.pendingAction = action;
}

void
FuzzyRLRP::touch(
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
                "FuzzyRLRP touch: set=%u urgency=%u->%u state=(%d,%d) "
                "rrpv_before=%d\n",
                replData->set, oldUrgency, static_cast<unsigned>(urgency),
                globalIdx, localIdx, static_cast<int>(replData->rrpv));
        observeReward(replData->set, true, globalIdx, localIdx);
    } else {
        DPRINTF(FuzzyRepl, "FuzzyRLRP touch: set=unknown rrpv_before=%d\n",
                static_cast<int>(replData->rrpv));
    }

    if (replData->hasSet) {
        const int globalIdx = globalStateIdx();
        const int localIdx = localStateIdx(replData->set);
        applyHitUpgrade(replData, globalIdx, localIdx);
        DPRINTF(
            FuzzyRepl,
            "FuzzyRLRP touch: state=(%d,%d) upgrade_prob=%.3f rrpv_after=%d\n",
            globalIdx, localIdx, hitUpgradeLut[globalIdx][localIdx],
            static_cast<int>(replData->rrpv));
    } else {
        BRRIP::touch(replacement_data);
    }
    fuzzyStats.rrpvSetByTouch[static_cast<unsigned>(replData->rrpv)]++;
}

void
FuzzyRLRP::touch(const std::shared_ptr<ReplacementData> &replacement_data,
                 const PacketPtr pkt)
{
    recordReuseHit(pkt);
    touch(replacement_data);
}

void
FuzzyRLRP::reset(const std::shared_ptr<ReplacementData> &replacement_data,
                 const PacketPtr pkt)
{
    std::shared_ptr<FuzzyReplData> replData =
        std::static_pointer_cast<FuzzyReplData>(replacement_data);

    const int globalIdx = globalStateIdx();
    const unsigned urgencyBefore =
        replData->hasSet
            ? static_cast<unsigned>(getSetUrgencyCounter(replData->set))
            : urgencyCounterInit;
    const int localIdx = replData->hasSet ? localStateIdx(replData->set) : 3;
    const double alpha = alphaTable[globalIdx][localIdx];
    const uint8_t reuseScore = lookupReuseScore(pkt);
    const bool allowImmediate = alpha >= alphaImmediateThreshold &&
                                reuseScore >= immediateReuseThreshold;
    const int oldPsel = psel;
    const double randomValue = rng->random<double>();
    const unsigned maxRRPV = (1u << numRRPVBits) - 1;

    if (allowImmediate) {
        replData->rrpv.reset();
        rlStats.immediateInsertions++;
    } else if (alpha >= alphaNearThreshold) {
        replData->rrpv.saturate();
        if (maxRRPV > 0) {
            replData->rrpv--;
        }
        rlStats.nearInsertions++;
    } else {
        replData->rrpv.saturate();
        rlStats.distantInsertions++;
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
        "FuzzyRLRP reset: set=%u has_set=%d state=(%d,%d) urgency=%u->%u "
        "psel=%d->%d alpha=%.3f reuse=%u rand=%.6f insert=%s rrpv=%d\n",
        replData->set, replData->hasSet, globalIdx, localIdx, urgencyBefore,
        urgencyAfter, oldPsel, psel, alpha, reuseScore, randomValue,
        allowImmediate
            ? "immediate(0)"
            : (alpha >= alphaNearThreshold ? "near(max-1)" : "distant(max)"),
        static_cast<int>(replData->rrpv));
}

void
FuzzyRLRP::reset(
    const std::shared_ptr<ReplacementData> &replacement_data) const
{
    std::shared_ptr<FuzzyReplData> replData =
        std::static_pointer_cast<FuzzyReplData>(replacement_data);

    const int globalIdx = globalStateIdx();
    const unsigned urgencyBefore =
        replData->hasSet
            ? static_cast<unsigned>(getSetUrgencyCounter(replData->set))
            : urgencyCounterInit;
    const int localIdx = replData->hasSet ? localStateIdx(replData->set) : 3;
    const double alpha = alphaTable[globalIdx][localIdx];
    const int oldPsel = psel;
    const double randomValue = rng->random<double>();
    const unsigned maxRRPV = (1u << numRRPVBits) - 1;

    if (alpha >= alphaNearThreshold) {
        replData->rrpv.saturate();
        if (maxRRPV > 0) {
            replData->rrpv--;
        }
        rlStats.nearInsertions++;
    } else {
        replData->rrpv.saturate();
        rlStats.distantInsertions++;
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

    DPRINTF(FuzzyRepl,
            "FuzzyRLRP reset: set=%u has_set=%d state=(%d,%d) urgency=%u->%u "
            "psel=%d->%d alpha=%.3f rand=%.6f insert=%s rrpv=%d\n",
            replData->set, replData->hasSet, globalIdx, localIdx,
            urgencyBefore, urgencyAfter, oldPsel, psel, alpha, randomValue,
            alpha >= alphaNearThreshold ? "near(max-1)" : "distant(max)",
            static_cast<int>(replData->rrpv));
}

} // namespace replacement_policy
} // namespace gem5
