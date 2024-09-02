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

#include "cpu/pred/simple_btb.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/BTB.hh"

namespace gem5::branch_prediction
{

/*
BTBEntry*
BTBCache::findEntry(const KeyType &key) const
{
    // auto tag = indexingPolicy->extractTag(key.address);

    auto entries = indexingPolicy->getPossibleEntries(key);

    for (auto it = entries.begin(); it != entries.end(); it++) {
        BTBEntry *entry = static_cast<BTBEntry*>(*it);
        if (entry->match(key)) {
            return entry;
        }
    }

    return nullptr;
}

BTBEntry*
BTBCache::accessEntry(const KeyType &key)
{
    auto entry = findEntry(key);

    if (entry) {
        replPolicy->touch(entry->replacementData);
    }

    return entry;
}

BTBEntry *
BTBCache::findVictim(const KeyType &key)
{
    auto candidates = indexingPolicy->getPossibleEntries(key);

    auto repl_victim = replPolicy->getVictim(candidates);
    auto victim = static_cast<BTBEntry*>(repl_victim);

    invalidate(victim);

    return victim;
}
*/

SimpleBTB::SimpleBTB(const SimpleBTBParams &p)
    : BranchTargetBuffer(p),
      btb("simpleBTB", p.numEntries, p.associativity,
          p.btbReplPolicy, p.btbIndexingPolicy, BTBEntry(genTagExtractor(p.btbIndexingPolicy)))
{
    DPRINTF(BTB, "BTB: Creating BTB object.\n");

    if (!isPowerOf2(p.numEntries)) {
        fatal("BTB entries is not a power of 2!");
    }
}

void
SimpleBTB::memInvalidate()
{
    btb.clear();
}

BTBEntry *
SimpleBTB::findEntry(const KeyType &key)
{
    return btb.findEntry(key);
}

bool
SimpleBTB::valid(const KeyType &key)
{
    BTBEntry *entry = btb.findEntry(key);

    return entry != nullptr;
}

// @todo Create some sort of return struct that has both whether or not the
// address is valid, and also the address.  For now will just use addr = 0 to
// represent invalid entry.
const PCStateBase *
SimpleBTB::lookup(const KeyType &key, BranchType type)
{
    stats.lookups[type]++;

    BTBEntry *entry = btb.accessEntry(key);

    if (entry) {
        return entry->target.get();
    }

    stats.misses[type]++;
    return nullptr;
}

const StaticInstPtr
SimpleBTB::getInst(const KeyType &key)
{
    BTBEntry *entry = btb.findEntry(key);

    if (entry) {
        return entry->inst;
    }

    return nullptr;
}

void
SimpleBTB::update(const KeyType &key,
                  const PCStateBase &target,
                  BranchType type, StaticInstPtr inst)
{
    stats.updates[type]++;

    BTBEntry *victim = btb.findVictim(key);

    btb.insertEntry(key, victim);
    victim->update(key.tid, target, inst);
}


} // namespace gem5::branch_prediction

