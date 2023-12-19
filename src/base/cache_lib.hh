/*
 * Copyright (c) 2024 Pranith Kumar
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

#ifndef __CACHE_LIBRARY_HH__
#define __CACHE_LIBRARY_HH__

#include <vector>

#include "intmath.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/base.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "types.hh"

namespace gem5
{

/** Cache Library.
 *
 */
template <typename Entry>
class CacheLibrary
{
    static_assert(std::is_base_of_v<TaggedEntry, Entry>,
                  "Entry should be derived from TaggedEntry");
  public:
    typedef replacement_policy::Base BaseReplacementPolicy;

  protected:

    /** Name of the cache. */
    std::string cache_name;

    /** The number of entries in the cache. */
    size_t numEntries;

    /** Associativity of the cache. */
    size_t _associativity;

    /** The size of an entry in the cache. */
    size_t entrySize;

    /** The replacement policy of the cache. */
    BaseReplacementPolicy *replPolicy;

    /** Indexing policy of the cache */
    BaseIndexingPolicy *indexingPolicy;

    /** Number of sets in the cache. */
    size_t numSets;

  private:

    void initParams(size_t _num_entries, size_t _assoc, size_t _entry_size)
    {
        for (auto entry_idx = 0; entry_idx < _num_entries; entry_idx++) {
            auto entry = new Entry();
            indexingPolicy->setEntry(entry, entry_idx);
            entry->replacementData = replPolicy->instantiateEntry();
        }
    }

  public:

    CacheLibrary() {}
    CacheLibrary(const char *_my_name, const size_t num_entries,
                 const size_t associativity, const size_t _entry_size,
                 const size_t num_tag_bits,
                 BaseReplacementPolicy *_replPolicy,
                 BaseIndexingPolicy *_indexingPolicy)
        : cache_name(_my_name), numEntries(num_entries),
          _associativity(associativity),
          entrySize(_entry_size), replPolicy(_replPolicy),
          indexingPolicy(_indexingPolicy),
          numSets(num_entries / associativity)
    {
        initParams(numEntries, associativity, entrySize);
    }

    ~CacheLibrary() {
        for (auto set = 0; set < numSets; set++) {
            for (auto way = 0; way < _associativity; way++) {
                auto entry = indexingPolicy->getEntry(set, way);
                delete entry;
            }
        }
    }

    std::string name() const { return cache_name; }

    void clear() {
        for (auto set = 0; set < numSets; set++) {
            for (auto way = 0; way < _associativity; way++) {
                auto repl_entry = indexingPolicy->getEntry(set, way);

                Entry *entry = dynamic_cast<Entry*>(repl_entry);
                invalidate(entry);
            }
        }
    }

    void init(const char *name, const size_t num_entries,
              const size_t associativity, const size_t _entry_size,
              const size_t _num_tag_bits,
              BaseReplacementPolicy *_repl_policy,
              BaseIndexingPolicy *_indexing_policy)
    {
        cache_name           = std::string(name);
        numEntries           = num_entries;
        _associativity       = associativity;
        entrySize            = _entry_size;
        replPolicy           = _repl_policy;
        indexingPolicy       = _indexing_policy;
        numSets              = num_entries / associativity;

        initParams(numEntries, associativity, entrySize);
    }

    virtual bool isEntryValid(const Addr addr)
    {
        auto const entry = findEntry(addr);

        return entry && entry->isValid();
    }

    virtual size_t getTag(const Addr addr)
    {
        return indexingPolicy->extractTag(addr);
    }

    virtual Entry* accessEntry(const Addr addr)
    {
        auto entry = findEntry(addr);

        if (entry) {
            replPolicy->touch(entry->replacementData);
        }

        return entry;
    }

    virtual Entry* findEntry(const Addr addr)
    {
        auto tag   = getTag(addr);

        auto entries = indexingPolicy->getPossibleEntries(addr);

        for (auto it = entries.begin(); it != entries.end(); it++) {
            Entry *entry = dynamic_cast<Entry*>(*it);
            if (entry->matchTag(tag)) {
                return entry;
            }
        }

        return nullptr;
    }

    virtual Entry* findVictim(const Addr addr)
    {
        auto candidates = indexingPolicy->getPossibleEntries(addr);

        auto victim = dynamic_cast<Entry*>(replPolicy->getVictim(candidates));

        invalidate(victim);

        return victim;
    }

    virtual void invalidate(Entry *entry) const
    {
        entry->invalidate();
        replPolicy->invalidate(entry->replacementData);
    }

    virtual void insertEntry(Addr addr, Entry *entry)
    {
        entry->insert(indexingPolicy->extractTag(addr));
        replPolicy->reset(entry->replacementData);
    }
};

}

#endif
