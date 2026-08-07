# Copyright (c) 2016, 2019, 2025 Arm Limited
# Copyright (c) 2022-2023 The University of Edinburgh
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2005-2007 The Regents of The University of Michigan
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

from m5.citations import add_citation
from m5.defines import buildEnv
from m5.objects.BaseCPU import BaseCPU

# from m5.objects.PO3Checker import PO3Checker
from m5.objects.BranchPredictor import *
from m5.objects.IndexingPolicies import *
from m5.objects.PO3IQUnit import *
from m5.objects.ReplacementPolicies import *
from m5.objects.SMT import *
from m5.params import *
from m5.proxy import *
from m5.SimObject import *


class BasePO3CPU(BaseCPU):
    type = "BasePO3CPU"
    cxx_class = "gem5::po3::CPU"
    cxx_header = "cpu/po3/cpu.hh"

    @classmethod
    def memory_mode(cls):
        return "timing"

    @classmethod
    def require_caches(cls):
        return True

    @classmethod
    def support_take_over(cls):
        return True

    activity = Param.Unsigned(0, "Initial count")

    cacheStorePorts = Param.Unsigned(
        1, "Merge-buffer D-cache write ports available per cycle"
    )
    cacheLoadPorts = Param.Unsigned(
        3, "One D-cache read port mapped to each memory pipe"
    )
    cacheBanks = Param.Unsigned(
        1,
        "Number of cache-line-interleaved D-cache banks used to model "
        "read/write bank conflicts",
    )

    po3MemPipeline = Param.Bool(
        True, "Enable PO3 cycle-by-cycle load/store execute sub-pipeline"
    )
    po3MemAddrGenLatency = Param.Cycles(
        1, "PO3 address generation stage latency"
    )
    po3MemTLBLookupLatency = Param.Cycles(
        1, "PO3 TLB lookup stage latency after translation completes"
    )
    po3MemCacheAccessLatency = Param.Cycles(
        1, "PO3 cache access issue stage latency"
    )
    po3MemWritebackLatency = Param.Cycles(
        1, "PO3 load writeback stage latency after cache response"
    )
    po3MemAddrGenWidth = Param.Unsigned(
        1, "PO3 address generation stage issue width"
    )
    po3MemTLBLookupWidth = Param.Unsigned(
        1, "PO3 TLB lookup stage issue width"
    )
    po3MemCacheAccessWidth = Param.Unsigned(
        1, "PO3 cache access stage issue width"
    )

    po3FetchPipeline = Param.Bool(
        True, "Enable PO3 cycle-by-cycle fetch sub-pipeline"
    )
    po3FetchPredTLBLatency = Param.Cycles(
        1, "PO3 fetch predictor/BTB and iTLB lookup stage latency"
    )
    po3FetchCacheAccessLatency = Param.Cycles(
        1, "PO3 fetch cache access issue stage latency"
    )
    po3FetchDecodeFeedLatency = Param.Cycles(
        1, "PO3 fetch stage latency before feeding decode"
    )
    po3FetchPredTLBWidth = Param.Unsigned(
        1, "PO3 fetch predictor/BTB and iTLB stage width"
    )
    po3FetchCacheAccessWidth = Param.Unsigned(
        1, "PO3 fetch cache access issue stage width"
    )
    po3FetchDecodeFeedWidth = Param.Unsigned(
        1, "PO3 fetch-to-decode feed stage width"
    )

    po3DecodePipeline = Param.Bool(
        True, "Enable PO3 explicit decode pipeline stage"
    )
    po3DecodeLatency = Param.Cycles(
        1, "PO3 decode stage latency before rename"
    )
    po3DecodeStageWidth = Param.Unsigned(1, "PO3 decode stage retire width")
    po3RenamePipeline = Param.Bool(
        True, "Enable PO3 explicit rename pipeline stage"
    )
    po3RenameLatency = Param.Cycles(1, "PO3 rename stage latency before issue")
    po3RenameStageWidth = Param.Unsigned(1, "PO3 rename stage retire width")
    po3IssuePipeline = Param.Bool(
        True, "Enable PO3 explicit issue/dispatch pipeline stage"
    )
    po3IssueLatency = Param.Cycles(
        1, "PO3 issue stage latency before dispatch into IQ/LSQ"
    )
    po3IssueStageWidth = Param.Unsigned(1, "PO3 issue stage retire width")
    po3DispatchPipeline = Param.Bool(
        True, "Enable PO3 explicit dispatch/allocation pipeline stage"
    )
    po3DispatchLatency = Param.Cycles(
        1, "PO3 dispatch/allocation stage latency"
    )
    po3DispatchStageWidth = Param.Unsigned(
        1, "PO3 dispatch/allocation stage retire width"
    )
    po3IssueSelectPipeline = Param.Bool(
        True, "Enable PO3 explicit issue-select pipeline stage"
    )
    po3IssueSelectLatency = Param.Cycles(
        1, "PO3 issue-select/scheduler stage latency"
    )
    po3RegReadPipeline = Param.Bool(
        True, "Enable PO3 explicit register-read/operand-collect stage"
    )
    po3RegReadLatency = Param.Cycles(
        1, "PO3 register-read/operand-collect stage latency"
    )
    po3ExecutePipeline = Param.Bool(
        True, "Enable PO3 explicit non-memory execute stage"
    )
    po3ExecuteLatency = Param.Cycles(1, "PO3 non-memory execute stage latency")
    po3WritebackPipeline = Param.Bool(
        True, "Enable PO3 explicit non-memory writeback stage"
    )
    po3WritebackLatency = Param.Cycles(
        1, "PO3 non-memory writeback stage latency"
    )
    po3CommitPipeline = Param.Bool(
        True, "Enable PO3 explicit commit/retire stage"
    )
    po3CommitLatency = Param.Cycles(1, "PO3 commit/retire stage latency")
    po3CommitStageWidth = Param.Unsigned(2, "PO3 commit/retire stage width")
    po3RedirectPipeline = Param.Bool(
        True, "Enable PO3 explicit branch redirect/recovery stage"
    )
    po3RedirectLatency = Param.Cycles(
        1, "PO3 branch redirect/recovery stage latency"
    )
    po3PCGenPipeline = Param.Bool(
        True, "Enable PO3 explicit PC-generation/FTQ stage"
    )
    po3PCGenLatency = Param.Cycles(1, "PO3 PC-generation/FTQ stage latency")

    # Backward pipeline delays
    fetchToBacDelay = Param.Cycles(1, "Fetch to Branch address calc. delay")
    decodeToFetchDelay = Param.Cycles(1, "Decode to fetch delay")
    renameToFetchDelay = Param.Cycles(1, "Rename to fetch delay")
    iewToFetchDelay = Param.Cycles(1, "Issue/Execute/Writeback to fetch delay")
    commitToFetchDelay = Param.Cycles(1, "Commit to fetch delay")
    fetchWidth = Param.Unsigned(8, "Fetch width")
    fetchBufferSize = Param.Unsigned(64, "Fetch buffer size in bytes")
    fetchQueueSize = Param.Unsigned(
        32, "Fetch queue size in micro-ops per-thread"
    )

    renameToDecodeDelay = Param.Cycles(1, "Rename to decode delay")
    iewToDecodeDelay = Param.Cycles(
        1, "Issue/Execute/Writeback to decode delay"
    )
    commitToDecodeDelay = Param.Cycles(1, "Commit to decode delay")

    # Forward pipeline delays
    bacToFetchDelay = Param.Cycles(1, "Branch address calc. to fetch delay")
    fetchToDecodeDelay = Param.Cycles(1, "Fetch to decode delay")
    decodeWidth = Param.Unsigned(8, "Decode width")

    iewToRenameDelay = Param.Cycles(
        1, "Issue/Execute/Writeback to rename delay"
    )
    commitToRenameDelay = Param.Cycles(1, "Commit to rename delay")
    decodeToRenameDelay = Param.Cycles(1, "Decode to rename delay")
    renameWidth = Param.Unsigned(8, "Rename width")

    commitToIEWDelay = Param.Cycles(
        1, "Commit to Issue/Execute/Writeback delay"
    )
    renameToIEWDelay = Param.Cycles(
        2, "Rename to Issue/Execute/Writeback delay"
    )
    issueToExecuteDelay = Param.Cycles(
        1, "Issue to execute delay (internal to the IEW stage)"
    )
    dispatchWidth = Param.Unsigned(8, "Dispatch width")
    issueWidth = Param.Unsigned(8, "Issue width")
    wbWidth = Param.Unsigned(8, "Writeback width")

    iewToCommitDelay = Param.Cycles(
        1, "Issue/Execute/Writeback to commit delay"
    )
    renameToROBDelay = Param.Cycles(1, "Rename to reorder buffer delay")
    commitWidth = Param.Unsigned(8, "Commit width")
    squashWidth = OptionalParam.Unsigned(
        "Squash width. If unspecified all instructions are "
        "squashed instantly within one cycle.",
    )
    trapLatency = Param.Cycles(13, "Trap latency")
    fetchTrapLatency = Param.Cycles(1, "Fetch trap latency")

    backComSize = Param.Unsigned(
        5, "Time buffer size for backwards communication"
    )
    forwardComSize = Param.Unsigned(
        5, "Time buffer size for forward communication"
    )

    LQEntries = Param.Unsigned(32, "Number of load queue entries")
    SQEntries = Param.Unsigned(32, "Number of store queue entries")
    LSQDepCheckShift = Param.Unsigned(
        4, "Number of places to shift addr before check"
    )
    useMergeBuffer = Param.Bool(False, "Use merge buffer")
    mergeBufferEntries = Param.Unsigned(
        32, "Number of entries in " "the merge buffer"
    )
    mergeBufferPrefetch = Param.Bool(
        True,
        "Prefetch cache line on merge buffer allocation to speed up drains",
    )
    storeDeallocateWidth = Param.Unsigned(
        2, "Number of stores that can retire from the store queue per cycle"
    )
    mergeBufferRetireCycles = Param.Cycles(
        16,
        "Cycles a merge buffer entry remains in MERGING state before "
        "retiring and draining",
    )
    mbRetireWhenFullValid = Param.Bool(
        False,
        "Retire merge buffer entries immediately when all bytes are valid",
    )
    mergeBufferMaxUnretire = Param.Unsigned(
        2,
        "Max times a merge buffer entry can unretire from RETIRED to MERGING",
    )
    mergeBufferResetRetireOnMerge = Param.Bool(
        False,
        "If true, reset the retire timer when a new store merges "
        "into an existing merge buffer entry",
    )
    mergeBufferRetireResetCycles = Param.Cycles(
        64, "Retire window used when resetting an entry on merge"
    )
    optimizeStoreRelease = Param.Bool(
        False,
        "Enable optimized store-release handling with release buffer tracking",
    )
    stlfLoadsBypassMBDrain = Param.Bool(
        True,
        "Allow STLF loads at ROB head to bypass MB version stall",
    )
    LSQCheckLoads = Param.Bool(
        True,
        "Should dependency violations be checked for "
        "loads & stores or just stores",
    )
    store_set_clear_period = Param.Unsigned(
        250000,
        "Number of load/store insts before the dep predictor "
        "should be invalidated",
    )
    memory_dep_predictor = Param.String(
        "store_set",
        "Memory dependence predictor: 'store_set' or 'scbf'",
    )
    scbf_num_segments = Param.Unsigned(
        4, "Number of independently hashed SCBF counter/bit segments"
    )
    scbf_entries_per_segment = Param.Unsigned(
        512, "Number of counters and presence bits in each SCBF segment"
    )
    scbf_history_entries = Param.Unsigned(
        1024,
        "Number of recent violating PC-pair signatures retained by SCBF",
    )
    LFSTSize = Param.Unsigned(1024, "Last fetched store table size")
    SSITSize = Param.MemorySize("1024", "Store set ID table size")
    SSITAssoc = Param.Unsigned(1, "SSIT table associativity")
    SSITReplPolicy = Param.BaseReplacementPolicy(
        LRURP(), "SSIT replacement policy"
    )
    SSITIndexingPolicy = Param.BaseIndexingPolicy(
        SetAssociative(
            size=Parent.SSITSize * 4,
            assoc=Parent.SSITAssoc,
            entry_size=4,
        ),
        "SSIT indexing policy",
    )

    numRobs = Param.Unsigned(1, "Number of Reorder Buffers")

    numPhysIntRegs = Param.Unsigned(
        256, "Number of physical integer registers"
    )
    numPhysFloatRegs = Param.Unsigned(
        256, "Number of physical floating point registers"
    )
    numPhysVecRegs = Param.Unsigned(256, "Number of physical vector registers")
    numPhysVecPredRegs = Param.Unsigned(
        32, "Number of physical predicate registers"
    )
    numPhysMatRegs = Param.Unsigned(2, "Number of physical matrix registers")
    # most ISAs don't use condition-code regs, so default is 0
    numPhysCCRegs = Param.Unsigned(0, "Number of physical cc registers")
    instQueues = VectorParam.PO3IQUnit(PO3IQUnit(), "Vector of IQs")
    iqInsertionPolicy = Param.IQInsertionPolicy(
        "FirstMatch", "Policy for inserting instructions to IQs"
    )
    numROBEntries = Param.Unsigned(192, "Number of reorder buffer entries")

    smtNumFetchingThreads = Param.Unsigned(1, "SMT Number of Fetching Threads")
    smtFetchPolicy = Param.SMTFetchPolicy("RoundRobin", "SMT Fetch policy")
    smtLSQPolicy = Param.SMTQueuePolicy(
        "Partitioned", "SMT LSQ Sharing Policy"
    )
    smtLSQThreshold = Param.Int(100, "SMT LSQ Threshold Sharing Parameter")
    smtROBPolicy = Param.SMTQueuePolicy(
        "Partitioned", "SMT ROB Sharing Policy"
    )
    smtROBThreshold = Param.Int(100, "SMT ROB Threshold Sharing Parameter")
    smtCommitPolicy = Param.CommitPolicy("RoundRobin", "SMT Commit Policy")

    branchPred = Param.BranchPredictor(
        BranchPredictor(
            conditionalBranchPred=TournamentBP(numThreads=Parent.numThreads)
        ),
        "Branch Predictor",
    )
    needsTSO = Param.Bool(False, "Enable TSO Memory model")

    recvRespThrottling = Param.Bool(
        False, "Enable load receive response throttling in the LSQ"
    )
    recvRespMaxCachelines = Param.Unsigned(
        1,
        "Maximum number of different receive response cachelines per cycle",
    )
    recvRespBufferSize = Param.Unsigned(
        64, "Maximum number of receive response bytes per cycle"
    )

    ## Parameters for decoupled front-end
    decoupledFrontEnd = Param.Bool(False, "Enables the decoupled front-end")
    numFTQEntries = Param.Unsigned(
        8,
        "Number of entries in the Fetch target queue. (only used for "
        "decoupled front-end)",
    )
    minInstSize = Param.Unsigned(
        1,
        "Minimum instruction size (bytes). Determines the granularity "
        "of the instruction minimum search width per cycle",
    )
    fetchTargetWidth = Param.Unsigned(
        32,
        "Max width (bytes) of Fetch target. "
        "Determines the maximum search width per cycle",
    )
    maxFTPerCycle = Param.Unsigned(4, "Max number of FT created per cycle")
    maxTakenPredPerCycle = Param.Unsigned(
        1, "Max number of taken predictions per cycle"
    )


add_citation(
    BasePO3CPU,
    """@inproceedings{10.1145/3613424.3614258,
  author    = {Schall, David and
               Sandberg, Andreas and
               Grot, Boris},
  title     = {Warming Up a Cold Front-End with Ignite},
  year      = {2023},
  publisher = {Association for Computing Machinery},
  address   = {Toronto, ON, Canada},
  doi       = {10.1145/3613424.3614258},
  booktitle = {Proceedings of the 56th Annual IEEE/ACM International Symposium on Microarchitecture (MICRO '23)},
  series    = {MICRO'23}
}
""",
)
