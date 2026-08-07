# Copyright (c) 2026
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Neoverse V2 parameters mapped onto the pipelined O3 (PO3) model."""

from m5.objects import ArmPO3CPU
from m5.objects.PO3FUPool import PO3FUPool
from m5.objects.PO3IQUnit import PO3IQUnit

from . import neoverse_v2


class NeoverseV2PO3SimpleIntPool(PO3FUPool):
    FUList = [neoverse_v2.NeoverseV2_Simple_Int(count=2)]


class NeoverseV2PO3ComplexIntPool(PO3FUPool):
    FUList = [neoverse_v2.NeoverseV2_Complex_Int(count=1)]


class NeoverseV2PO3FPPool(PO3FUPool):
    FUList = [neoverse_v2.NeoverseV2_FP(count=2)]


class NeoverseV2PO3LoadPool(PO3FUPool):
    FUList = [neoverse_v2.NeoverseV2_Load(count=1)]


class NeoverseV2PO3LoadStorePool(PO3FUPool):
    FUList = [
        neoverse_v2.NeoverseV2_Load(count=1),
        neoverse_v2.NeoverseV2_Store(count=1),
    ]


class NeoverseV2PO3IQ0(PO3IQUnit):
    numEntries = 22
    fuPool = NeoverseV2PO3SimpleIntPool()


class NeoverseV2PO3IQ1(PO3IQUnit):
    numEntries = 22
    fuPool = NeoverseV2PO3SimpleIntPool()


class NeoverseV2PO3IQ2(PO3IQUnit):
    numEntries = 22
    fuPool = NeoverseV2PO3ComplexIntPool()


class NeoverseV2PO3IQ3(PO3IQUnit):
    numEntries = 22
    fuPool = NeoverseV2PO3ComplexIntPool()


class NeoverseV2PO3IQ4(PO3IQUnit):
    numEntries = 28
    fuPool = NeoverseV2PO3FPPool()


class NeoverseV2PO3IQ5(PO3IQUnit):
    numEntries = 28
    fuPool = NeoverseV2PO3FPPool()


class NeoverseV2PO3IQ6(PO3IQUnit):
    numEntries = 16
    fuPool = NeoverseV2PO3LoadPool()


class NeoverseV2PO3IQ7(PO3IQUnit):
    numEntries = 16
    fuPool = NeoverseV2PO3LoadStorePool()


class NeoverseV2PO3IQ8(PO3IQUnit):
    numEntries = 16
    fuPool = NeoverseV2PO3LoadStorePool()


class NeoverseV2PO3(ArmPO3CPU):
    # Backward and forward pipeline latencies.
    decodeToFetchDelay = 1
    renameToFetchDelay = 1
    iewToFetchDelay = 1
    commitToFetchDelay = 1
    renameToDecodeDelay = 1
    iewToDecodeDelay = 1
    commitToDecodeDelay = 1
    iewToRenameDelay = 1
    commitToRenameDelay = 1
    commitToIEWDelay = 1
    fetchToDecodeDelay = 3
    decodeToRenameDelay = 2
    renameToIEWDelay = 1
    renameToROBDelay = 1
    issueToExecuteDelay = 1
    iewToCommitDelay = 1

    fetchWidth = 6
    fetchBufferSize = 64
    decodeWidth = 6
    renameWidth = 8
    dispatchWidth = 8
    issueWidth = 8
    wbWidth = 8
    commitWidth = 8
    trapLatency = 13

    # Match PO3's explicit sub-pipeline bandwidths to the V2 pipeline.
    po3DecodeStageWidth = 6
    po3RenameStageWidth = 8
    po3IssueStageWidth = 8
    po3DispatchStageWidth = 8
    po3CommitStageWidth = 8
    po3MemAddrGenWidth = 3
    po3MemTLBLookupWidth = 3
    po3MemCacheAccessWidth = 3

    iqInsertionPolicy = "LeastLoaded"
    instQueues = [
        NeoverseV2PO3IQ0(),
        NeoverseV2PO3IQ1(),
        NeoverseV2PO3IQ2(),
        NeoverseV2PO3IQ3(),
        NeoverseV2PO3IQ4(),
        NeoverseV2PO3IQ5(),
        NeoverseV2PO3IQ6(),
        NeoverseV2PO3IQ7(),
        NeoverseV2PO3IQ8(),
    ]

    backComSize = 5
    forwardComSize = 5
    numPhysIntRegs = 213
    numPhysFloatRegs = 188
    numROBEntries = 320
    LQEntries = 175
    SQEntries = 80
    LSQDepCheckShift = 0
    LFSTSize = 1024
    SSITSize = "1024"

    branchPred = neoverse_v2.NeoverseV2_BP()
    mmu = neoverse_v2.NeoverseMMU()

    decoupledFrontEnd = True
    fetchTargetWidth = 64
    minInstSize = 4
    numFTQEntries = 32
