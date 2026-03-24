#include "mem/cache/replacement_policies/dynamic_fuzzy_rp.hh"

#include <algorithm>

#include "params/DynamicFuzzyRP.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace replacement_policy
{

DynamicFuzzyRP::DynamicFuzzyStats::DynamicFuzzyStats(statistics::Group *parent,
                                                     unsigned num_rrpv_bits)
    : statistics::Group(parent),
      maxRRPV((1u << num_rrpv_bits) - 1),
      ADD_STAT(rrpvSetByReset, statistics::units::Count::get(),
               "Number of reset() writes per RRPV value"),
      ADD_STAT(rrpvSetByTouch, statistics::units::Count::get(),
               "Number of touch() writes per RRPV value"),
      ADD_STAT(globalIdxByReset, statistics::units::Count::get(),
               "Number of reset() events per fuzzy LUT global index"),
      ADD_STAT(localIdxByReset, statistics::units::Count::get(),
               "Number of reset() events per fuzzy LUT local index"),
      ADD_STAT(immediateInsertions, statistics::units::Count::get(),
               "Number of immediate insertions (rrpv=0)"),
      ADD_STAT(nearInsertions, statistics::units::Count::get(),
               "Number of near insertions (rrpv=max-1)"),
      ADD_STAT(distantInsertions, statistics::units::Count::get(),
               "Number of distant insertions (rrpv=max)"),
      ADD_STAT(
          lutImmediateDecisions, statistics::units::Count::get(),
          "Number of immediate decisions taken from LUT alpha thresholding"),
      ADD_STAT(lutNearDecisions, statistics::units::Count::get(),
               "Number of near decisions taken from LUT thresholding"),
      ADD_STAT(lutDistantDecisions, statistics::units::Count::get(),
               "Number of distant decisions taken from LUT thresholding"),
      ADD_STAT(tuneEvents, statistics::units::Count::get(),
               "Number of dynamic tuning updates"),
      ADD_STAT(tunedIntervalMisses, statistics::units::Count::get(),
               "Total misses seen in tuning intervals"),
      ADD_STAT(tunedIntervalAccesses, statistics::units::Count::get(),
               "Total accesses seen in tuning intervals")
{}

void
DynamicFuzzyRP::DynamicFuzzyStats::regStats()
{
    statistics::Group::regStats();

    rrpvSetByReset.init(maxRRPV + 1);
    rrpvSetByTouch.init(maxRRPV + 1);
    globalIdxByReset.init(8);
    localIdxByReset.init(8);
}

const double DynamicFuzzyRP::fuzzyLut[8][8] = {
    // More opinionated values to reduce noisy mid-probability behavior.
    {0.90, 0.90, 0.92, 0.94, 0.96, 0.98, 0.99, 1.00},
    {0.80, 0.82, 0.86, 0.90, 0.94, 0.97, 0.99, 1.00},
    {0.60, 0.65, 0.75, 0.82, 0.90, 0.95, 0.98, 1.00},
    {0.40, 0.45, 0.60, 0.72, 0.84, 0.92, 0.97, 1.00},
    {0.20, 0.25, 0.45, 0.62, 0.78, 0.90, 0.96, 1.00},
    {0.10, 0.12, 0.30, 0.50, 0.70, 0.86, 0.94, 0.99},
    {0.05, 0.08, 0.20, 0.40, 0.62, 0.82, 0.92, 0.98},
    {0.02, 0.05, 0.15, 0.32, 0.55, 0.78, 0.90, 0.97}};

DynamicFuzzyRP::DynamicFuzzyRP(const Params &p)
    : BRRIP(p),
      currentMissRate(0.15),
      intervalMisses(0),
      intervalAccesses(0),
      updateInterval(std::max<Tick>(1, p.update_interval)),
      nextTuneTick(curTick() + updateInterval),
      dynamicStats(this, numRRPVBits)
{}

void
DynamicFuzzyRP::tunePerformance() const
{
    if (intervalAccesses > 0) {
        dynamicStats.tuneEvents++;
        dynamicStats.tunedIntervalMisses += intervalMisses;
        dynamicStats.tunedIntervalAccesses += intervalAccesses;
        currentMissRate = static_cast<double>(intervalMisses) /
                          static_cast<double>(intervalAccesses);
    }

    intervalMisses = 0;
    intervalAccesses = 0;
}

std::shared_ptr<ReplacementData>
DynamicFuzzyRP::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(
        new DynamicFuzzyReplData(numRRPVBits));
}

void
DynamicFuzzyRP::touch(
    const std::shared_ptr<ReplacementData> &replacement_data) const
{
    auto data =
        std::static_pointer_cast<DynamicFuzzyReplData>(replacement_data);
    intervalAccesses++;
    BRRIP::touch(replacement_data);
    dynamicStats.rrpvSetByTouch[static_cast<unsigned>(data->rrpv)]++;
    if (data->hitCounter < maxHitCounter) {
        data->hitCounter++;
    }

    if (curTick() >= nextTuneTick) {
        tunePerformance();
        nextTuneTick = curTick() + updateInterval;
    }
}

void
DynamicFuzzyRP::reset(
    const std::shared_ptr<ReplacementData> &replacement_data) const
{
    auto data =
        std::static_pointer_cast<DynamicFuzzyReplData>(replacement_data);
    const unsigned maxRRPV = (1u << numRRPVBits) - 1;
    intervalAccesses++;
    intervalMisses++;

    // Map global miss rate [0.0, 0.40+] to LUT row [0, 7].
    const int globalIdx =
        std::clamp(static_cast<int>(currentMissRate * 20.0), 0, 7);
    // 5-bit local hit counter [0, 31] -> LUT col [0, 7].
    const int localIdx =
        std::clamp(static_cast<int>(data->hitCounter) >> 2, 0, 7);
    dynamicStats.globalIdxByReset[globalIdx]++;
    dynamicStats.localIdxByReset[localIdx]++;
    const double alpha = fuzzyLut[globalIdx][localIdx];

    // 3-state insertion:
    //   alpha > 0.85 -> rrpv=0 (immediate priority)
    //   alpha > 0.40 -> rrpv=max-1 (near insertion)
    //   else         -> rrpv=max (distant insertion)
    if (alpha > alphaImmediateThreshold) {
        data->rrpv.reset();
        dynamicStats.immediateInsertions++;
        dynamicStats.lutImmediateDecisions++;
    } else if (alpha > alphaNearThreshold) {
        data->rrpv.saturate();
        if (maxRRPV > 0) {
            --data->rrpv;
        }
        dynamicStats.nearInsertions++;
        dynamicStats.lutNearDecisions++;
    } else {
        data->rrpv.saturate();
        dynamicStats.distantInsertions++;
        dynamicStats.lutDistantDecisions++;
    }
    data->valid = true;
    dynamicStats.rrpvSetByReset[static_cast<unsigned>(data->rrpv)]++;
    // Aggressive local decay: each miss insertion resets heat to a low
    // baseline.
    data->hitCounter = (data->hitCounter > 0) ? 1 : 0;

    if (curTick() >= nextTuneTick) {
        tunePerformance();
        nextTuneTick = curTick() + updateInterval;
    }
}

} // namespace replacement_policy
} // namespace gem5
