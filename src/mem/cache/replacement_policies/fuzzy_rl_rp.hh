#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_FUZZY_RL_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_FUZZY_RL_RP_HH__

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/fuzzy_drrip_rp.hh"
#include "params/FuzzyRLRP.hh"

namespace gem5
{

namespace replacement_policy
{

class FuzzyRLRP : public FuzzyDRRIPRP
{
  protected:
    enum RLAction : uint8_t
    {
        DecreaseAlpha = 0,
        HoldAlpha = 1,
        IncreaseAlpha = 2,
        NumActions = 3
    };

    struct RLSetState
    {
        uint64_t accesses;
        uint64_t hits;
        double previousHitRate;
        bool hasPendingAction;
        uint8_t pendingGlobalIdx;
        uint8_t pendingLocalIdx;
        RLAction pendingAction;

        RLSetState()
            : accesses(0),
              hits(0),
              previousHitRate(0.0),
              hasPendingAction(false),
              pendingGlobalIdx(0),
              pendingLocalIdx(0),
              pendingAction(HoldAlpha)
        {}
    };

    struct RLStats : public statistics::Group
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

        RLStats(statistics::Group *parent);
    };

    mutable std::array<std::array<double, 8>, 8> alphaTable;
    mutable std::array<std::array<std::array<double, NumActions>, 8>, 8>
        qTable;
    mutable std::unordered_map<uint32_t, RLSetState> setState;
    mutable std::unordered_map<Addr, uint8_t> reuseHistory;

    static constexpr unsigned reuseLineOffsetBits = 6;
    static constexpr uint8_t maxReuseScore = 7;
    static constexpr uint8_t immediateReuseThreshold = 2;
    static constexpr double alphaImmediateThreshold = 0.85;
    static constexpr double alphaNearThreshold = 0.35;

    // Strong hit promotion is intentionally much sparser than insertion
    // adaptation. In FP mode, every hit already reduces RRPV by one, so an
    // aggressive reset-to-zero policy tends to over-protect hot/thrashing
    // sets.
    inline static constexpr double hitUpgradeLut[8][8] = {
        {0.30, 0.28, 0.26, 0.22, 0.18, 0.12, 0.08, 0.04},
        {0.28, 0.26, 0.24, 0.20, 0.16, 0.10, 0.06, 0.03},
        {0.24, 0.22, 0.20, 0.16, 0.12, 0.08, 0.05, 0.03},
        {0.18, 0.16, 0.14, 0.12, 0.10, 0.06, 0.03, 0.02},
        {0.12, 0.10, 0.09, 0.08, 0.06, 0.04, 0.02, 0.01},
        {0.08, 0.07, 0.06, 0.05, 0.04, 0.03, 0.01, 0.00},
        {0.05, 0.04, 0.03, 0.02, 0.01, 0.01, 0.00, 0.00},
        {0.03, 0.02, 0.01, 0.00, 0.00, 0.00, 0.00, 0.00},
    };

    const double qLearningRate;
    const double qDiscount;
    const double qEpsilon;
    const double alphaStep;
    const uint64_t rewardWindowSize;
    const double minAlpha;
    const double maxAlpha;
    const double rewardDeadband;
    const double holdActionInitQ;

    mutable RLStats rlStats;

    RLSetState &getSetState(uint32_t set) const;
    double clampAlpha(double alpha) const;
    Addr reuseKey(const PacketPtr pkt) const;
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

  public:
    typedef FuzzyRLRPParams Params;
    FuzzyRLRP(const Params &p);

    void touch(const std::shared_ptr<ReplacementData> &replacement_data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
    void reset(const std::shared_ptr<ReplacementData> &replacement_data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData> &replacement_data)
        const override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_FUZZY_RL_RP_HH__
