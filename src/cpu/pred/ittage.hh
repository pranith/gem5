#ifndef __CPU_PRED_ITTAGE_HH__
#define __CPU_PRED_ITTAGE_HH__

#include "cpu/pred/indirect.hh"
#include "params/ITTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

class ITTAGE: public IndirectPredictor
{

#define STEP1 3
#define STEP2 11
#define HISTBUFFERLENGTH 4096 // Size of the history circular buffer
#define LOGG 12
#define LOGTICK 6 // For management of the reset of useful counters
#define LOGSPEC 6

  private:

    // ITTAGE global table entry
    class ITTageEntry
    {
      public:
        int8_t   ctr;
        uint64_t tag;
        uint8_t  u;
        Addr target; // 25 bits (18 offset + 7-bit region pointer)
        ITTageEntry() : ctr(0), tag(0), u(0), target(0) {}
    };

    struct ITTageBranchInfo
    {
        Addr predTarget; // Prediction
        Addr altTarget;  // Alternate prediction
        Addr pred;       // LTTAGE prediction
        Addr longestMatchPredTarget;
        Addr branchPC;

        int hitBank;
        int altBank;

        bool taken;
        bool condBranch;

        // Pointer to dynamically allocated storage
        // to save table indices and folded histories.
        // To do one call to new instead of 5.
        int * storage;

        // Pointers to actual saved array within the dynamically
        // allocated storage.
        int * tableIndices;
        int * tableTags;
        int * ci;
        int * ct0;
        int * ct1;

        ITTageBranchInfo(int sz)
            : predTarget(0),
              altTarget(0),
              pred(0),
              longestMatchPredTarget(0),
              branchPC(0),
              hitBank(0),
              altBank(0),
              taken(false),
              condBranch(false)
        {
            storage = new int [sz * 5];
            tableIndices = storage;
            tableTags = storage + sz;
            ci = tableTags + sz;
            ct0 = ci + sz;
            ct1 = ct0 + sz;
        }
    };

    // Class for storing speculative predictions: i.e. provided by a table
    // entry that has already provided a still speculative prediction
    // IUM: Immediate update mimicker
    class IUMEntry
    {
      public:
        uint64_t tag;
        Addr pred;
        IUMEntry() : pred(0)
        {
        }

        ~IUMEntry()
        {
        }
    };

    void recordTarget(InstSeqNum seq_num,  void * indirect_history,
                      const PCStateBase& target, ThreadID tid);
    void genIndirectInfo(ThreadID tid, void* &i_history);
    void deleteIndirectInfo(ThreadID tid, void * indirect_history);
    void historyUpdate(ThreadID tid, Addr branch_pc, bool taken,
                       void * bp_history, const StaticInstPtr & inst,
                       Addr target);


    bool lookup(ThreadID tid, Addr pc, PCStateBase* &br_target,
                ITTageBranchInfo * &bp_history);
    class FoldedHistorytmp
    {
        // This is the cyclic shift register for folding
        // a long global history into a smaller number of bits;
        // see P. Michaud's PPM-like predictor at CBP-1
      public:
        unsigned comp;
        int compLength;
        int origLength;
        int outpoint;

        FoldedHistorytmp()
        {
        }

        void
        init(int original_length, int compressed_length)
        {
            comp = 0;
            origLength = original_length;
            compLength = compressed_length;
            outpoint = origLength % compLength;
        }

        void
        update(uint8_t * h, int pt)
        {
            comp = (comp << 1) | h[pt & (HISTBUFFERLENGTH - 1)];
            comp ^= h[(pt + origLength) & (HISTBUFFERLENGTH - 1)] << outpoint;
            comp ^= (comp >> compLength);
            comp &= (1 << compLength) - 1;
        }
    };

    int tick; // Control counter for the resetting of useful bits

    /**
     * ITTAGE target region table entry
     *
     */
    class RegionEntry
    {
      public:
        uint64_t region; // 14 bits (not any more, now 46 bits)
        int8_t u;        // 1 bit
        RegionEntry() : region(0), u(0) {}
    };

    struct HistoryEntry
    {
        HistoryEntry(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num)
            : pcAddr(br_addr), targetAddr(tgt_addr), seqNum(seq_num) { }
        Addr pcAddr;
        Addr targetAddr;
        InstSeqNum seqNum;
    };

    /**
     * Per-thread history
     *
     */
    class ThreadHistory
    {
      public:
        // Speculative branch history (circular buffer)
        uint8_t ghist[HISTBUFFERLENGTH];

        // For management at fetch time
        int fetchPtGhist;
        FoldedHistorytmp * fetchComputeIndices;
        FoldedHistorytmp * fetchComputeTags[2];

        // For management at retire time
        int retirePtGhist;
        FoldedHistorytmp * retireComputeIndices;
        FoldedHistorytmp * retireComputeTags[2];

        std::deque<HistoryEntry> pathHist;
        unsigned headHistEntry;
    };

 
    std::vector<ThreadHistory> threadHistory;
    // "Use alternate prediction on weak predictions": a 4-bit counter to
    // determine whether the newly allocated entries should be considered
    // as valid or not for delivering the prediction
    int8_t useAltOnNA;

    // For the IUM
    int ptIumRetire;
    int ptIumFetch;
    IUMEntry * IUMTable;

    // Target region tables
    RegionEntry * regionTable;

    const unsigned nHistoryTables;
    std::vector<unsigned> tagTableTagWidths;
    std::vector<int> logTagTableSizes;
    ITTageEntry ** gtable;
    std::vector<int> histLengths;
    int * tableIndices;
    int * tableTags;


  public:

    ITTAGE(const ITTAGEParams &params);

    const PCStateBase* lookup(ThreadID tid, InstSeqNum sn,
                              Addr pc, void * &i_history) override;

    void update(ThreadID tid, InstSeqNum sn, Addr pc, bool squash,
                bool taken, const PCStateBase& target,
                BranchType br_type, void * &i_history) override;

    void
    updateBrIndirect(ThreadID tid, InstSeqNum sn, Addr branch_pc, bool squash,
                     bool taken, const Addr& target,
                     BranchType br_type, void * &i_history);

    virtual void squash(ThreadID tid, InstSeqNum sn, void * &i_history) override
    {
        if (i_history == nullptr) {
            return;
        }

        ITTageBranchInfo *history = static_cast<ITTageBranchInfo *>(i_history);

        // delete history
        delete history;
        i_history = nullptr;
    }

    void commit(ThreadID tid, InstSeqNum sn, void * &i_history) override;

  private:

    int getRandom() const;

    // gindex computes a full hash of pc and ghist
    int gindex(ThreadID tid, Addr pc, int bank, bool at_fetch);

    // Tag computation
    uint16_t gtag(ThreadID tid, unsigned int pc, int bank, bool at_fetch);

    void tagePredict(ThreadID tid, Addr branch_pc, ITTageBranchInfo * bi);

    void calculateIndicesAndTags(ThreadID tid, Addr branch_pc,
                                 ITTageBranchInfo * bi, bool at_fetch);

    Addr predictIUM(ITTageBranchInfo * bi);

    void IUMUpdate(Addr target, void * b);

    // Update fetch histories
    //void fetchHistoryUpdate(Addr pc, uint16_t br_type, bool taken,
    //                        Addr target, ThreadID tid);
    void historyUpdate(ThreadID tid, Addr branch_pc, bool taken, void * b,
                       Addr target, bool at_fetch);

    // Predictor update
    void updateBrIndirect(Addr pc, uint16_t br_type, bool taken, Addr target,
                          ThreadID tid, void * indirect_history);
};

} // namespace branch_prediction

} // namespace gem5
#endif // __CPU_PRED_ITTAGE
