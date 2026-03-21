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

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_FUZZY_DRRIP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_FUZZY_DRRIP_RP_HH__

#include <cstdint>
#include <unordered_map>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/brrip_rp.hh"
#include "params/FuzzyDRRIPRP.hh"

namespace gem5
{

namespace replacement_policy
{

class FuzzyDRRIPRP : public BRRIP
{
  protected:
    struct FuzzyStats : public statistics::Group
    {
        const unsigned maxRRPV;

        statistics::Vector rrpvSetByReset;
        statistics::Vector rrpvSetByTouch;

        FuzzyStats(statistics::Group* parent, unsigned num_rrpv_bits);
        void regStats() override;
    };

    struct FuzzyReplData : BRRIPReplData
    {
        uint32_t set;
        bool hasSet;

        explicit FuzzyReplData(const int num_bits)
            : BRRIPReplData(num_bits), set(0), hasSet(false)
        {
        }
    };

    // The Look-Up Table (8x8) for fuzzy insertion probability.
    inline static constexpr double fuzzyLut[8][8] = {
        {0.95, 0.90, 0.85, 0.70, 0.50, 0.30, 0.15, 0.05},
        {0.90, 0.85, 0.80, 0.65, 0.45, 0.25, 0.10, 0.05},
        {0.80, 0.75, 0.70, 0.55, 0.35, 0.20, 0.10, 0.05},
        {0.60, 0.55, 0.50, 0.45, 0.30, 0.15, 0.05, 0.02},
        {0.40, 0.35, 0.30, 0.25, 0.20, 0.10, 0.05, 0.02},
        {0.20, 0.15, 0.10, 0.08, 0.05, 0.03, 0.01, 0.00},
        {0.10, 0.05, 0.03, 0.02, 0.01, 0.01, 0.00, 0.00},
        {0.05, 0.02, 0.01, 0.00, 0.00, 0.00, 0.00, 0.00},
    };

    // Global PSEL state used to select a LUT row.
    static constexpr int pselMax = 1023;
    mutable int psel;
    const bool useInternalPsel;
    const int fixedGlobalStateIdx;

    static constexpr unsigned urgencyCounterBits = 6;
    static constexpr unsigned urgencyCounterMax =
        (1u << urgencyCounterBits) - 1;
    static constexpr unsigned urgencyCounterInit = urgencyCounterMax / 2;

    mutable std::unordered_map<uint32_t, SatCounter8> setUrgency;
    mutable FuzzyStats fuzzyStats;

    SatCounter8& getSetUrgencyCounter(uint32_t set) const;
    int globalStateIdx() const;
    int localStateIdx(uint32_t set) const;

  public:
    typedef FuzzyDRRIPRPParams Params;
    FuzzyDRRIPRP(const Params &p);

    void touch(
        const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void reset(
        const std::shared_ptr<ReplacementData>& replacement_data) const override;
    ReplaceableEntry* getVictim(
        const ReplacementCandidates& candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_FUZZY_DRRIP_RP_HH__
