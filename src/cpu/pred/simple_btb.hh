/*
 * Copyright (c) 2022-2023 The University of Edinburgh
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#ifndef __CPU_PRED_SIMPLE_BTB_HH__
#define __CPU_PRED_SIMPLE_BTB_HH__

#include <memory>

#include "base/cache/associative_cache.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/pred/btb.hh"
#include "cpu/pred/btb_indexing_policy.hh"
#include "params/SimpleBTB.hh"

namespace gem5
{

namespace branch_prediction
{

class BTBEntry : public CacheEntry
{
  public:
    using TagExtractor = std::function<Addr(Addr)>;

    /** The entry's target. */
    std::unique_ptr<PCStateBase> target;

    /** The entry's thread id. */
    ThreadID tid;

    /** Pointer to the static branch instruction at this address */
    StaticInstPtr inst;

    /** Update the entry in the BTB */
    void update(ThreadID _tid,
                const PCStateBase &_target,
                StaticInstPtr _inst)
    {
        tid = _tid;
        set(target, _target);
        inst = _inst;
    }

    /** Match the tag of the BTB entry */
    bool match(const Addr _tag, ThreadID _tid)
    {
        return (match(_tag) && (tid == _tid));
    }

    /** Copy constructor */
    BTBEntry(const BTBEntry &other) : CacheEntry(other)
    {
        tid   = other.tid;
        inst  = other.inst;
    }

    /** Default constructor */
    BTBEntry(TagExtractor ext) : CacheEntry(ext), tid(0), inst(nullptr)  {}

    using CacheEntry::match;
};

class BTBCache : public AssociativeCache<BTBEntry>
{
  private:
    BTBIndexingPolicy *indexingPolicy;

  public:
    BTBCache(const char *_my_name, const size_t num_entries,
             const size_t associativity,
             replacement_policy::Base *_repl_policy,
             BaseIndexingPolicy *_indexing_policy)
        : AssociativeCache(_my_name, num_entries, associativity,
                           _repl_policy, _indexing_policy,
                           BTBEntry(genTagExtractor(_indexing_policy)))
    {
        indexingPolicy = static_cast<BTBIndexingPolicy*>(_indexing_policy);
    }

    BTBEntry *findEntry(const Addr instPC, ThreadID tid) const;
    BTBEntry *accessEntry(Addr instPC, ThreadID tid);
    BTBEntry *findVictim(const Addr addr, ThreadID tid);

  private:
    using AssociativeCache<BTBEntry>::accessEntry;
    using AssociativeCache<BTBEntry>::findEntry;
    using AssociativeCache<BTBEntry>::findVictim;
};

class SimpleBTB : public BranchTargetBuffer
{
  public:
    SimpleBTB(const SimpleBTBParams &params);

    void memInvalidate() override;
    bool valid(ThreadID tid, Addr instPC) override;
    const PCStateBase *lookup(ThreadID tid, Addr instPC,
                              BranchType type = BranchType::NoBranch) override;
    void update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                BranchType type = BranchType::NoBranch,
                StaticInstPtr inst = nullptr) override;
    const StaticInstPtr getInst(ThreadID tid, Addr instPC) override;

  private:
    /** Returns the index into the BTB, based on the branch's PC.
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the BTB.
     */
    inline unsigned getIndex(Addr instPC, ThreadID tid);

    /** Internal call to find an address in the BTB
     * @param instPC The branch's address.
     * @return Returns a pointer to the BTB entry if found, nullptr otherwise.
    */
    BTBEntry *findEntry(Addr instPC, ThreadID tid);

    /** The actual BTB. */
    BTBCache btb;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_SIMPLE_BTB_HH__
