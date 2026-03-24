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

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_HW_FUZZY_DRRIP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_HW_FUZZY_DRRIP_RP_HH__

#include <array>
#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/brrip_rp.hh"
#include "params/HwFuzzyDRRIPRP.hh"

namespace gem5
{

namespace replacement_policy
{

/**
 * Hardware-oriented fuzzy RRIP approximation:
 *  - global input: top 3 bits of a 10-bit PSEL-like counter
 *  - local input: popcount of per-line recently-hit bits, quantized to 3 bits
 *  - output: Q4 threshold (0..16) selected by an 8x8 LUT
 *  - random source: 5-bit LFSR, compared as a 4-bit value (0..15)
 */
class HwFuzzyDRRIPRP : public BRRIP
{
  protected:
    struct HwFuzzyStats : public statistics::Group
    {
        const unsigned maxRRPV;

        statistics::Vector rrpvSetByReset;
        statistics::Vector rrpvSetByTouch;

        HwFuzzyStats(statistics::Group *parent, unsigned num_rrpv_bits);
        void regStats() override;
    };

    struct HwFuzzyReplData : BRRIPReplData
    {
        bool recentlyHit;
        uint8_t setHeat;
        bool setHeatValid;

        explicit HwFuzzyReplData(const int num_bits)
            : BRRIPReplData(num_bits),
              recentlyHit(false),
              setHeat(0),
              setHeatValid(false)
        {}
    };

    /**
     * 8x8 fuzzy LUT encoded as Q4 "near insertion" thresholds (0..16).
     * Index: (global_state << 3) | local_state.
     *
     * global_state: 0 (friendly) .. 7 (thrashing)
     * local_state:  0 (cold) .. 7 (hot)
     */
    inline static constexpr std::array<uint8_t, 64> nearProbQ4Lut = {
        13, 14, 14, 15, 15, 16, 16, 16, 11, 12, 13, 14, 14, 15, 16, 16,
        8,  10, 11, 12, 13, 14, 14, 15, 5,  6,  8,  9,  10, 11, 13, 14,
        2,  4,  6,  6,  7,  9,  10, 12, 1,  2,  3,  4,  5,  6,  8,  10,
        0,  1,  2,  2,  3,  4,  6,  7,  0,  0,  0,  1,  2,  2,  3,  5,
    };

    static constexpr int pselMax = 1023;

    mutable int psel;
    mutable uint8_t lfsrState;

    const bool useInternalPsel;
    const int fixedGlobalStateIdx;
    const bool dynamicInternalPsel;
    const uint64_t pselControlEpochMisses;
    const int pselDisableLowRail;
    const int pselDisableHighRail;
    const double pselDisableNearHigh;
    const double pselDisableNearLow;
    const uint32_t pselDisableEpochs;
    const uint32_t pselReenableCooldownEpochs;
    const double pselReenableNearLow;
    const double pselReenableNearHigh;
    const bool enableDuelingHotSetOverride;
    const uint8_t duelingHotSetOverrideThreshold;

    mutable bool internalPselEnabled;
    mutable uint64_t epochMisses;
    mutable uint64_t epochNearInserts;
    mutable uint32_t disableSignalEpochs;
    mutable uint32_t cooldownEpochsLeft;

    mutable HwFuzzyStats hwFuzzyStats;

    int globalStateIdx() const;
    uint8_t nextLfsrNibble() const;
    static uint8_t quantizeSetHeat(unsigned hitCount, unsigned ways);
    void updateDynamicPselState(bool insertedNear) const;

  public:
    typedef HwFuzzyDRRIPRPParams Params;
    HwFuzzyDRRIPRP(const Params &p);

    void touch(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
    void reset(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
    ReplaceableEntry *
    getVictim(const ReplacementCandidates &candidates) const override;
    bool shouldOverrideDuelingOnFollowers(
        const std::vector<std::shared_ptr<ReplacementData>> &candidates)
        const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_HW_FUZZY_DRRIP_RP_HH__
