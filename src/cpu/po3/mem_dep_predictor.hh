/*
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

#ifndef __CPU_PO3_MEM_DEP_PREDICTOR_HH__
#define __CPU_PO3_MEM_DEP_PREDICTOR_HH__

#include <cstdint>
#include <string_view>

#include "base/named.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5
{

namespace po3
{

/** Information PHAST consumes when an instruction is dispatched. */
struct MemDepPredInstruction
{
    Addr pc = 0;
    InstSeqNum seqNum = 0;
    bool isLoad = false;
    bool isStore = false;
    bool isControl = false;
    bool isConditional = false;
    bool isIndirect = false;
    bool predictedTaken = false;
    Addr predictedTarget = 0;
};

/** Common interface for PO3 memory-dependence predictors. */
class MemDepPredictor : public Named
{
  public:
    explicit MemDepPredictor(std::string_view name) : Named(name) {}
    virtual ~MemDepPredictor() = default;

    virtual void
    observeInstruction(const MemDepPredInstruction &inst)
    {}

    virtual void violation(Addr store_pc, Addr load_pc,
                           InstSeqNum store_seq_num,
                           InstSeqNum load_seq_num) = 0;
    virtual void insertLoad(Addr load_pc, InstSeqNum load_seq_num) = 0;
    virtual void insertStore(Addr store_pc, InstSeqNum store_seq_num,
                             ThreadID tid) = 0;
    virtual InstSeqNum checkInst(Addr pc, InstSeqNum seq_num,
                                 bool is_load) = 0;
    virtual void issued(Addr issued_pc, InstSeqNum issued_seq_num,
                        bool is_store) = 0;
    virtual void
    commitInstruction(InstSeqNum seq_num, bool is_load, bool stlf_forwarded,
                      InstSeqNum forwarding_store_seq,
                      bool prediction_validated, bool prediction_correct)
    {}
    virtual void
    resolveBranch(InstSeqNum seq_num, bool taken, Addr target)
    {}
    virtual void squash(InstSeqNum squashed_num, ThreadID tid) = 0;
    virtual void clear() = 0;
    virtual void dump() = 0;

    /** Work performed by the most recent checkInst() lookup. */
    virtual uint64_t
    lastCandidateChecks() const
    {
        return 0;
    }

    /** Bloom-positive candidates seen by the most recent lookup. */
    virtual uint64_t
    lastFilterPositivePairs() const
    {
        return 0;
    }

    /** Prediction-cache tables searched by the most recent lookup. */
    virtual uint64_t
    lastTableLookups() const
    {
        return 0;
    }

    /** Entries allocated by the most recent training operation. */
    virtual uint64_t
    lastAllocations() const
    {
        return 0;
    }

    /** Confidence updates made by the most recent operation. */
    virtual uint64_t
    lastConfidenceUpdates() const
    {
        return 0;
    }

    /** Predictions whose distance did not name an in-flight store. */
    virtual uint64_t
    lastDistanceOutOfRange() const
    {
        return 0;
    }

    /** Confident lookup performed with no tracked stores. */
    virtual uint64_t
    lastNoTrackedStores() const
    {
        return 0;
    }

    /** Stored distance exceeded the newest dynamic store ordinal. */
    virtual uint64_t
    lastDistanceUnderflows() const
    {
        return 0;
    }

    /** Predicted dynamic store ordinal was not tracked. */
    virtual uint64_t
    lastTargetOrdinalMissing() const
    {
        return 0;
    }

    /** Training deltas that exceeded the predictor's encoding. */
    virtual uint64_t
    lastTrainingDistanceOverflows() const
    {
        return 0;
    }

    /** Whether the most recent operation observed a training STID delta. */
    virtual bool
    hasLastStoreIdDelta() const
    {
        return false;
    }

    /** Exact training STID delta, including values beyond the encoding. */
    virtual uint64_t
    lastStoreIdDelta() const
    {
        return 0;
    }
};

} // namespace po3
} // namespace gem5

#endif // __CPU_PO3_MEM_DEP_PREDICTOR_HH__
