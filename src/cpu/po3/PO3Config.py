def set_po3_pipeline_defaults(cpu_cls):
    """Apply PO3's explicit pipelined out-of-order defaults."""

    cpu_cls.fetchToBacDelay = 1
    cpu_cls.decodeToFetchDelay = 1
    cpu_cls.renameToFetchDelay = 1
    cpu_cls.iewToFetchDelay = 1
    cpu_cls.commitToFetchDelay = 1

    cpu_cls.renameToDecodeDelay = 1
    cpu_cls.iewToDecodeDelay = 1
    cpu_cls.commitToDecodeDelay = 1

    cpu_cls.bacToFetchDelay = 1
    cpu_cls.fetchToDecodeDelay = 1
    cpu_cls.decodeToRenameDelay = 1
    cpu_cls.renameToIEWDelay = 1
    cpu_cls.issueToExecuteDelay = 1
    cpu_cls.iewToCommitDelay = 1

    cpu_cls.iewToRenameDelay = 1
    cpu_cls.commitToRenameDelay = 1
    cpu_cls.commitToIEWDelay = 1
    cpu_cls.renameToROBDelay = 1

    cpu_cls.fetchWidth = 4
    cpu_cls.decodeWidth = 4
    cpu_cls.renameWidth = 4
    cpu_cls.dispatchWidth = 4
    cpu_cls.issueWidth = 4
    cpu_cls.wbWidth = 4
    cpu_cls.commitWidth = 4
    cpu_cls.squashWidth = 4

    cpu_cls.fetchQueueSize = 16
    cpu_cls.numROBEntries = 128
    cpu_cls.LQEntries = 32
    cpu_cls.SQEntries = 32
    cpu_cls.backComSize = 5
    cpu_cls.forwardComSize = 5
    cpu_cls.decoupledFrontEnd = False

    cpu_cls.po3MemPipeline = True
    cpu_cls.po3MemAddrGenLatency = 1
    cpu_cls.po3MemTLBLookupLatency = 1
    cpu_cls.po3MemCacheAccessLatency = 1
    cpu_cls.po3MemWritebackLatency = 1
    cpu_cls.po3MemAddrGenWidth = 3
    cpu_cls.po3MemTLBLookupWidth = 3
    cpu_cls.po3MemCacheAccessWidth = 3

    cpu_cls.po3FetchPipeline = True
    cpu_cls.po3FetchPredTLBLatency = 1
    cpu_cls.po3FetchCacheAccessLatency = 1
    cpu_cls.po3FetchDecodeFeedLatency = 1
    cpu_cls.po3FetchPredTLBWidth = 1
    cpu_cls.po3FetchCacheAccessWidth = 1
    cpu_cls.po3FetchDecodeFeedWidth = 1

    cpu_cls.po3DecodePipeline = True
    cpu_cls.po3DecodeLatency = 1
    cpu_cls.po3DecodeStageWidth = 1
    cpu_cls.po3RenamePipeline = True
    cpu_cls.po3RenameLatency = 1
    cpu_cls.po3RenameStageWidth = 1
    cpu_cls.po3IssuePipeline = True
    cpu_cls.po3IssueLatency = 1
    cpu_cls.po3IssueStageWidth = 1
    cpu_cls.po3DispatchPipeline = True
    cpu_cls.po3DispatchLatency = 1
    cpu_cls.po3DispatchStageWidth = 1
    cpu_cls.po3IssueSelectPipeline = True
    cpu_cls.po3IssueSelectLatency = 1
    cpu_cls.po3RegReadPipeline = True
    cpu_cls.po3RegReadLatency = 1
    cpu_cls.po3ExecutePipeline = True
    cpu_cls.po3ExecuteLatency = 1
    cpu_cls.po3WritebackPipeline = True
    cpu_cls.po3WritebackLatency = 1
    cpu_cls.po3CommitPipeline = True
    cpu_cls.po3CommitLatency = 1
    cpu_cls.po3CommitStageWidth = 2
    cpu_cls.po3RedirectPipeline = True
    cpu_cls.po3RedirectLatency = 1
    cpu_cls.po3PCGenPipeline = True
    cpu_cls.po3PCGenLatency = 1

    return cpu_cls
