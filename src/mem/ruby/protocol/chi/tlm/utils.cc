/*
 * Copyright (c) 2023 ARM Limited
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

#include "mem/ruby/protocol/chi/tlm/utils.hh"

#include "base/bitfield.hh"
#include "base/logging.hh"

namespace gem5 {

namespace tlm::chi {

namespace {

using namespace ARM::CHI;

std::string
reqOpcodeToName(ReqOpcode req_opcode)
{
    switch (req_opcode) {
      case REQ_OPCODE_REQ_LCRD_RETURN: return "ReqLCrdReturn";
      case REQ_OPCODE_READ_SHARED: return "ReadShared";
      case REQ_OPCODE_READ_CLEAN: return "ReadClean";
      case REQ_OPCODE_READ_ONCE: return "ReadOnce";
      case REQ_OPCODE_READ_NO_SNP: return "ReadNoSnp";
      case REQ_OPCODE_PCRD_RETURN: return "PCrdReturn";
      case REQ_OPCODE_READ_UNIQUE: return "ReadUnique";
      case REQ_OPCODE_CLEAN_SHARED: return "CleanShared";
      case REQ_OPCODE_CLEAN_INVALID: return "CleanInvalid";
      case REQ_OPCODE_MAKE_INVALID: return "MakeInvalid";
      case REQ_OPCODE_CLEAN_UNIQUE: return "CleanUnique";
      case REQ_OPCODE_MAKE_UNIQUE: return "MakeUnique";
      case REQ_OPCODE_EVICT: return "Evict";
      case REQ_OPCODE_EO_BARRIER: return "EoBarrier";
      case REQ_OPCODE_EC_BARRIER: return "EcBarrier";
      case REQ_OPCODE_READ_NO_SNP_SEP: return "ReadNoSnpSep";
      case REQ_OPCODE_CLEAN_SHARED_PERSIST_SEP: return "CleanSharedPersistSep";
      case REQ_OPCODE_DVM_OP: return "DvmOp";
      case REQ_OPCODE_WRITE_EVICT_FULL: return "WriteEvictFull";
      case REQ_OPCODE_WRITE_CLEAN_PTL: return "WriteCleanPtl";
      case REQ_OPCODE_WRITE_CLEAN_FULL: return "WriteCleanFull";
      case REQ_OPCODE_WRITE_UNIQUE_PTL: return "WriteUniquePtl";
      case REQ_OPCODE_WRITE_UNIQUE_FULL: return "WriteUniqueFull";
      case REQ_OPCODE_WRITE_BACK_PTL: return "WriteBackPtl";
      case REQ_OPCODE_WRITE_BACK_FULL: return "WriteBackFull";
      case REQ_OPCODE_WRITE_NO_SNP_PTL: return "WriteNoSnpPtl";
      case REQ_OPCODE_WRITE_NO_SNP_FULL: return "WriteNoSnpFull";
      case REQ_OPCODE_WRITE_UNIQUE_FULL_STASH: return "WriteUniqueFullStash";
      case REQ_OPCODE_WRITE_UNIQUE_PTL_STASH: return "WriteUniquePtlStash";
      case REQ_OPCODE_STASH_ONCE_SHARED: return "StashOnceShared";
      case REQ_OPCODE_STASH_ONCE_UNIQUE: return "StashOnceUnique";
      case REQ_OPCODE_READ_ONCE_CLEAN_INVALID: return "ReadOnceCleanInvalid";
      case REQ_OPCODE_READ_ONCE_MAKE_INVALID: return "ReadOnceMakeInvalid";
      case REQ_OPCODE_READ_NOT_SHARED_DIRTY: return "ReadNotSharedDirty";
      case REQ_OPCODE_CLEAN_SHARED_PERSIST: return "CleanSharedPersist";
      case REQ_OPCODE_ATOMIC_STORE_ADD: return "AtomicStoreAdd";
      case REQ_OPCODE_ATOMIC_STORE_CLR: return "AtomicStoreClr";
      case REQ_OPCODE_ATOMIC_STORE_EOR: return "AtomicStoreEor";
      case REQ_OPCODE_ATOMIC_STORE_SET: return "AtomicStoreSet";
      case REQ_OPCODE_ATOMIC_STORE_SMAX: return "AtomicStoreSmax";
      case REQ_OPCODE_ATOMIC_STORE_SMIN: return "AtomicStoreSmin";
      case REQ_OPCODE_ATOMIC_STORE_UMAX: return "AtomicStoreUmax";
      case REQ_OPCODE_ATOMIC_STORE_UMIN: return "AtomicStoreUmin";
      case REQ_OPCODE_ATOMIC_LOAD_ADD: return "AtomicLoadAdd";
      case REQ_OPCODE_ATOMIC_LOAD_CLR: return "AtomicLoadClr";
      case REQ_OPCODE_ATOMIC_LOAD_EOR: return "AtomicLoadEor";
      case REQ_OPCODE_ATOMIC_LOAD_SET: return "AtomicLoadSet";
      case REQ_OPCODE_ATOMIC_LOAD_SMAX: return "AtomicLoadSmax";
      case REQ_OPCODE_ATOMIC_LOAD_SMIN: return "AtomicLoadSmin";
      case REQ_OPCODE_ATOMIC_LOAD_UMAX: return "AtomicLoadUmax";
      case REQ_OPCODE_ATOMIC_LOAD_UMIN: return "AtomicLoadUmin";
      case REQ_OPCODE_ATOMIC_SWAP: return "AtomicSwap";
      case REQ_OPCODE_ATOMIC_COMPARE: return "AtomicCompare";
      case REQ_OPCODE_PREFETCH_TGT: return "PrefetchTgt";
      case REQ_OPCODE_MAKE_READ_UNIQUE: return "MakeReadUnique";
      case REQ_OPCODE_WRITE_EVICT_OR_EVICT: return "WriteEvictOrEvict";
      case REQ_OPCODE_WRITE_UNIQUE_ZERO: return "WriteUniqueZero";
      case REQ_OPCODE_WRITE_NO_SNP_ZERO: return "WriteNoSnpZero";
      case REQ_OPCODE_STASH_ONCE_SEP_SHARED: return "StashOnceSepShared";
      case REQ_OPCODE_STASH_ONCE_SEP_UNIQUE: return "StashOnceSepUnique";
      case REQ_OPCODE_READ_PREFER_UNIQUE: return "ReadPreferUnique";
      case REQ_OPCODE_WRITE_NO_SNP_FULL_CLEAN_SH: return "WriteNoSnpFullCleanSh";
      default: return std::string{};
    }
}

std::string
snpOpcodeToName(SnpOpcode snp_opcode)
{
    switch (snp_opcode) {
      case SNP_OPCODE_SNP_LCRD_RETURN: return "SnpLcrdReturn";
      case SNP_OPCODE_SNP_SHARED: return "SnpShared";
      case SNP_OPCODE_SNP_CLEAN: return "SnpClean";
      case SNP_OPCODE_SNP_ONCE: return "SnpOnce";
      case SNP_OPCODE_SNP_NOT_SHARED_DIRTY: return "SnpNotSharedDirty";
      case SNP_OPCODE_SNP_UNIQUE_STASH: return "SnpUniqueStash";
      case SNP_OPCODE_SNP_MAKE_INVALID_STASH: return "SnpMakeInvalidStash";
      case SNP_OPCODE_SNP_UNIQUE: return "SnpUnique";
      case SNP_OPCODE_SNP_CLEAN_SHARED: return "SnpCleanShared";
      case SNP_OPCODE_SNP_CLEAN_INVALID: return "SnpCleanInvalid";
      case SNP_OPCODE_SNP_MAKE_INVALID: return "SnpMakeInvalid";
      case SNP_OPCODE_SNP_STASH_UNIQUE: return "SnpStashUnique";
      case SNP_OPCODE_SNP_STASH_SHARED: return "SnpStashShared";
      case SNP_OPCODE_SNP_DVM_OP: return "SnpDvmOp";
      case SNP_OPCODE_SNP_QUERY: return "SnpQuery";
      case SNP_OPCODE_SNP_SHARED_FWD: return "SnpSharedFwd";
      case SNP_OPCODE_SNP_CLEAN_FWD: return "SnpCleanFwd";
      case SNP_OPCODE_SNP_ONCE_FWD: return "SnpOnceFwd";
      case SNP_OPCODE_SNP_NOT_SHARED_DIRTY_FWD: return "SnpNotSharedDirtyFwd";
      case SNP_OPCODE_SNP_PREFER_UNIQUE: return "SnpPreferUnique";
      case SNP_OPCODE_SNP_PREFER_UNIQUE_FWD: return "SnpPreferUniqueFwd";
      case SNP_OPCODE_SNP_UNIQUE_FWD: return "SnpUniqueFwd";
      default: return std::string{};
    }
}

std::string
datOpcodeToName(DatOpcode dat_opcode)
{
    switch (dat_opcode) {
      case DAT_OPCODE_DAT_LCRD_RETURN: return "DatLcrdReturn";
      case DAT_OPCODE_SNP_RESP_DATA: return "SnpRespData";
      case DAT_OPCODE_COPY_BACK_WR_DATA: return "CopyBackWrData";
      case DAT_OPCODE_NON_COPY_BACK_WR_DATA: return "NonCopyBackWrData";
      case DAT_OPCODE_COMP_DATA: return "CompData";
      case DAT_OPCODE_SNP_RESP_DATA_PTL: return "SnpRespDataPtl";
      case DAT_OPCODE_SNP_RESP_DATA_FWDED: return "SnpRespDataFwded";
      case DAT_OPCODE_WRITE_DATA_CANCEL: return "WriteDataCancel";
      case DAT_OPCODE_DATA_SEP_RESP: return "DataSepResp";
      case DAT_OPCODE_NCB_WR_DATA_COMP_ACK: return "NcbWrDataCompAck";
      default: return std::string{};
    }
}

std::string
rspOpcodeToName(RspOpcode rsp_opcode)
{
    switch (rsp_opcode) {
      case RSP_OPCODE_RSP_LCRD_RETURN: return "RspLcrdReturn";
      case RSP_OPCODE_SNP_RESP: return "SnpResp";
      case RSP_OPCODE_COMP_ACK: return "CompAck";
      case RSP_OPCODE_RETRY_ACK: return "RetryAck";
      case RSP_OPCODE_COMP: return "OpcodeComp";
      case RSP_OPCODE_COMP_DBID_RESP: return "CompDbidResp";
      case RSP_OPCODE_DBID_RESP: return "DbidResp";
      case RSP_OPCODE_PCRD_GRANT: return "PcrdGrant";
      case RSP_OPCODE_READ_RECEIPT: return "ReadReceipt";
      case RSP_OPCODE_SNP_RESP_FWDED: return "SnpRespFwded";
      case RSP_OPCODE_TAG_MATCH: return "TagMatch";
      case RSP_OPCODE_RESP_SEP_DATA: return "RespSepData";
      case RSP_OPCODE_PERSIST: return "Persist";
      case RSP_OPCODE_COMP_PERSIST: return "CompPersist";
      case RSP_OPCODE_DBID_RESP_ORD: return "DbidRespOrd";
      case RSP_OPCODE_STASH_DONE: return "StashDone";
      case RSP_OPCODE_COMP_STASH_DONE: return "CompStashDone";
      case RSP_OPCODE_COMP_CMO: return "CompCMO";
      default: return std::string{};
    }
}

std::string
phaseToOpcodeName(const Phase &phase)
{
    switch (phase.channel) {
      case CHANNEL_REQ:
        return reqOpcodeToName(phase.req_opcode);
      case CHANNEL_SNP:
        return snpOpcodeToName(phase.snp_opcode);
      case CHANNEL_DAT:
        return datOpcodeToName(phase.dat_opcode);
      case CHANNEL_RSP:
        return rspOpcodeToName(phase.rsp_opcode);
      default:
        break;
      }
      return std::string{};
}

std::string
phaseToChannelName(const Phase &phase)
{
    switch (phase.channel) {
      case CHANNEL_REQ:
        return "REQ";
      case CHANNEL_SNP:
        return "SNP";
      case CHANNEL_DAT:
        return "DAT";
      case CHANNEL_RSP:
        return "RSP";
      default:
        return std::string{};
    }
}

} // namespace

std::string
transactionToString(const Payload &payload,
                    const Phase &phase)
{
    return csprintf("%s %s addr=0x%08lx ns=%d size=%d attrs=0x%x",
        phaseToChannelName(phase),
        phaseToOpcodeName(phase).c_str(),
        payload.address, payload.ns,
        (int)payload.size, (int)payload.mem_attr);
}

namespace tlm_to_ruby {

ruby::CHIRequestType
reqOpcode(ReqOpcode req)
{
    static std::unordered_map<uint8_t, ruby::CHIRequestType> translation_map = {
        { REQ_OPCODE_READ_SHARED, ruby::CHIRequestType_ReadShared },
        { REQ_OPCODE_READ_CLEAN, ruby::CHIRequestType_ReadOnce }, // TODO
        { REQ_OPCODE_READ_ONCE, ruby::CHIRequestType_ReadOnce },
        { REQ_OPCODE_READ_NO_SNP, ruby::CHIRequestType_ReadNoSnp }, // TODO
        { REQ_OPCODE_READ_UNIQUE, ruby::CHIRequestType_ReadUnique },
        { REQ_OPCODE_READ_NOT_SHARED_DIRTY, ruby::CHIRequestType_ReadNotSharedDirty },
        { REQ_OPCODE_READ_PREFER_UNIQUE, ruby::CHIRequestType_ReadUnique }, // TODO
        { REQ_OPCODE_MAKE_READ_UNIQUE, ruby::CHIRequestType_MakeReadUnique }, // TODO

        { REQ_OPCODE_CLEAN_UNIQUE, ruby::CHIRequestType_CleanUnique },
        { REQ_OPCODE_MAKE_UNIQUE, ruby::CHIRequestType_CleanUnique }, // TODO
        { REQ_OPCODE_EVICT, ruby::CHIRequestType_Evict },
        { REQ_OPCODE_STASH_ONCE_SEP_SHARED, ruby::CHIRequestType_StashOnceShared }, // TODO
        { REQ_OPCODE_STASH_ONCE_SEP_UNIQUE, ruby::CHIRequestType_StashOnceUnique },
        { REQ_OPCODE_WRITE_NO_SNP_PTL, ruby::CHIRequestType_WriteUniquePtl },
        { REQ_OPCODE_WRITE_NO_SNP_FULL, ruby::CHIRequestType_WriteUniqueFull },
        { REQ_OPCODE_WRITE_UNIQUE_FULL, ruby::CHIRequestType_WriteUniqueFull },
        { REQ_OPCODE_WRITE_UNIQUE_ZERO, ruby::CHIRequestType_WriteUniqueZero },
        { REQ_OPCODE_WRITE_BACK_FULL, ruby::CHIRequestType_WriteBackFull },
        { REQ_OPCODE_WRITE_EVICT_OR_EVICT, ruby::CHIRequestType_WriteEvictFull }, // TODO
    };

    auto it = translation_map.find(req);
    if (it != translation_map.end()) {
        return it->second;
    } else {
        panic("Unsupported Translation: %s\n", reqOpcodeToName(req));
    }
}

#define RESP_CASE(opc) \
  switch (resp) { \
    case RESP_I: return opc ## _ ## I; \
    case RESP_SC: return opc ## _ ## SC; \
    case RESP_UC: return opc ## _ ## UC; \
    case RESP_SD: return opc ## _ ## SD; \
    case RESP_I_PD: return opc ## _ ## I_PD; \
    case RESP_SC_PD: return opc ## _ ## SC_PD; \
    case RESP_UC_PD: return opc ## _ ## UC; \
    case RESP_SD_PD: return opc ## _ ## SD; \
    default: panic(""); \
    }

ruby::CHIDataType
datOpcode(DatOpcode dat, Resp resp)
{
    switch (dat) {
      case DAT_OPCODE_NON_COPY_BACK_WR_DATA:
        return ruby::CHIDataType_NCBWrData;
      case DAT_OPCODE_COPY_BACK_WR_DATA:
        switch (resp) {
          case RESP_I: return ruby::CHIDataType_CBWrData_I;
          case RESP_UC: return ruby::CHIDataType_CBWrData_UC;
          case RESP_SC: return ruby::CHIDataType_CBWrData_SC;
          case RESP_UD_PD: return ruby::CHIDataType_CBWrData_UD_PD;
          default: panic("");
        }
      case DAT_OPCODE_SNP_RESP_DATA:
        RESP_CASE(ruby::CHIDataType_SnpRespData)
      default:
        panic("Unsupported Translation: %s\n", datOpcodeToName(dat));
    }
}

ruby::CHIResponseType
rspOpcode(RspOpcode opc, Resp resp)
{
    switch(opc) {
      case RSP_OPCODE_COMP_ACK: return ruby::CHIResponseType_CompAck;
      case RSP_OPCODE_SNP_RESP:
        switch (resp) {
          case RESP_I: return ruby::CHIResponseType_SnpResp_I;
          default: panic("Invalid resp %d for %d\n", resp, opc);
        }
      default:
        panic("Unsupported Translation: %s\n", rspOpcodeToName(opc));
    };
}

}

namespace ruby_to_tlm {

uint8_t
datOpcode(ruby::CHIDataType dat)
{
    switch (dat) {
      case ruby::CHIDataType_CompData_I:
      case ruby::CHIDataType_CompData_UC:
      case ruby::CHIDataType_CompData_SC:
      case ruby::CHIDataType_CompData_UD_PD:
      case ruby::CHIDataType_CompData_SD_PD:
        return 0x4;
      case ruby::CHIDataType_DataSepResp_UC:
        return 0xb;
      case ruby::CHIDataType_CBWrData_UC:
      case ruby::CHIDataType_CBWrData_SC:
      case ruby::CHIDataType_CBWrData_UD_PD:
      case ruby::CHIDataType_CBWrData_SD_PD:
      case ruby::CHIDataType_CBWrData_I:
        return 0x2;
      case ruby::CHIDataType_NCBWrData:
        return 0x3;
      case ruby::CHIDataType_SnpRespData_I:
      case ruby::CHIDataType_SnpRespData_I_PD:
      case ruby::CHIDataType_SnpRespData_SC:
      case ruby::CHIDataType_SnpRespData_SC_PD:
      case ruby::CHIDataType_SnpRespData_SD:
      case ruby::CHIDataType_SnpRespData_UC:
      case ruby::CHIDataType_SnpRespData_UD:
        return 0x1;
      case ruby::CHIDataType_SnpRespData_SC_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_SC_Fwded_SD_PD:
      case ruby::CHIDataType_SnpRespData_SC_PD_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_SD_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_I_Fwded_SD_PD:
      case ruby::CHIDataType_SnpRespData_I_PD_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_I_Fwded_SC:
        return 0x6;
      default:
        panic("Unrecognised data opcode: %d\n", dat);
    }
}

uint8_t
rspOpcode(ruby::CHIResponseType rsp)
{
    switch (rsp) {
      case ruby::CHIResponseType_Comp_UD_PD:
      case ruby::CHIResponseType_Comp_UC:
      case ruby::CHIResponseType_Comp_I:
        return RSP_OPCODE_COMP;
      case ruby::CHIResponseType_CompDBIDResp:
        return RSP_OPCODE_COMP_DBID_RESP;
      case ruby::CHIResponseType_RetryAck:
        return RSP_OPCODE_RETRY_ACK;
      default:
        panic("Unrecognised rsp opcode: %d\n", rsp);
    }
}

uint8_t
snpOpcode(ruby::CHIRequestType snp)
{
    switch (snp) {
      case ruby::CHIRequestType_SnpOnceFwd:
        return SNP_OPCODE_SNP_ONCE_FWD;
      case ruby::CHIRequestType_SnpOnce:
        return SNP_OPCODE_SNP_ONCE;
      case ruby::CHIRequestType_SnpShared:
        return SNP_OPCODE_SNP_SHARED;
      case ruby::CHIRequestType_SnpCleanInvalid:
        return SNP_OPCODE_SNP_CLEAN_INVALID;
      case ruby::CHIRequestType_SnpUnique:
        return SNP_OPCODE_SNP_UNIQUE;
      default:
        panic("Unrecognised snp opcode: %d\n", snp);
    }
}

Resp
datResp(ruby::CHIDataType dat)
{
    switch (dat) {
      case ruby::CHIDataType_SnpRespData_I:
      case ruby::CHIDataType_CompData_I:
      case ruby::CHIDataType_CBWrData_I:
        return RESP_I;
      case ruby::CHIDataType_SnpRespData_SC:
      case ruby::CHIDataType_CompData_SC:
      case ruby::CHIDataType_CBWrData_SC:
        return RESP_SC;
      case ruby::CHIDataType_SnpRespData_UC:
      case ruby::CHIDataType_CompData_UC:
      case ruby::CHIDataType_CBWrData_UC:
      case ruby::CHIDataType_DataSepResp_UC:
        return RESP_UC;
      case ruby::CHIDataType_SnpRespData_UD:
        return RESP_UD;
      case ruby::CHIDataType_SnpRespData_SD:
        return RESP_SD;
      case ruby::CHIDataType_SnpRespData_I_PD:
        return RESP_I_PD;
      case ruby::CHIDataType_SnpRespData_SC_PD:
        return RESP_SC_PD;
      case ruby::CHIDataType_CompData_UD_PD:
      case ruby::CHIDataType_CBWrData_UD_PD:
        return RESP_UD_PD;
      case ruby::CHIDataType_CompData_SD_PD:
      case ruby::CHIDataType_CBWrData_SD_PD:
        return RESP_SD_PD;
      // TODO
      case ruby::CHIDataType_SnpRespData_SC_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_SC_Fwded_SD_PD:
      case ruby::CHIDataType_SnpRespData_SC_PD_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_SD_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_I_Fwded_SD_PD:
      case ruby::CHIDataType_SnpRespData_I_PD_Fwded_SC:
      case ruby::CHIDataType_SnpRespData_I_Fwded_SC:
      default:
        panic("Unrecognised data opcode: %d\n", dat);
    }
}

Resp
rspResp(ruby::CHIResponseType rsp)
{
    switch (rsp) {
      case ruby::CHIResponseType_Comp_I:
        return RESP_I;
      case ruby::CHIResponseType_Comp_UC:
        return RESP_UC;
      case ruby::CHIResponseType_Comp_UD_PD:
        return RESP_UD_PD;
      case ruby::CHIResponseType_CompDBIDResp:
        return RESP_I;
      case ruby::CHIResponseType_RetryAck:
        // Just setup to zero
        return RESP_I;
      default:
        panic("Unrecognised rsp opcode: %d\n", rsp);
    }
}

}

Addr
transactionSize(Size sz)
{
    return 1 << sz;
}

uint8_t
dataId(Addr address)
{
    uint64_t bus_width = 256;
    switch (bus_width) {
      case 128:
       return bits(address, 5, 4);
      case 256:
        return bits(address, 5, 4) & 0b10;
      case 512:
      default:
        return 0b00;
    }
}

} // namespace tlm::chi

} // namespace gem5
