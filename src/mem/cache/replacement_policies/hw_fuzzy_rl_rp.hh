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

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_HW_FUZZY_RL_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_HW_FUZZY_RL_RP_HH__

#include <array>
#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/fuzzy_drrip_rp.hh"
#include "params/HwFuzzyRLRP.hh"

namespace gem5
{

namespace replacement_policy
{

class HwFuzzyRLRP : public FuzzyDRRIPRP
{
  protected:
    enum RLAction : uint8_t
    {
        DecreaseAlpha = 0,
        HoldAlpha = 1,
        IncreaseAlpha = 2,
        NumActions = 3
    };

    struct SetState
    {
        uint8_t accesses;
        uint8_t hits;
        uint8_t previousHitRateQ6;
        bool hasPendingAction;
        uint8_t pendingGlobalIdx;
        uint8_t pendingLocalIdx;
        RLAction pendingAction;

        SetState()
            : accesses(0),
              hits(0),
              previousHitRateQ6(0),
              hasPendingAction(false),
              pendingGlobalIdx(0),
              pendingLocalIdx(0),
              pendingAction(HoldAlpha)
        {}
    };

    struct ReuseEntry
    {
        Addr tag;
        uint8_t score;
        bool valid;

        ReuseEntry() : tag(0), score(0), valid(false) {}
    };

    struct HwRLStats : public statistics::Group
    {
        statistics::Scalar rewardWindows;
        statistics::Scalar qUpdates;
        statistics::Scalar increaseActions;
        statistics::Scalar decreaseActions;
        statistics::Scalar holdActions;
        statistics::Scalar exploratoryActions;
        statistics::Scalar strongHitUpgrades;
        statistics::Scalar weakHitUpgrades;
        statistics::Scalar immediateInsertions;
        statistics::Scalar nearInsertions;
        statistics::Scalar distantInsertions;
        statistics::Scalar positiveRewards;
        statistics::Scalar negativeRewards;
        statistics::Scalar neutralRewards;
        statistics::Scalar reusePredictorHits;
        statistics::Scalar reusePredictorMisses;
        statistics::Scalar reusePredictorAllocs;

        HwRLStats(statistics::Group *parent);
    };

    static constexpr uint8_t alphaScale = 63;
    static constexpr int16_t qMin = -31;
    static constexpr int16_t qMax = 31;
    static constexpr uint8_t probScale = 31;
    static constexpr uint8_t alphaImmediateThresholdQ6 = 54;
    static constexpr uint8_t alphaNearThresholdQ6 = 22;
    static constexpr uint8_t maxReuseScore = 7;
    static constexpr unsigned reuseLineOffsetBits = 6;

    inline static constexpr uint8_t hitUpgradeLutQ5[8][8] = {
        {9, 9, 8, 7, 6, 4, 2, 1}, {8, 8, 7, 6, 5, 3, 2, 1},
        {7, 7, 6, 5, 4, 2, 2, 1}, {6, 5, 4, 4, 3, 2, 1, 1},
        {4, 3, 3, 2, 2, 1, 1, 0}, {2, 2, 2, 1, 1, 1, 0, 0},
        {1, 1, 1, 1, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0, 0, 0},
    };

    mutable std::array<std::array<uint8_t, 8>, 8> alphaTableQ6;
    mutable std::array<std::array<std::array<int16_t, NumActions>, 8>, 8>
        qTable;
    mutable std::vector<SetState> setStates;
    mutable std::vector<ReuseEntry> reuseTable;
    mutable uint8_t lfsrState;

    const uint8_t qLearningRateQ8;
    const uint8_t qDiscountQ8;
    const uint8_t qEpsilonQ5;
    const uint8_t alphaStepQ6;
    const uint8_t rewardWindowSize;
    const uint8_t rewardDeadbandQ6;
    const int8_t holdActionInitQ;
    const uint8_t immediateReuseThreshold;
    const unsigned reuseTagBits;

    mutable HwRLStats hwRlStats;

    SetState &getSetState(uint32_t set) const;
    uint8_t nextRandom5() const;
    static int16_t clampQ(int32_t value);
    static uint8_t clampAlpha(int32_t value);
    Addr reuseLineAddr(const PacketPtr pkt) const;
    size_t reuseIndex(Addr line_addr) const;
    Addr reuseTag(Addr line_addr) const;
    uint8_t lookupReuseScore(const PacketPtr pkt) const;
    void recordReuseHit(const PacketPtr pkt) const;
    void ageReuseScore(const PacketPtr pkt) const;
    RLAction selectAction(int global_idx, int local_idx) const;
    void applyAction(int global_idx, int local_idx, RLAction action) const;
    void
    applyHitUpgrade(const std::shared_ptr<FuzzyReplData> &replacement_data,
                    int global_idx, int local_idx) const;
    void observeReward(uint32_t set, bool is_hit, int global_idx,
                       int local_idx) const;
    void doReset(const std::shared_ptr<FuzzyReplData> &repl_data,
                 const PacketPtr pkt) const;

  public:
    typedef HwFuzzyRLRPParams Params;
    HwFuzzyRLRP(const Params &p);

    void touch(const std::shared_ptr<ReplacementData> &replacement_data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
    void reset(const std::shared_ptr<ReplacementData> &replacement_data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_HW_FUZZY_RL_RP_HH__
