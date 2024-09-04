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
#include "cpu/pred/btb_entry.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/base.hh"
#include "params/SimpleBTB.hh"
#include "params/BTBIndexingPolicy.hh"

namespace gem5
{

using BTBIndexingPolicy = IndexingPolicyTemplate<branch_prediction::BTBTagTypes>;
template class IndexingPolicyTemplate<branch_prediction::BTBTagTypes>;

namespace branch_prediction
{

class BTBEntry : public ReplaceableEntry
{
  public:
    using IndexingPolicy = BTBIndexingPolicy;
    using KeyType = BTBTagTypes::KeyType;
    using TagExtractor = std::function<Addr(Addr)>;

    /** Default constructor */
    BTBEntry(TagExtractor ext)
        : inst(nullptr), tid(0), extractTag(ext), valid(false), tag(MaxAddr)
    {}

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
    bool match(const KeyType &key)
    {
        return (match(key.address) && (tid == key.tid));
    }

    /** Copy constructor */
    BTBEntry(const BTBEntry &other) : ReplaceableEntry(other)
    {
        tid   = other.tid;
        inst  = other.inst;
        set(target, other.target);
        valid = other.valid;
        tag = other.tag;
        extractTag = other.extractTag;
    }

    /**
     * Checks if the entry is valid.
     *
     * @return True if the entry is valid.
     */
    virtual bool isValid() const { return valid; }

    /**
     * Get tag associated to this block.
     *
     * @return The tag value.
     */
    virtual Addr getTag() const { return tag; }

    /**
     * Checks if the given tag information corresponds to this entry's.
     *
     * @param addr The address value to be compared before tag is extracted
     * @return True if the tag information match this entry's.
     */
    virtual bool
    match(const Addr addr) const
    {
        return isValid() && (getTag() == extractTag(addr));
    }

    /**
     * Insert the block by assigning it a tag and marking it valid. Touches
     * block if it hadn't been touched previously.
     *
     * @param addr The address value.
     */
    virtual void
    insert(const KeyType &key)
    {
        setValid();
        setTag(extractTag(key.address));
    }

    /** Invalidate the block. Its contents are no longer valid. */
    virtual void
    invalidate()
    {
        valid = false;
        setTag(MaxAddr);
    }

    std::string
    print() const override
    {
        return csprintf("tag: %#x valid: %d | %s", getTag(),
                        isValid(), ReplaceableEntry::print());
    }

    /** The entry's target. */
    std::unique_ptr<PCStateBase> target;

    /** Pointer to the static branch instruction at this address */
    StaticInstPtr inst;

  protected:
    /**
     * Set tag associated to this block.
     *
     * @param tag The tag value.
     */
    virtual void setTag(Addr _tag) { tag = _tag; }

    /** Set valid bit. The block must be invalid beforehand. */
    virtual void
    setValid()
    {
        assert(!isValid());
        valid = true;
    }

  private:
    /** The entry's thread id. */
    ThreadID tid;

    /** Callback used to extract the tag from the entry */
    TagExtractor extractTag;

    /**
     * Valid bit. The contents of this entry are only valid if this bit is set.
     * @sa invalidate()
     * @sa insert()
     */
    bool valid;

    /** The entry's tag. */
    Addr tag;

};

/**
 * This helper generates an a tag extractor function object
 * which will be typically used by Replaceable entries indexed
 * with the BaseIndexingPolicy.
 * It allows to "decouple" indexing from tagging. Those entries
 * would call the functor without directly holding a pointer
 * to the indexing policy which should reside in the cache.
 */
static constexpr auto
genTagExtractor(BTBIndexingPolicy *ip)
{
    return [ip] (Addr addr) { return ip->extractTag(addr); };
}

class SimpleBTB : public BranchTargetBuffer
{
  public:
    using KeyType = BTBTagTypes::KeyType;
    SimpleBTB(const SimpleBTBParams &params);

    void memInvalidate() override;
    bool valid(const KeyType &key) override;
    const PCStateBase *lookup(const KeyType &key,
                              BranchType type = BranchType::NoBranch) override;
    void update(const KeyType &key, const PCStateBase &target_pc,
                BranchType type = BranchType::NoBranch,
                StaticInstPtr inst = nullptr) override;
    const StaticInstPtr getInst(const KeyType &key) override;

  private:
    /** Returns the index into the BTB, based on the branch's PC.
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the BTB.
     */
    inline unsigned getIndex(const KeyType &key);

    /** Internal call to find an address in the BTB
     * @param instPC The branch's address.
     * @return Returns a pointer to the BTB entry if found, nullptr otherwise.
    */
    BTBEntry *findEntry(const KeyType &key);

    /** The actual BTB. */
    AssociativeCache<BTBEntry> btb;
};

} // namespace gem5::branch_prediction

}

#endif // __CPU_PRED_SIMPLE_BTB_HH__
