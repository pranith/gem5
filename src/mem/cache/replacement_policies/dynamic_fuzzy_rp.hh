#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_DYNAMIC_FUZZY_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_DYNAMIC_FUZZY_RP_HH__

#include <cstdint>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/replacement_policies/brrip_rp.hh"

namespace gem5
{

struct DynamicFuzzyRPParams;

namespace replacement_policy
{

class DynamicFuzzyRP : public BRRIP
{
  protected:
    struct DynamicFuzzyStats : public statistics::Group
    {
        const unsigned maxRRPV;

        statistics::Vector rrpvSetByReset;
        statistics::Vector rrpvSetByTouch;
        statistics::Vector globalIdxByReset;
        statistics::Vector localIdxByReset;
        statistics::Scalar immediateInsertions;
        statistics::Scalar nearInsertions;
        statistics::Scalar distantInsertions;
        statistics::Scalar lutImmediateDecisions;
        statistics::Scalar lutNearDecisions;
        statistics::Scalar lutDistantDecisions;
        statistics::Scalar tuneEvents;
        statistics::Scalar tunedIntervalMisses;
        statistics::Scalar tunedIntervalAccesses;

        DynamicFuzzyStats(statistics::Group *parent, unsigned num_rrpv_bits);
        void regStats() override;
    };

    struct DynamicFuzzyReplData : BRRIPReplData
    {
        uint8_t hitCounter;

        explicit DynamicFuzzyReplData(const int num_bits)
            : BRRIPReplData(num_bits), hitCounter(0)
        {}
    };

    static constexpr uint8_t maxHitCounter = 31;
    static constexpr double alphaImmediateThreshold = 0.85;
    static constexpr double alphaNearThreshold = 0.40;

    mutable double currentMissRate;
    mutable uint64_t intervalMisses;
    mutable uint64_t intervalAccesses;
    const Tick updateInterval;
    mutable Tick nextTuneTick;
    mutable DynamicFuzzyStats dynamicStats;

    static const double fuzzyLut[8][8];

    void tunePerformance() const;

  public:
    typedef DynamicFuzzyRPParams Params;
    DynamicFuzzyRP(const Params &p);
    ~DynamicFuzzyRP() = default;

    std::shared_ptr<ReplacementData> instantiateEntry() override;
    void touch(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
    void reset(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_DYNAMIC_FUZZY_RP_HH__
