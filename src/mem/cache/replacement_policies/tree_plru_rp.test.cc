/**
 * Copyright (c) 2025 The Regents of the University of California
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

#include <gtest/gtest.h>

#include <cassert>

#include "mem/cache/replacement_policies/tree_plru_rp.hh"
#include "params/TreePLRURP.hh"

class TreePLRUTestF : public ::testing::Test
{
  public:
    std::shared_ptr<gem5::replacement_policy::TreePLRU> rp;
    int num_leaves = 8;

    TreePLRUTestF()
    {
        gem5::TreePLRURPParams params;
        params.eventq_index = 0;
        params.num_leaves = num_leaves;
        rp = std::make_shared<gem5::replacement_policy::TreePLRU>(params);
    }
    TreePLRUTestF(int num_entries)
    {
        gem5::TreePLRURPParams params;
        params.eventq_index = 0;
        num_leaves = num_entries;
        params.num_leaves = num_leaves;
        rp = std::make_shared<gem5::replacement_policy::TreePLRU>(params);
    }
};

TEST_F(TreePLRUTestF, InstantiatedEntry)
{
    const auto repl_data = rp->instantiateEntry();
    ASSERT_NE(repl_data, nullptr);
}

/// Test that if there is one candidate and it is invalid, it will be
/// the victim

// I'm not sure that it makes sense to test this for TreePLRU.
// Mostly just taken from the FIFO SimObject unit tests introduced in
// https://github.com/gem5/gem5/pull/1977.

TEST_F(TreePLRUTestF, GetVictim1Candidate)
{
    gem5::ReplaceableEntry entry;
    entry.replacementData = rp->instantiateEntry();
    gem5::ReplacementCandidates candidates;
    candidates.push_back(&entry);
    ASSERT_EQ(rp->getVictim(candidates), &entry);

    rp->invalidate(entry.replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entry);
}

/// Fixture that tests victimization
class TreePLRUVictimizationTestF : public TreePLRUTestF
{
  protected:
    // The entries being victimized
    std::vector<gem5::ReplaceableEntry> entries;

    // The entries, in candidate form
    gem5::ReplacementCandidates candidates;

  public:
    void
    instantiateAllEntries(void)
    {
        for (auto &entry : entries) {
            entry.replacementData = rp->instantiateEntry();
            candidates.push_back(&entry);
        }
    }
    // We use 8 entries here
    TreePLRUVictimizationTestF() : TreePLRUTestF(), entries(num_leaves)
    {
        instantiateAllEntries();
    }
    TreePLRUVictimizationTestF(int num_entries)
        : TreePLRUTestF(num_entries), entries(num_leaves)
    {
        instantiateAllEntries();
    }
};

// Test only resetting one entry

// Test that if the entry at index 0 is the most recently used, the entry at
// index 4 will be the victim.
// The tree looks like this after candidate A is reset:
//    ____1____
//  __1__   __0__
// _1_ _0_ _0_ _0_
// A B C D E F G H
// The tree points to candidate E as the victim.

TEST_F(TreePLRUVictimizationTestF, GetVictimSingleResetLeftmost)
{
    rp->reset(entries[0].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[4]);
}

// Reset entry H and entry A will be victimized.
TEST_F(TreePLRUVictimizationTestF, GetVictimSingleResetRightmost)
{
    rp->reset(entries[7].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[0]);
}

// Reset entry B and entry E will be victimized.
TEST_F(TreePLRUVictimizationTestF, GetVictimSingleResetMiddle)
{
    rp->reset(entries[1].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[4]);
}

// Test resetting no entries. The tree's nodes should all have values of 0,
// pointing toward entry A at index 0.

TEST_F(TreePLRUVictimizationTestF, GetVictimNoReset)
{
    ASSERT_EQ(rp->getVictim(candidates), &entries[0]);
}

// Entries A, B, E, and F are reset, in that order. The victimized entry
// should be C at index 2.

TEST_F(TreePLRUVictimizationTestF, GetVictimHalfReset)
{
    rp->reset(entries[0].replacementData);
    rp->reset(entries[1].replacementData);
    rp->reset(entries[4].replacementData);
    rp->reset(entries[5].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[2]);
}

// Reset all entries once, starting from the leftmost side of the tree. The
// victimized entry should be A, as it was the least recently used.

TEST_F(TreePLRUVictimizationTestF, GetVictimAllReset)
{
    for (auto &entry : entries) {
        rp->reset(entry.replacementData);
    }

    ASSERT_EQ(rp->getVictim(candidates), &entries[0]);
}

// Reset all entries twice, starting from the leftmost side, then from the
// rightmost side. The victimized entry should be the rightmost entry,
// H, at index 7.

TEST_F(TreePLRUVictimizationTestF, GetVictimAllTwiceReset)
{
    for (auto &entry : entries) {
        rp->reset(entry.replacementData);
    }

    for (auto entry = entries.rbegin(); entry != entries.rend(); entry++) {
        rp->reset((*entry).replacementData);
    }

    ASSERT_EQ(rp->getVictim(candidates), &entries[7]);
}

// Test that when there is at least a single invalid entry, it will be
// selected during the victimization
// Taken directly from the FIFO SimObject unit tests, introduced by
// https://github.com/gem5/gem5/pull/1977

TEST_F(TreePLRUVictimizationTestF, GetVictimOneInvalid)
{
    for (auto &entry : entries) {
        // Validate all entries to start from a clean state
        for (auto &entry : entries) {
            rp->reset(entry.replacementData);
        }

        // Set one of the entries as invalid
        rp->invalidate(entry.replacementData);

        ASSERT_EQ(rp->getVictim(candidates), &entry);
    }
}

TEST_F(TreePLRUVictimizationTestF, TestTwoTrees)
{
    // Instantiate enough entries to fill two trees, then check that making
    // changes in one tree doesn't affect the other

    // we store entries from both trees in `entries`, so resize it to
    // accomodate all entries
    entries.resize(16);
    // ensure that the first `candidates` vector has the correct memory
    // locations
    for (int i = 0; i < 8; i++) {
        candidates[i] = &entries[i];
    }

    gem5::ReplacementCandidates second_candidates(8);
    for (int i = 8; i < 16; i++) {
        entries[i].replacementData = rp->instantiateEntry();
        second_candidates[i - 8] = &entries[i];
    }

    // if the trees are separate (as they should be), then the victim entry
    // should be entries[4]. If not, entries[8] will be selected.
    rp->reset(entries[0].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[4]);
    ASSERT_NE(rp->getVictim(candidates), &entries[8]);

    // If the entries are all (incorrectly) in the same tree, then entries[7]
    // will be selected.
    rp->reset(entries[8].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[4]);
    ASSERT_NE(rp->getVictim(candidates), &entries[7]);

    ASSERT_EQ(rp->getVictim(second_candidates), &entries[12]);
    ASSERT_NE(rp->getVictim(second_candidates), &entries[7]);
}

TEST_F(TreePLRUVictimizationTestF, TestMixedResetInvalidate)
{
    // If the entry is correctly invalidated, the entry at index 5 will be
    // selected.
    // If nothing happens when invalidate is called, the entry at index 4 will
    // be selected.
    rp->reset(entries[0].replacementData);
    rp->invalidate(entries[5].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[5]);
    ASSERT_NE(rp->getVictim(candidates), &entries[4]);

    rp->reset(entries[1].replacementData);
    rp->invalidate(entries[1].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[1]);
}

// Test the smallest tree size (2 leaves) and a large tree size (512 leaves)
class SmallTreePLRUVictimizationTestF : public TreePLRUVictimizationTestF
{
  public:
    SmallTreePLRUVictimizationTestF() : TreePLRUVictimizationTestF(2) {}
};

class LargeTreePLRUVictimizationTestF : public TreePLRUVictimizationTestF
{
  public:
    LargeTreePLRUVictimizationTestF() : TreePLRUVictimizationTestF(512) {}
};

TEST_F(SmallTreePLRUVictimizationTestF, TestSmallTree)
{
    // check that the test has been set up correctly
    ASSERT_EQ(entries.size(), 2);
    ASSERT_EQ(candidates.size(), 2);

    // check that resetting one entry will cause the other to be selected
    rp->reset(entries[0].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[1]);

    for (auto &entry : entries) {
        rp->reset(entry.replacementData);
    }
    rp->reset(entries[1].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[0]);

    // check invalidate
    rp->invalidate(entries[1].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[1]);
}

TEST_F(LargeTreePLRUVictimizationTestF, TestLargeTree)
{
    // check that the test has been set up correctly
    ASSERT_EQ(entries.size(), 512);
    ASSERT_EQ(candidates.size(), 512);

    rp->reset(entries[0].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[256]);

    rp->invalidate(entries[511].replacementData);
    ASSERT_EQ(rp->getVictim(candidates), &entries[511]);
}

// Taken directly from the FIFO SimObject unit tests, introduced by
// https://github.com/gem5/gem5/pull/1977

typedef TreePLRUTestF TreePLRUDeathTest;

TEST_F(TreePLRUDeathTest, InvalidateNull)
{
    ASSERT_DEATH(rp->invalidate(nullptr), "");
}

TEST_F(TreePLRUDeathTest, ResetNull)
{
    ASSERT_DEATH(rp->reset(nullptr), "");
}

TEST_F(TreePLRUDeathTest, TouchNull)
{
    ASSERT_DEATH(rp->touch(nullptr), "");
}

TEST_F(TreePLRUDeathTest, NoCandidates)
{
    gem5::ReplacementCandidates candidates;
    ASSERT_DEATH(rp->getVictim(candidates), "");
}
