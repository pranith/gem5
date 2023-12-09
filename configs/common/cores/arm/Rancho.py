# Copyright (c) 2023 Pranith Kumar
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

"""The High-Performance In-order (Rancho) CPU timing model is tuned to be
representative of a modern in-order ARMv8-A implementation. The Rancho
core and its supporting simulation scripts, namely starter_se.py and
starter_fs.py (under /configs/example/arm/) are part of the ARM
Research Starter Kit on System Modeling. More information can be found
at: http://www.arm.com/ResearchEnablement/SystemModeling

"""

from m5.objects import *
from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *


# Simple function to allow a string of [01x_] to be converted into a
# mask and value for use with MinorFUTiming
def make_implicant(implicant_string):
    ret_mask = 0
    ret_match = 0

    shift = False
    for char in implicant_string:
        char = char.lower()
        if shift:
            ret_mask <<= 1
            ret_match <<= 1

        shift = True
        if char == "_":
            shift = False
        elif char == "0":
            ret_mask |= 1
        elif char == "1":
            ret_mask |= 1
            ret_match |= 1
        elif char == "x":
            pass
        else:
            print("Can't parse implicant character", char)

    return (ret_mask, ret_match)


#                          ,----- 36 thumb
#                          | ,--- 35 bigThumb
#                          | |,-- 34 aarch64
a64_inst = make_implicant("0_01xx__xxxx_xxxx_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
a32_inst = make_implicant("0_00xx__xxxx_xxxx_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
t32_inst = make_implicant("1_10xx__xxxx_xxxx_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
t16_inst = make_implicant("1_00xx__xxxx_xxxx_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
any_inst = make_implicant("x_xxxx__xxxx_xxxx_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
#                          | ||
any_a64_inst = make_implicant(
    "x_x1xx__xxxx_xxxx_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx"
)
any_non_a64_inst = make_implicant(
    "x_x0xx__xxxx_xxxx_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx"
)


def encode_opcode(pattern):
    def encode(opcode_string):
        a64_mask, a64_match = pattern
        mask, match = make_implicant(opcode_string)
        return (a64_mask | mask), (a64_match | match)

    return encode


a64_opcode = encode_opcode(a64_inst)
a32_opcode = encode_opcode(a32_inst)
t32_opcode = encode_opcode(t32_inst)
t16_opcode = encode_opcode(t16_inst)

# These definitions (in some form) should probably be part of TimingExpr


def literal(value):
    def body(env):
        ret = TimingExprLiteral()
        ret.value = value
        return ret

    return body


def bin(op, left, right):
    def body(env):
        ret = TimingExprBin()
        ret.op = "timingExpr" + op
        ret.left = left(env)
        ret.right = right(env)
        return ret

    return body


def un(op, arg):
    def body(env):
        ret = TimingExprUn()
        ret.op = "timingExpr" + op
        ret.arg = arg(env)
        return ret

    return body


def ref(name):
    def body(env):
        if name in env:
            ret = TimingExprRef()
            ret.index = env[name]
        else:
            print("Invalid expression name", name)
            ret = TimingExprNull()
        return ret

    return body


def if_expr(cond, true_expr, false_expr):
    def body(env):
        ret = TimingExprIf()
        ret.cond = cond(env)
        ret.trueExpr = true_expr(env)
        ret.falseExpr = false_expr(env)
        return ret

    return body


def src_reg(index):
    def body(env):
        ret = TimingExprSrcReg()
        ret.index = index
        return ret

    return body


def let(bindings, expr):
    def body(env):
        ret = TimingExprLet()
        let_bindings = []
        new_env = {}
        i = 0

        # Make the sub-expression as null to start with
        for name, binding in bindings:
            new_env[name] = i
            i += 1

        defns = []
        # Then apply them to the produced new env
        for i in range(0, len(bindings)):
            name, binding_expr = bindings[i]
            defns.append(binding_expr(new_env))

        ret.defns = defns
        ret.expr = expr(new_env)

        return ret

    return body


def expr_top(expr):
    return expr([])


class Rancho_DefaultInt(MinorFUTiming):
    description = "Rancho_DefaultInt"
    mask, match = any_non_a64_inst
    srcRegsRelativeLats = [3, 3, 2, 2, 2, 1, 0]


class Rancho_DefaultA64Int(MinorFUTiming):
    description = "Rancho_DefaultA64Int"
    mask, match = any_a64_inst
    # r, l, (c)
    srcRegsRelativeLats = [2, 2, 2, 0]


class Rancho_DefaultMul(MinorFUTiming):
    description = "Rancho_DefaultMul"
    mask, match = any_non_a64_inst
    # f, f, f, r, l, a?
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 0]


class Rancho_DefaultA64Mul(MinorFUTiming):
    description = "Rancho_DefaultA64Mul"
    mask, match = any_a64_inst
    # a (zr for mul), l, r
    srcRegsRelativeLats = [0, 0, 0, 0]
    # extraCommitLat = 1


class Rancho_DefaultVfp(MinorFUTiming):
    description = "Rancho_DefaultVfp"
    mask, match = any_non_a64_inst
    # cpsr, z, z, z, cpacr, fpexc, l_lo, r_lo, l_hi, r_hi (from vadd2h)
    srcRegsRelativeLats = [5, 5, 5, 5, 5, 5, 2, 2, 2, 2, 2, 2, 2, 2, 0]


class Rancho_DefaultA64Vfp(MinorFUTiming):
    description = "Rancho_DefaultA64Vfp"
    mask, match = any_a64_inst
    # cpsr, cpacr_el1, fpscr_exc, ...
    srcRegsRelativeLats = [5, 5, 5, 2]


class Rancho_FMADD_A64(MinorFUTiming):
    description = "Rancho_FMADD_A64"
    mask, match = a64_opcode("0001_1111_0x0x_xxxx__0xxx_xxxx_xxxx_xxxx")
    #                                    t
    # cpsr, cpacr_el1, fpscr_exc, 1, 1, 2, 2, 3, 3, fpscr_exc, d, d, d, d
    srcRegsRelativeLats = [5, 5, 5, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0]


class Rancho_FMSUB_D_A64(MinorFUTiming):
    description = "Rancho_FMSUB_D_A64"
    mask, match = a64_opcode("0001_1111_0x0x_xxxx__1xxx_xxxx_xxxx_xxxx")
    #                                    t
    # cpsr, cpacr_el1, fpscr_exc, 1, 1, 2, 2, 3, 3, fpscr_exc, d, d, d, d
    srcRegsRelativeLats = [5, 5, 5, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0]


class Rancho_FMOV_A64(MinorFUTiming):
    description = "Rancho_FMOV_A64"
    mask, match = a64_opcode("0001_1110_0x10_0000__0100_00xx_xxxx_xxxx")
    # cpsr, cpacr_el1, fpscr_exc, 1, 1, 2, 2, 3, 3, fpscr_exc, d, d, d, d
    srcRegsRelativeLats = [5, 5, 5, 0]


class Rancho_ADD_SUB_vector_scalar_A64(MinorFUTiming):
    description = "Rancho_ADD_SUB_vector_scalar_A64"
    mask, match = a64_opcode("01x1_1110_xx1x_xxxx__1000_01xx_xxxx_xxxx")
    # cpsr, z, z, z, cpacr, fpexc, l0, r0, l1, r1, l2, r2, l3, r3 (for vadd2h)
    srcRegsRelativeLats = [5, 5, 5, 4]


class Rancho_ADD_SUB_vector_vector_A64(MinorFUTiming):
    description = "Rancho_ADD_SUB_vector_vector_A64"
    mask, match = a64_opcode("0xx0_1110_xx1x_xxxx__1000_01xx_xxxx_xxxx")
    # cpsr, z, z, z, cpacr, fpexc, l0, r0, l1, r1, l2, r2, l3, r3 (for vadd2h)
    srcRegsRelativeLats = [5, 5, 5, 4]


class Rancho_FDIV_scalar_32_A64(MinorFUTiming):
    description = "Rancho_FDIV_scalar_32_A64"
    mask, match = a64_opcode("0001_1110_001x_xxxx__0001_10xx_xxxx_xxxx")
    extraCommitLat = 6
    srcRegsRelativeLats = [0, 0, 0, 20, 4]


class Rancho_FDIV_scalar_64_A64(MinorFUTiming):
    description = "Rancho_FDIV_scalar_64_A64"
    mask, match = a64_opcode("0001_1110_011x_xxxx__0001_10xx_xxxx_xxxx")
    extraCommitLat = 15
    srcRegsRelativeLats = [0, 0, 0, 20, 4]


# CINC CINV CSEL CSET CSETM CSINC CSINC CSINV CSINV CSNEG
class Rancho_Cxxx_A64(MinorFUTiming):
    description = "Rancho_Cxxx_A64"
    mask, match = a64_opcode("xx01_1010_100x_xxxx_xxxx__0xxx_xxxx_xxxx")
    srcRegsRelativeLats = [3, 3, 3, 2, 2]


class Rancho_DefaultMem(MinorFUTiming):
    description = "Rancho_DefaultMem"
    mask, match = any_non_a64_inst
    srcRegsRelativeLats = [1, 1, 1, 1, 1, 2]
    # Assume that LDR/STR take 2 cycles for resolving dependencies
    # (1 + 1 of the FU)
    extraAssumedLat = 2


class Rancho_DefaultMem64(MinorFUTiming):
    description = "Rancho_DefaultMem64"
    mask, match = any_a64_inst
    srcRegsRelativeLats = [2]
    # Assume that LDR/STR take 2 cycles for resolving dependencies
    # (1 + 1 of the FU)
    extraAssumedLat = 3


class Rancho_DataProcessingMovShiftr(MinorFUTiming):
    description = "Rancho_DataProcessingMovShiftr"
    mask, match = a32_opcode("xxxx_0001_101x_xxxx__xxxx_xxxx_xxx1_xxxx")
    srcRegsRelativeLats = [3, 3, 2, 2, 2, 1, 0]


class Rancho_DataProcessingMayShift(MinorFUTiming):
    description = "Rancho_DataProcessingMayShift"
    mask, match = a32_opcode("xxxx_000x_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
    srcRegsRelativeLats = [3, 3, 2, 2, 1, 1, 0]


class Rancho_DataProcessingNoShift(MinorFUTiming):
    description = "Rancho_DataProcessingNoShift"
    mask, match = a32_opcode("xxxx_000x_xxxx_xxxx__xxxx_0000_0xx0_xxxx")
    srcRegsRelativeLats = [3, 3, 2, 2, 2, 1, 0]


class Rancho_DataProcessingAllowShifti(MinorFUTiming):
    description = "Rancho_DataProcessingAllowShifti"
    mask, match = a32_opcode("xxxx_000x_xxxx_xxxx__xxxx_xxxx_xxx0_xxxx")
    srcRegsRelativeLats = [3, 3, 2, 2, 1, 1, 0]


class Rancho_DataProcessingSuppressShift(MinorFUTiming):
    description = "Rancho_DataProcessingSuppressShift"
    mask, match = a32_opcode("xxxx_000x_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
    srcRegsRelativeLats = []
    suppress = True


class Rancho_DataProcessingSuppressBranch(MinorFUTiming):
    description = "Rancho_DataProcessingSuppressBranch"
    mask, match = a32_opcode("xxxx_1010_xxxx_xxxx__xxxx_xxxx_xxxx_xxxx")
    srcRegsRelativeLats = []
    suppress = True


class Rancho_BFI_T1(MinorFUTiming):
    description = "Rancho_BFI_T1"
    mask, match = t32_opcode("1111_0x11_0110_xxxx__0xxx_xxxx_xxxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 1, 0]


class Rancho_BFI_A1(MinorFUTiming):
    description = "Rancho_BFI_A1"
    mask, match = a32_opcode("xxxx_0111_110x_xxxx__xxxx_xxxx_x001_xxxx")
    # f, f, f, dest, src
    srcRegsRelativeLats = [0, 0, 0, 1, 1, 0]


class Rancho_CLZ_T1(MinorFUTiming):
    description = "Rancho_CLZ_T1"
    mask, match = t32_opcode("1111_1010_1011_xxxx__1111_xxxx_1000_xxxx")
    srcRegsRelativeLats = [3, 3, 2, 2, 2, 1, 0]


class Rancho_CLZ_A1(MinorFUTiming):
    description = "Rancho_CLZ_A1"
    mask, match = a32_opcode("xxxx_0001_0110_xxxx__xxxx_xxxx_0001_xxxx")
    srcRegsRelativeLats = [3, 3, 2, 2, 2, 1, 0]


class Rancho_CMN_immediate_A1(MinorFUTiming):
    description = "Rancho_CMN_immediate_A1"
    mask, match = a32_opcode("xxxx_0011_0111_xxxx__xxxx_xxxx_xxxx_xxxx")
    srcRegsRelativeLats = [3, 3, 3, 2, 2, 3, 3, 3, 0]


class Rancho_CMN_register_A1(MinorFUTiming):
    description = "Rancho_CMN_register_A1"
    mask, match = a32_opcode("xxxx_0001_0111_xxxx__xxxx_xxxx_xxx0_xxxx")
    srcRegsRelativeLats = [3, 3, 3, 2, 2, 3, 3, 3, 0]


class Rancho_CMP_immediate_A1(MinorFUTiming):
    description = "Rancho_CMP_immediate_A1"
    mask, match = a32_opcode("xxxx_0011_0101_xxxx__xxxx_xxxx_xxxx_xxxx")
    srcRegsRelativeLats = [3, 3, 3, 2, 2, 3, 3, 3, 0]


class Rancho_CMP_register_A1(MinorFUTiming):
    description = "Rancho_CMP_register_A1"
    mask, match = a32_opcode("xxxx_0001_0101_xxxx__xxxx_xxxx_xxx0_xxxx")
    srcRegsRelativeLats = [3, 3, 3, 2, 2, 3, 3, 3, 0]


class Rancho_MLA_T1(MinorFUTiming):
    description = "Rancho_MLA_T1"
    mask, match = t32_opcode("1111_1011_0000_xxxx__xxxx_xxxx_0000_xxxx")
    # z, z, z, a, l?, r?
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_MLA_A1(MinorFUTiming):
    description = "Rancho_MLA_A1"
    mask, match = a32_opcode("xxxx_0000_001x_xxxx__xxxx_xxxx_1001_xxxx")
    # z, z, z, a, l?, r?
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_MADD_A64(MinorFUTiming):
    description = "Rancho_MADD_A64"
    mask, match = a64_opcode("x001_1011_000x_xxxx__0xxx_xxxx_xxxx_xxxx")
    # a, l?, r?
    srcRegsRelativeLats = [1, 1, 1, 0]
    extraCommitLat = 1


class Rancho_MLS_T1(MinorFUTiming):
    description = "Rancho_MLS_T1"
    mask, match = t32_opcode("1111_1011_0000_xxxx__xxxx_xxxx_0001_xxxx")
    # z, z, z, l?, a, r?
    srcRegsRelativeLats = [0, 0, 0, 2, 0, 0, 0]


class Rancho_MLS_A1(MinorFUTiming):
    description = "Rancho_MLS_A1"
    mask, match = a32_opcode("xxxx_0000_0110_xxxx__xxxx_xxxx_1001_xxxx")
    # z, z, z, l?, a, r?
    srcRegsRelativeLats = [0, 0, 0, 2, 0, 0, 0]


class Rancho_MOVT_A1(MinorFUTiming):
    description = "Rancho_MOVT_A1"
    mask, match = t32_opcode("xxxx_0010_0100_xxxx__xxxx_xxxx_xxxx_xxxx")


class Rancho_MUL_T1(MinorFUTiming):
    description = "Rancho_MUL_T1"
    mask, match = t16_opcode("0100_0011_01xx_xxxx")


class Rancho_MUL_T2(MinorFUTiming):
    description = "Rancho_MUL_T2"
    mask, match = t32_opcode("1111_1011_0000_xxxx_1111_xxxx_0000_xxxx")


class Rancho_PKH_T1(MinorFUTiming):
    description = "Rancho_PKH_T1"
    mask, match = t32_opcode("1110_1010_110x_xxxx__xxxx_xxxx_xxxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 1, 0]


class Rancho_PKH_A1(MinorFUTiming):
    description = "Rancho_PKH_A1"
    mask, match = a32_opcode("xxxx_0110_1000_xxxx__xxxx_xxxx_xx01_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 1, 0]


class Rancho_QADD_QSUB_T1(MinorFUTiming):
    description = "Rancho_QADD_QSUB_T1"
    mask, match = t32_opcode("1111_1010_1000_xxxx__1111_xxxx_10x0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 1, 0]


class Rancho_QADD_QSUB_A1(MinorFUTiming):
    description = "Rancho_QADD_QSUB_A1"
    mask, match = a32_opcode("xxxx_0001_00x0_xxxx__xxxx_xxxx_0101_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 1, 0]


# T1 QADD16 QADD8 QSUB16 QSUB8 UQADD16 UQADD8 UQSUB16 UQSUB8
class Rancho_QADD_ETC_T1(MinorFUTiming):
    description = "Rancho_QADD_ETC_T1"
    mask, match = t32_opcode("1111_1010_1x0x_xxxx__1111_xxxx_0x01_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 1, 0]


# A1 QADD16 QADD8 QSAX QSUB16 QSUB8 UQADD16 UQADD8 UQASX UQSAX UQSUB16 UQSUB8
class Rancho_QADD_ETC_A1(MinorFUTiming):
    description = "Rancho_QADD_ETC_A1"
    mask, match = a32_opcode("xxxx_0110_0x10_xxxx__xxxx_xxxx_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 1, 0]


class Rancho_QASX_QSAX_UQASX_UQSAX_T1(MinorFUTiming):
    description = "Rancho_QASX_QSAX_UQASX_UQSAX_T1"
    mask, match = t32_opcode("1111_1010_1x10_xxxx__1111_xxxx_0x01_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 1, 0]


class Rancho_QDADD_QDSUB_T1(MinorFUTiming):
    description = "Rancho_QDADD_QDSUB_T1"
    mask, match = t32_opcode("1111_1010_1000_xxxx__1111_xxxx_10x1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 1, 0]


class Rancho_QDADD_QDSUB_A1(MinorFUTiming):
    description = "Rancho_QDADD_QSUB_A1"
    mask, match = a32_opcode("xxxx_0001_01x0_xxxx__xxxx_xxxx_0101_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 1, 0]


class Rancho_RBIT_A1(MinorFUTiming):
    description = "Rancho_RBIT_A1"
    mask, match = a32_opcode("xxxx_0110_1111_xxxx__xxxx_xxxx_0011_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 0]


class Rancho_REV_REV16_A1(MinorFUTiming):
    description = "Rancho_REV_REV16_A1"
    mask, match = a32_opcode("xxxx_0110_1011_xxxx__xxxx_xxxx_x011_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 0]


class Rancho_REVSH_A1(MinorFUTiming):
    description = "Rancho_REVSH_A1"
    mask, match = a32_opcode("xxxx_0110_1111_xxxx__xxxx_xxxx_1011_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 0]


class Rancho_ADD_ETC_A1(MinorFUTiming):
    description = "Rancho_ADD_ETC_A1"
    mask, match = a32_opcode("xxxx_0110_0xx1_xxxx__xxxx_xxxx_x001_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 2, 0]


class Rancho_ADD_ETC_T1(MinorFUTiming):
    description = "Rancho_ADD_ETC_A1"
    mask, match = t32_opcode("1111_1010_100x_xxxx__1111_xxxx_0xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 2, 0]


class Rancho_SASX_SHASX_UASX_UHASX_A1(MinorFUTiming):
    description = "Rancho_SASX_SHASX_UASX_UHASX_A1"
    mask, match = a32_opcode("xxxx_0110_0xx1_xxxx__xxxx_xxxx_0011_xxxx")
    srcRegsRelativeLats = [3, 3, 2, 2, 2, 1, 0]


class Rancho_SBFX_UBFX_A1(MinorFUTiming):
    description = "Rancho_SBFX_UBFX_A1"
    mask, match = a32_opcode("xxxx_0111_1x1x_xxxx__xxxx_xxxx_x101_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 0]


### SDIV

sdiv_lat_expr = expr_top(
    let(
        [
            ("left", un("SignExtend32To64", src_reg(4))),
            ("right", un("SignExtend32To64", src_reg(3))),
            (
                "either_signed",
                bin(
                    "Or",
                    bin("SLessThan", ref("left"), literal(0)),
                    bin("SLessThan", ref("right"), literal(0)),
                ),
            ),
            ("left_size", un("SizeInBits", un("Abs", ref("left")))),
            (
                "signed_adjust",
                if_expr(ref("either_signed"), literal(1), literal(0)),
            ),
            (
                "right_size",
                un(
                    "SizeInBits",
                    bin(
                        "UDiv",
                        un("Abs", ref("right")),
                        if_expr(ref("either_signed"), literal(4), literal(2)),
                    ),
                ),
            ),
            (
                "left_minus_right",
                if_expr(
                    bin("SLessThan", ref("left_size"), ref("right_size")),
                    literal(0),
                    bin("Sub", ref("left_size"), ref("right_size")),
                ),
            ),
        ],
        bin(
            "Add",
            ref("signed_adjust"),
            if_expr(
                bin("Equal", ref("right"), literal(0)),
                literal(0),
                bin("UDiv", ref("left_minus_right"), literal(4)),
            ),
        ),
    )
)

sdiv_lat_expr64 = expr_top(
    let(
        [
            ("left", un("SignExtend32To64", src_reg(0))),
            ("right", un("SignExtend32To64", src_reg(1))),
            (
                "either_signed",
                bin(
                    "Or",
                    bin("SLessThan", ref("left"), literal(0)),
                    bin("SLessThan", ref("right"), literal(0)),
                ),
            ),
            ("left_size", un("SizeInBits", un("Abs", ref("left")))),
            (
                "signed_adjust",
                if_expr(ref("either_signed"), literal(1), literal(0)),
            ),
            (
                "right_size",
                un(
                    "SizeInBits",
                    bin(
                        "UDiv",
                        un("Abs", ref("right")),
                        if_expr(ref("either_signed"), literal(4), literal(2)),
                    ),
                ),
            ),
            (
                "left_minus_right",
                if_expr(
                    bin("SLessThan", ref("left_size"), ref("right_size")),
                    literal(0),
                    bin("Sub", ref("left_size"), ref("right_size")),
                ),
            ),
        ],
        bin(
            "Add",
            ref("signed_adjust"),
            if_expr(
                bin("Equal", ref("right"), literal(0)),
                literal(0),
                bin("UDiv", ref("left_minus_right"), literal(4)),
            ),
        ),
    )
)


class Rancho_SDIV_A1(MinorFUTiming):
    description = "Rancho_SDIV_A1"
    mask, match = a32_opcode("xxxx_0111_0001_xxxx__xxxx_xxxx_0001_xxxx")
    extraCommitLat = 0
    srcRegsRelativeLats = []
    extraCommitLatExpr = sdiv_lat_expr


class Rancho_SDIV_A64(MinorFUTiming):
    description = "Rancho_SDIV_A64"
    mask, match = a64_opcode("x001_1010_110x_xxxx__0000_11xx_xxxx_xxxx")
    extraCommitLat = 0
    srcRegsRelativeLats = []
    extraCommitLatExpr = sdiv_lat_expr64


### SEL


class Rancho_SEL_A1(MinorFUTiming):
    description = "Rancho_SEL_A1"
    mask, match = a32_opcode("xxxx_0110_1000_xxxx__xxxx_xxxx_1011_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 2, 2, 0]


class Rancho_SEL_A1_Suppress(MinorFUTiming):
    description = "Rancho_SEL_A1_Suppress"
    mask, match = a32_opcode("xxxx_0110_1000_xxxx__xxxx_xxxx_1011_xxxx")
    srcRegsRelativeLats = []
    suppress = True


class Rancho_SHSAX_SSAX_UHSAX_USAX_A1(MinorFUTiming):
    description = "Rancho_SHSAX_SSAX_UHSAX_USAX_A1"
    mask, match = a32_opcode("xxxx_0110_0xx1_xxxx__xxxx_xxxx_0101_xxxx")
    # As Default
    srcRegsRelativeLats = [3, 3, 2, 2, 2, 1, 0]


class Rancho_USUB_ETC_A1(MinorFUTiming):
    description = "Rancho_USUB_ETC_A1"
    mask, match = a32_opcode("xxxx_0110_0xx1_xxxx__xxxx_xxxx_x111_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 2, 0]


class Rancho_SMLABB_T1(MinorFUTiming):
    description = "Rancho_SMLABB_T1"
    mask, match = t32_opcode("1111_1011_0001_xxxx__xxxx_xxxx_00xx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_SMLABB_A1(MinorFUTiming):
    description = "Rancho_SMLABB_A1"
    mask, match = a32_opcode("xxxx_0001_0000_xxxx__xxxx_xxxx_1xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_SMLAD_T1(MinorFUTiming):
    description = "Rancho_SMLAD_T1"
    mask, match = t32_opcode("1111_1011_0010_xxxx__xxxx_xxxx_000x_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_SMLAD_A1(MinorFUTiming):
    description = "Rancho_SMLAD_A1"
    mask, match = a32_opcode("xxxx_0111_0000_xxxx__xxxx_xxxx_00x1_xxxx")
    # z, z, z, l, r, a
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_SMLAL_T1(MinorFUTiming):
    description = "Rancho_SMLAL_T1"
    mask, match = t32_opcode("1111_1011_1100_xxxx__xxxx_xxxx_0000_xxxx")


class Rancho_SMLAL_A1(MinorFUTiming):
    description = "Rancho_SMLAL_A1"
    mask, match = a32_opcode("xxxx_0000_111x_xxxx__xxxx_xxxx_1001_xxxx")


class Rancho_SMLALBB_T1(MinorFUTiming):
    description = "Rancho_SMLALBB_T1"
    mask, match = t32_opcode("1111_1011_1100_xxxx__xxxx_xxxx_10xx_xxxx")


class Rancho_SMLALBB_A1(MinorFUTiming):
    description = "Rancho_SMLALBB_A1"
    mask, match = a32_opcode("xxxx_0001_0100_xxxx__xxxx_xxxx_1xx0_xxxx")


class Rancho_SMLALD_T1(MinorFUTiming):
    description = "Rancho_SMLALD_T1"
    mask, match = t32_opcode("1111_1011_1100_xxxx__xxxx_xxxx_110x_xxxx")


class Rancho_SMLALD_A1(MinorFUTiming):
    description = "Rancho_SMLALD_A1"
    mask, match = a32_opcode("xxxx_0111_0100_xxxx__xxxx_xxxx_00x1_xxxx")


class Rancho_SMLAWB_T1(MinorFUTiming):
    description = "Rancho_SMLAWB_T1"
    mask, match = t32_opcode("1111_1011_0011_xxxx__xxxx_xxxx_000x_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_SMLAWB_A1(MinorFUTiming):
    description = "Rancho_SMLAWB_A1"
    mask, match = a32_opcode("xxxx_0001_0010_xxxx__xxxx_xxxx_1x00_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_SMLSD_A1(MinorFUTiming):
    description = "Rancho_SMLSD_A1"
    mask, match = a32_opcode("xxxx_0111_0000_xxxx__xxxx_xxxx_01x1_xxxx")


class Rancho_SMLSLD_T1(MinorFUTiming):
    description = "Rancho_SMLSLD_T1"
    mask, match = t32_opcode("1111_1011_1101_xxxx__xxxx_xxxx_110x_xxxx")


class Rancho_SMLSLD_A1(MinorFUTiming):
    description = "Rancho_SMLSLD_A1"
    mask, match = a32_opcode("xxxx_0111_0100_xxxx__xxxx_xxxx_01x1_xxxx")


class Rancho_SMMLA_T1(MinorFUTiming):
    description = "Rancho_SMMLA_T1"
    mask, match = t32_opcode("1111_1011_0101_xxxx__xxxx_xxxx_000x_xxxx")
    #                                              ^^^^ != 1111
    srcRegsRelativeLats = [0, 0, 0, 2, 0, 0, 0]


class Rancho_SMMLA_A1(MinorFUTiming):
    description = "Rancho_SMMLA_A1"
    # Note that this must be after the encoding for SMMUL
    mask, match = a32_opcode("xxxx_0111_0101_xxxx__xxxx_xxxx_00x1_xxxx")
    #                                              ^^^^ != 1111
    srcRegsRelativeLats = [0, 0, 0, 2, 0, 0, 0]


class Rancho_SMMLS_T1(MinorFUTiming):
    description = "Rancho_SMMLS_T1"
    mask, match = t32_opcode("1111_1011_0110_xxxx__xxxx_xxxx_000x_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 0, 0, 0]


class Rancho_SMMLS_A1(MinorFUTiming):
    description = "Rancho_SMMLS_A1"
    mask, match = a32_opcode("xxxx_0111_0101_xxxx__xxxx_xxxx_11x1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 0, 0, 0]


class Rancho_SMMUL_T1(MinorFUTiming):
    description = "Rancho_SMMUL_T1"
    mask, match = t32_opcode("1111_1011_0101_xxxx__1111_xxxx_000x_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0]


class Rancho_SMMUL_A1(MinorFUTiming):
    description = "Rancho_SMMUL_A1"
    mask, match = a32_opcode("xxxx_0111_0101_xxxx__1111_xxxx_00x1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0]


class Rancho_SMUAD_T1(MinorFUTiming):
    description = "Rancho_SMUAD_T1"
    mask, match = t32_opcode("1111_1011_0010_xxxx__1111_xxxx_000x_xxxx")


class Rancho_SMUAD_A1(MinorFUTiming):
    description = "Rancho_SMUAD_A1"
    mask, match = a32_opcode("xxxx_0111_0000_xxxx__1111_xxxx_00x1_xxxx")


class Rancho_SMULBB_T1(MinorFUTiming):
    description = "Rancho_SMULBB_T1"
    mask, match = t32_opcode("1111_1011_0001_xxxx__1111_xxxx_00xx_xxxx")


class Rancho_SMULBB_A1(MinorFUTiming):
    description = "Rancho_SMULBB_A1"
    mask, match = a32_opcode("xxxx_0001_0110_xxxx__xxxx_xxxx_1xx0_xxxx")


class Rancho_SMULL_T1(MinorFUTiming):
    description = "Rancho_SMULL_T1"
    mask, match = t32_opcode("1111_1011_1000_xxxx__xxxx_xxxx_0000_xxxx")


class Rancho_SMULL_A1(MinorFUTiming):
    description = "Rancho_SMULL_A1"
    mask, match = a32_opcode("xxxx_0000_110x_xxxx__xxxx_xxxx_1001_xxxx")


class Rancho_SMULWB_T1(MinorFUTiming):
    description = "Rancho_SMULWB_T1"
    mask, match = t32_opcode("1111_1011_0011_xxxx__1111_xxxx_000x_xxxx")


class Rancho_SMULWB_A1(MinorFUTiming):
    description = "Rancho_SMULWB_A1"
    mask, match = a32_opcode("xxxx_0001_0010_xxxx__xxxx_xxxx_1x10_xxxx")


class Rancho_SMUSD_T1(MinorFUTiming):
    description = "Rancho_SMUSD_T1"
    mask, match = t32_opcode("1111_1011_0100_xxxx__1111_xxxx_000x_xxxx")


class Rancho_SMUSD_A1(MinorFUTiming):
    description = "Rancho_SMUSD_A1"
    mask, match = a32_opcode("xxxx_0111_0000_xxxx__1111_xxxx_01x1_xxxx")


class Rancho_SSAT_USAT_no_shift_A1(MinorFUTiming):
    description = "Rancho_SSAT_USAT_no_shift_A1"
    # Order *before* shift
    mask, match = a32_opcode("xxxx_0110_1x1x_xxxx__xxxx_0000_0001_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 0]


class Rancho_SSAT_USAT_shift_A1(MinorFUTiming):
    description = "Rancho_SSAT_USAT_shift_A1"
    # Order after shift
    mask, match = a32_opcode("xxxx_0110_1x1x_xxxx__xxxx_xxxx_xx01_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 0]


class Rancho_SSAT16_USAT16_A1(MinorFUTiming):
    description = "Rancho_SSAT16_USAT16_A1"
    mask, match = a32_opcode("xxxx_0110_1x10_xxxx__xxxx_xxxx_0011_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 0]


class Rancho_SXTAB_T1(MinorFUTiming):
    description = "Rancho_SXTAB_T1"
    mask, match = t32_opcode("1111_1010_0100_xxxx__1111_xxxx_1xxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 2, 0]


class Rancho_SXTAB_SXTAB16_SXTAH_UXTAB_UXTAB16_UXTAH_A1(MinorFUTiming):
    description = "Rancho_SXTAB_SXTAB16_SXTAH_UXTAB_UXTAB16_UXTAH_A1"
    # Place AFTER Rancho_SXTB_SXTB16_SXTH_UXTB_UXTB16_UXTH_A1
    # e6[9d][^f]0070 are undefined
    mask, match = a32_opcode("xxxx_0110_1xxx_xxxx__xxxx_xxxx_0111_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 2, 0]


class Rancho_SXTAB16_T1(MinorFUTiming):
    description = "Rancho_SXTAB16_T1"
    mask, match = t32_opcode("1111_1010_0010_xxxx__1111_xxxx_1xxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 2, 0]


class Rancho_SXTAH_T1(MinorFUTiming):
    description = "Rancho_SXTAH_T1"
    mask, match = t32_opcode("1111_1010_0000_xxxx__1111_xxxx_1xxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 2, 0]


class Rancho_SXTB_T1(MinorFUTiming):
    description = "Rancho_SXTB_T1"
    mask, match = t16_opcode("1011_0010_01xx_xxxx")


class Rancho_SXTB_T2(MinorFUTiming):
    description = "Rancho_SXTB_T2"
    mask, match = t32_opcode("1111_1010_0100_1111__1111_xxxx_1xxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 2, 0]


class Rancho_SXTB_SXTB16_SXTH_UXTB_UXTB16_UXTH_A1(MinorFUTiming):
    description = "Rancho_SXTB_SXTB16_SXTH_UXTB_UXTB16_UXTH_A1"
    # e6[9d]f0070 are undefined
    mask, match = a32_opcode("xxxx_0110_1xxx_1111__xxxx_xxxx_0111_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 2, 0]


class Rancho_SXTB16_T1(MinorFUTiming):
    description = "Rancho_SXTB16_T1"
    mask, match = t32_opcode("1111_1010_0010_1111__1111_xxxx_1xxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 2, 0]


class Rancho_SXTH_T1(MinorFUTiming):
    description = "Rancho_SXTH_T1"
    mask, match = t16_opcode("1011_0010_00xx_xxxx")


class Rancho_SXTH_T2(MinorFUTiming):
    description = "Rancho_SXTH_T2"
    mask, match = t32_opcode("1111_1010_0000_1111__1111_xxxx_1xxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 1, 2, 0]


class Rancho_UDIV_T1(MinorFUTiming):
    description = "Rancho_UDIV_T1"
    mask, match = t32_opcode("1111_1011_1011_xxxx__xxxx_xxxx_1111_xxxx")


udiv_lat_expr = expr_top(
    let(
        [
            ("left", src_reg(4)),
            ("right", src_reg(3)),
            ("left_size", un("SizeInBits", ref("left"))),
            (
                "right_size",
                un("SizeInBits", bin("UDiv", ref("right"), literal(2))),
            ),
            (
                "left_minus_right",
                if_expr(
                    bin("SLessThan", ref("left_size"), ref("right_size")),
                    literal(0),
                    bin("Sub", ref("left_size"), ref("right_size")),
                ),
            ),
        ],
        if_expr(
            bin("Equal", ref("right"), literal(0)),
            literal(0),
            bin("UDiv", ref("left_minus_right"), literal(4)),
        ),
    )
)


class Rancho_UDIV_A1(MinorFUTiming):
    description = "Rancho_UDIV_A1"
    mask, match = a32_opcode("xxxx_0111_0011_xxxx__xxxx_xxxx_0001_xxxx")
    extraCommitLat = 0
    srcRegsRelativeLats = []
    extraCommitLatExpr = udiv_lat_expr


class Rancho_UMAAL_T1(MinorFUTiming):
    description = "Rancho_UMAAL_T1"
    mask, match = t32_opcode("1111_1011_1110_xxxx__xxxx_xxxx_0110_xxxx")
    # z, z, z, dlo, dhi, l, r
    extraCommitLat = 1
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 0, 0]


class Rancho_UMAAL_A1(MinorFUTiming):
    description = "Rancho_UMAAL_A1"
    mask, match = a32_opcode("xxxx_0000_0100_xxxx__xxxx_xxxx_1001_xxxx")
    # z, z, z, dlo, dhi, l, r
    extraCommitLat = 1
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 0, 0]


class Rancho_UMLAL_T1(MinorFUTiming):
    description = "Rancho_UMLAL_T1"
    mask, match = t32_opcode("1111_1011_1110_xxxx__xxxx_xxxx_0000_xxxx")


class Rancho_UMLAL_A1(MinorFUTiming):
    description = "Rancho_UMLAL_A1"
    mask, match = t32_opcode("xxxx_0000_101x_xxxx__xxxx_xxxx_1001_xxxx")


class Rancho_UMULL_T1(MinorFUTiming):
    description = "Rancho_UMULL_T1"
    mask, match = t32_opcode("1111_1011_1010_xxxx__xxxx_xxxx_0000_xxxx")


class Rancho_UMULL_A1(MinorFUTiming):
    description = "Rancho_UMULL_A1"
    mask, match = a32_opcode("xxxx_0000_100x_xxxx__xxxx_xxxx_1001_xxxx")


class Rancho_USAD8_USADA8_A1(MinorFUTiming):
    description = "Rancho_USAD8_USADA8_A1"
    mask, match = a32_opcode("xxxx_0111_1000_xxxx__xxxx_xxxx_0001_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 2, 0]


class Rancho_USAD8_USADA8_A1_Suppress(MinorFUTiming):
    description = "Rancho_USAD8_USADA8_A1_Suppress"
    mask, match = a32_opcode("xxxx_0111_1000_xxxx__xxxx_xxxx_0001_xxxx")
    srcRegsRelativeLats = []
    suppress = True


class Rancho_VMOV_immediate_A1(MinorFUTiming):
    description = "Rancho_VMOV_register_A1"
    mask, match = a32_opcode("1111_0010_0x10_xxxx_xxxx_0001_xxx1_xxxx")
    # cpsr, z, z, z, hcptr, nsacr, cpacr, fpexc, scr
    srcRegsRelativeLats = [5, 5, 5, 5, 5, 5, 5, 5, 5, 0]


class Rancho_VMRS_A1(MinorFUTiming):
    description = "Rancho_VMRS_A1"
    mask, match = a32_opcode("xxxx_1110_1111_0001_xxxx_1010_xxx1_xxxx")
    # cpsr,z,z,z,hcptr,nsacr,cpacr,scr,r42
    srcRegsRelativeLats = [5, 5, 5, 5, 5, 5, 5, 5, 5, 0]


class Rancho_VMOV_register_A2(MinorFUTiming):
    description = "Rancho_VMOV_register_A2"
    mask, match = a32_opcode("xxxx_1110_1x11_0000_xxxx_101x_01x0_xxxx")
    # cpsr, z, r39, z, hcptr, nsacr, cpacr, fpexc, scr, f4, f5, f0, f1
    srcRegsRelativeLats = [
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        4,
        4,
        4,
        4,
        4,
        4,
        4,
        4,
        0,
    ]


# VADD.I16 D/VADD.F32 D/VADD.I8 D/VADD.I32 D
class Rancho_VADD2H_A32(MinorFUTiming):
    description = "Vadd2hALU"
    mask, match = a32_opcode("1111_0010_0xxx_xxxx__xxxx_1000_xxx0_xxxx")
    # cpsr, z, z, z, cpacr, fpexc, l0, r0, l1, r1, l2, r2, l3, r3 (for vadd2h)
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VAQQHN.I16 Q/VAQQHN.I32 Q/VAQQHN.I64 Q
class Rancho_VADDHN_A32(MinorFUTiming):
    description = "VaddhnALU"
    mask, match = a32_opcode("1111_0010_1xxx_xxxx__xxxx_0100_x0x0_xxxx")
    # cpsr, z, z, z, cpacr, fpexc, l0, l1, l2, l3, r0, r1, r2, r3
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0]


class Rancho_VADDL_A32(MinorFUTiming):
    description = "VaddlALU"
    mask, match = a32_opcode("1111_001x_1xxx_xxxx__xxxx_0000_x0x0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0]


class Rancho_VADDW_A32(MinorFUTiming):
    description = "Rancho_VADDW_A32"
    mask, match = a32_opcode("1111_001x_1xxx_xxxx__xxxx_0001_x0x0_xxxx")
    # cpsr, z, z, z, cpacr, fpexc, l0, l1, l2, l3, r0, r1
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 3, 3, 0]


# VHADD/VHSUB S8,S16,S32,U8,U16,U32 Q and D
class Rancho_VHADD_A32(MinorFUTiming):
    description = "Rancho_VHADD_A32"
    mask, match = a32_opcode("1111_001x_0xxx_xxxx__xxxx_00x0_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VPADAL_A32(MinorFUTiming):
    description = "VpadalALU"
    mask, match = a32_opcode("1111_0011_1x11_xx00__xxxx_0110_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0]


# VPADDH.I16
class Rancho_VPADDH_A32(MinorFUTiming):
    description = "VpaddhALU"
    mask, match = a32_opcode("1111_0010_0xxx_xxxx__xxxx_1011_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0]


# VPADDH.F32
class Rancho_VPADDS_A32(MinorFUTiming):
    description = "VpaddsALU"
    mask, match = a32_opcode("1111_0011_0x0x_xxxx__xxxx_1101_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0]


# VPADDL.S16
class Rancho_VPADDL_A32(MinorFUTiming):
    description = "VpaddlALU"
    mask, match = a32_opcode("1111_0011_1x11_xx00__xxxx_0010_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0]


# VRADDHN.I16
class Rancho_VRADDHN_A32(MinorFUTiming):
    description = "Rancho_VRADDHN_A32"
    mask, match = a32_opcode("1111_0011_1xxx_xxxx__xxxx_0100_x0x0_xxxx")
    # cpsr, z, z, z, cpacr, fpexc, l0, l1, l2, l3, r0, r1, r2, r3
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VRHADD_A32(MinorFUTiming):
    description = "VrhaddALU"
    mask, match = a32_opcode("1111_001x_0xxx_xxxx__xxxx_0001_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VQADD_A32(MinorFUTiming):
    description = "VqaddALU"
    mask, match = a32_opcode("1111_001x_0xxx_xxxx__xxxx_0000_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0]


class Rancho_VANDQ_A32(MinorFUTiming):
    description = "VandqALU"
    mask, match = a32_opcode("1111_0010_0x00_xxxx__xxxx_0001_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 5, 5, 5, 5, 5, 5, 5, 5, 0]


# VMUL (integer)
class Rancho_VMULI_A32(MinorFUTiming):
    description = "VmuliALU"
    mask, match = a32_opcode("1111_001x_0xxx_xxxx__xxxx_1001_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0]


# VBIC (reg)
class Rancho_VBIC_A32(MinorFUTiming):
    description = "VbicALU"
    mask, match = a32_opcode("1111_0010_0x01_xxxx__xxxx_0001_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 5, 5, 5, 5, 5, 5, 5, 5, 0]


# VBIF VBIT VBSL
class Rancho_VBIF_ETC_A32(MinorFUTiming):
    description = "VbifALU"
    mask, match = a32_opcode("1111_0011_0xxx_xxxx__xxxx_0001_xxx1_xxxx")
    srcRegsRelativeLats = [
        0,
        0,
        0,
        0,
        0,
        0,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        5,
        0,
    ]


class Rancho_VACGE_A32(MinorFUTiming):
    description = "VacgeALU"
    mask, match = a32_opcode("1111_0011_0xxx_xxxx__xxxx_1110_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VCEQ.F32
class Rancho_VCEQ_A32(MinorFUTiming):
    description = "VceqALU"
    mask, match = a32_opcode("1111_0010_0x0x_xxxx__xxxx_1110_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VCEQ.[IS]... register
class Rancho_VCEQI_A32(MinorFUTiming):
    description = "VceqiALU"
    mask, match = a32_opcode("1111_0011_0xxx_xxxx__xxxx_1000_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VCEQ.[IS]... immediate
class Rancho_VCEQII_A32(MinorFUTiming):
    description = "Rancho_VCEQII_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx01__xxxx_0x01_0xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VTST_A32(MinorFUTiming):
    description = "Rancho_VTST_A32"
    mask, match = a32_opcode("1111_0010_0xxx_xxxx__xxxx_1000_xxx1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0]


class Rancho_VCLZ_A32(MinorFUTiming):
    description = "Rancho_VCLZ_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx00__xxxx_0100_1xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VCNT_A32(MinorFUTiming):
    description = "Rancho_VCNT_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx00__xxxx_0101_0xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VEXT_A32(MinorFUTiming):
    description = "Rancho_VCNT_A32"
    mask, match = a32_opcode("1111_0010_1x11_xxxx__xxxx_xxxx_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VMAX VMIN integer
class Rancho_VMAXI_A32(MinorFUTiming):
    description = "Rancho_VMAXI_A32"
    mask, match = a32_opcode("1111_001x_0xxx_xxxx__xxxx_0110_xxxx_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VMAX VMIN float
class Rancho_VMAXS_A32(MinorFUTiming):
    description = "Rancho_VMAXS_A32"
    mask, match = a32_opcode("1111_0010_0xxx_xxxx__xxxx_1111_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0]


# VNEG integer
class Rancho_VNEGI_A32(MinorFUTiming):
    description = "Rancho_VNEGI_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx01__xxxx_0x11_1xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VNEG float
class Rancho_VNEGF_A32(MinorFUTiming):
    description = "Rancho_VNEGF_A32"
    mask, match = a32_opcode("xxxx_1110_1x11_0001__xxxx_101x_01x0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0]


# VREV16 VREV32 VREV64
class Rancho_VREVN_A32(MinorFUTiming):
    description = "Rancho_VREVN_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx00__xxxx_000x_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VQNEG_A32(MinorFUTiming):
    description = "Rancho_VQNEG_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx00__xxxx_0111_1xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0]


class Rancho_VSWP_A32(MinorFUTiming):
    description = "Rancho_VSWP_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx10__xxxx_0000_0xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 0]


class Rancho_VTRN_A32(MinorFUTiming):
    description = "Rancho_VTRN_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx10__xxxx_0000_1xx0_xxxx")
    # cpsr, z, z, z, cpact, fpexc, o0, d0, o1, d1, o2, d2, o3, d3
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0]


# VQMOVN VQMOVUN
class Rancho_VQMOVN_A32(MinorFUTiming):
    description = "Rancho_VQMOVN_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx10__xxxx_0010_xxx0_xxxx")
    # cpsr, z, z, z, cpact, fpexc, o[0], o[1], o[2], o[3], fpscr
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 0]


# VUZP double word
class Rancho_VUZP_A32(MinorFUTiming):
    description = "Rancho_VUZP_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx10__xxxx_0001_00x0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0]


# VDIV.F32
class Rancho_VDIV32_A32(MinorFUTiming):
    description = "Rancho_VDIV32_A32"
    mask, match = a32_opcode("xxxx_1110_1x00_xxxx__xxxx_1010_x0x0_xxxx")
    # cpsr, z, z, z, cpact, fpexc, fpscr_exc, l, r
    extraCommitLat = 9
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 20, 4, 4, 0]


# VDIV.F64
class Rancho_VDIV64_A32(MinorFUTiming):
    description = "Rancho_VDIV64_A32"
    mask, match = a32_opcode("xxxx_1110_1x00_xxxx__xxxx_1011_x0x0_xxxx")
    # cpsr, z, z, z, cpact, fpexc, fpscr_exc, l, r
    extraCommitLat = 18
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 20, 4, 4, 0]


class Rancho_VZIP_A32(MinorFUTiming):
    description = "Rancho_VZIP_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx10__xxxx_0001_1xx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 0]


# VPMAX integer
class Rancho_VPMAX_A32(MinorFUTiming):
    description = "Rancho_VPMAX_A32"
    mask, match = a32_opcode("1111_001x_0xxx_xxxx__xxxx_1010_xxxx_xxxx")
    # cpsr, z, z, z, cpact, fpexc, l0, r0, l1, r1, fpscr
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 0]


# VPMAX float
class Rancho_VPMAXF_A32(MinorFUTiming):
    description = "Rancho_VPMAXF_A32"
    mask, match = a32_opcode("1111_0011_0xxx_xxxx__xxxx_1111_xxx0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 0]


class Rancho_VMOVN_A32(MinorFUTiming):
    description = "Rancho_VMOVN_A32"
    mask, match = a32_opcode("1111_0011_1x11_xx10__xxxx_0010_00x0_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 0]


class Rancho_VMOVL_A32(MinorFUTiming):
    description = "Rancho_VMOVL_A32"
    mask, match = a32_opcode("1111_001x_1xxx_x000__xxxx_1010_00x1_xxxx")
    srcRegsRelativeLats = [0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 0]


# VSQRT.F64
class Rancho_VSQRT64_A32(MinorFUTiming):
    description = "Rancho_VSQRT64_A32"
    mask, match = a32_opcode("xxxx_1110_1x11_0001__xxxx_1011_11x0_xxxx")
    extraCommitLat = 18
    srcRegsRelativeLats = []


# VSQRT.F32
class Rancho_VSQRT32_A32(MinorFUTiming):
    description = "Rancho_VSQRT32_A32"
    mask, match = a32_opcode("xxxx_1110_1x11_0001__xxxx_1010_11x0_xxxx")
    extraCommitLat = 9
    srcRegsRelativeLats = []


class Rancho_FloatSimdFU(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "FloatAdd",
            "FloatCmp",
            "FloatCvt",
            "FloatMult",
            "FloatDiv",
            "FloatSqrt",
            "FloatMisc",
            "FloatMultAcc",
            "SimdAdd",
            "SimdAddAcc",
            "SimdAlu",
            "SimdCmp",
            "SimdCvt",
            "SimdMisc",
            "SimdMult",
            "SimdMultAcc",
            "SimdMatMultAcc",
            "SimdShift",
            "SimdShiftAcc",
            "SimdSqrt",
            "SimdFloatAdd",
            "SimdFloatAlu",
            "SimdFloatCmp",
            "SimdFloatCvt",
            "SimdFloatDiv",
            "SimdFloatMisc",
            "SimdFloatMult",
            "SimdFloatMultAcc",
            "SimdFloatMatMultAcc",
            "SimdFloatSqrt",
        ]
    )

    timings = [
        # VUZP and VZIP must be before VADDW/L
        Rancho_VUZP_A32(),
        Rancho_VZIP_A32(),
        Rancho_VADD2H_A32(),
        Rancho_VADDHN_A32(),
        Rancho_VADDL_A32(),
        Rancho_VADDW_A32(),
        Rancho_VHADD_A32(),
        Rancho_VPADAL_A32(),
        Rancho_VPADDH_A32(),
        Rancho_VPADDS_A32(),
        Rancho_VPADDL_A32(),
        Rancho_VRADDHN_A32(),
        Rancho_VRHADD_A32(),
        Rancho_VQADD_A32(),
        Rancho_VANDQ_A32(),
        Rancho_VBIC_A32(),
        Rancho_VBIF_ETC_A32(),
        Rancho_VACGE_A32(),
        Rancho_VCEQ_A32(),
        Rancho_VCEQI_A32(),
        Rancho_VCEQII_A32(),
        Rancho_VTST_A32(),
        Rancho_VCLZ_A32(),
        Rancho_VCNT_A32(),
        Rancho_VEXT_A32(),
        Rancho_VMAXI_A32(),
        Rancho_VMAXS_A32(),
        Rancho_VNEGI_A32(),
        Rancho_VNEGF_A32(),
        Rancho_VREVN_A32(),
        Rancho_VQNEG_A32(),
        Rancho_VSWP_A32(),
        Rancho_VTRN_A32(),
        Rancho_VPMAX_A32(),
        Rancho_VPMAXF_A32(),
        Rancho_VMOVN_A32(),
        Rancho_VMRS_A1(),
        Rancho_VMOV_immediate_A1(),
        Rancho_VMOV_register_A2(),
        Rancho_VQMOVN_A32(),
        Rancho_VMOVL_A32(),
        Rancho_VDIV32_A32(),
        Rancho_VDIV64_A32(),
        Rancho_VSQRT32_A32(),
        Rancho_VSQRT64_A32(),
        Rancho_VMULI_A32(),
        # Add before here
        Rancho_FMADD_A64(),
        Rancho_FMSUB_D_A64(),
        Rancho_FMOV_A64(),
        Rancho_ADD_SUB_vector_scalar_A64(),
        Rancho_ADD_SUB_vector_vector_A64(),
        Rancho_FDIV_scalar_32_A64(),
        Rancho_FDIV_scalar_64_A64(),
        Rancho_DefaultA64Vfp(),
        Rancho_DefaultVfp(),
    ]

    opLat = 6


class Rancho_IntFU(MinorFU):
    opClasses = minorMakeOpClassSet(["IntAlu"])
    # IMPORTANT! Keep the order below, add new entries *at the head*
    timings = [
        Rancho_SSAT_USAT_no_shift_A1(),
        Rancho_SSAT_USAT_shift_A1(),
        Rancho_SSAT16_USAT16_A1(),
        Rancho_QADD_QSUB_A1(),
        Rancho_QADD_QSUB_T1(),
        Rancho_QADD_ETC_A1(),
        Rancho_QASX_QSAX_UQASX_UQSAX_T1(),
        Rancho_QADD_ETC_T1(),
        Rancho_USUB_ETC_A1(),
        Rancho_ADD_ETC_A1(),
        Rancho_ADD_ETC_T1(),
        Rancho_QDADD_QDSUB_A1(),
        Rancho_QDADD_QDSUB_T1(),
        Rancho_SASX_SHASX_UASX_UHASX_A1(),
        Rancho_SHSAX_SSAX_UHSAX_USAX_A1(),
        Rancho_SXTB_SXTB16_SXTH_UXTB_UXTB16_UXTH_A1(),
        # Must be after Rancho_SXTB_SXTB16_SXTH_UXTB_UXTB16_UXTH_A1
        Rancho_SXTAB_SXTAB16_SXTAH_UXTAB_UXTAB16_UXTAH_A1(),
        Rancho_SXTAB_T1(),
        Rancho_SXTAB16_T1(),
        Rancho_SXTAH_T1(),
        Rancho_SXTB_T2(),
        Rancho_SXTB16_T1(),
        Rancho_SXTH_T2(),
        Rancho_PKH_A1(),
        Rancho_PKH_T1(),
        Rancho_SBFX_UBFX_A1(),
        Rancho_SEL_A1(),
        Rancho_RBIT_A1(),
        Rancho_REV_REV16_A1(),
        Rancho_REVSH_A1(),
        Rancho_USAD8_USADA8_A1(),
        Rancho_BFI_A1(),
        Rancho_BFI_T1(),
        Rancho_CMN_register_A1(),
        Rancho_CMN_immediate_A1(),
        Rancho_CMP_register_A1(),
        Rancho_CMP_immediate_A1(),
        Rancho_DataProcessingNoShift(),
        Rancho_DataProcessingMovShiftr(),
        Rancho_DataProcessingMayShift(),
        Rancho_Cxxx_A64(),
        Rancho_DefaultA64Int(),
        Rancho_DefaultInt(),
    ]
    opLat = 3


class Rancho_Int2FU(MinorFU):
    opClasses = minorMakeOpClassSet(["IntAlu"])
    # IMPORTANT! Keep the order below, add new entries *at the head*
    timings = [
        Rancho_SSAT_USAT_no_shift_A1(),
        Rancho_SSAT_USAT_shift_A1(),
        Rancho_SSAT16_USAT16_A1(),
        Rancho_QADD_QSUB_A1(),
        Rancho_QADD_QSUB_T1(),
        Rancho_QADD_ETC_A1(),
        Rancho_QASX_QSAX_UQASX_UQSAX_T1(),
        Rancho_QADD_ETC_T1(),
        Rancho_USUB_ETC_A1(),
        Rancho_ADD_ETC_A1(),
        Rancho_ADD_ETC_T1(),
        Rancho_QDADD_QDSUB_A1(),
        Rancho_QDADD_QDSUB_T1(),
        Rancho_SASX_SHASX_UASX_UHASX_A1(),
        Rancho_SHSAX_SSAX_UHSAX_USAX_A1(),
        Rancho_SXTB_SXTB16_SXTH_UXTB_UXTB16_UXTH_A1(),
        # Must be after Rancho_SXTB_SXTB16_SXTH_UXTB_UXTB16_UXTH_A1
        Rancho_SXTAB_SXTAB16_SXTAH_UXTAB_UXTAB16_UXTAH_A1(),
        Rancho_SXTAB_T1(),
        Rancho_SXTAB16_T1(),
        Rancho_SXTAH_T1(),
        Rancho_SXTB_T2(),
        Rancho_SXTB16_T1(),
        Rancho_SXTH_T2(),
        Rancho_PKH_A1(),
        Rancho_PKH_T1(),
        Rancho_SBFX_UBFX_A1(),
        Rancho_SEL_A1_Suppress(),
        Rancho_RBIT_A1(),
        Rancho_REV_REV16_A1(),
        Rancho_REVSH_A1(),
        Rancho_USAD8_USADA8_A1_Suppress(),
        Rancho_BFI_A1(),
        Rancho_BFI_T1(),
        Rancho_CMN_register_A1(),  # Need to check for shift
        Rancho_CMN_immediate_A1(),
        Rancho_CMP_register_A1(),  # Need to check for shift
        Rancho_CMP_immediate_A1(),
        Rancho_DataProcessingNoShift(),
        Rancho_DataProcessingAllowShifti(),
        # Rancho_DataProcessingAllowMovShiftr(),
        # Data processing ops that match SuppressShift but are *not*
        # to be suppressed here
        Rancho_CLZ_A1(),
        Rancho_CLZ_T1(),
        Rancho_DataProcessingSuppressShift(),
        # Can you dual issue a branch?
        # Rancho_DataProcessingSuppressBranch(),
        Rancho_Cxxx_A64(),
        Rancho_DefaultA64Int(),
        Rancho_DefaultInt(),
    ]
    opLat = 3


class Rancho_IntMulFU(MinorFU):
    opClasses = minorMakeOpClassSet(["IntMult"])
    timings = [
        Rancho_MLA_A1(),
        Rancho_MLA_T1(),
        Rancho_MLS_A1(),
        Rancho_MLS_T1(),
        Rancho_SMLABB_A1(),
        Rancho_SMLABB_T1(),
        Rancho_SMLAWB_A1(),
        Rancho_SMLAWB_T1(),
        Rancho_SMLAD_A1(),
        Rancho_SMLAD_T1(),
        Rancho_SMMUL_A1(),
        Rancho_SMMUL_T1(),
        # SMMUL_A1 must be before SMMLA_A1
        Rancho_SMMLA_A1(),
        Rancho_SMMLA_T1(),
        Rancho_SMMLS_A1(),
        Rancho_SMMLS_T1(),
        Rancho_UMAAL_A1(),
        Rancho_UMAAL_T1(),
        Rancho_MADD_A64(),
        Rancho_DefaultA64Mul(),
        Rancho_DefaultMul(),
    ]
    opLat = 3
    cantForwardFromFUIndices = [0, 1, 5]  # Int1, Int2, Mem


class Rancho_IntDivFU(MinorFU):
    opClasses = minorMakeOpClassSet(["IntDiv"])
    timings = [Rancho_SDIV_A1(), Rancho_UDIV_A1(), Rancho_SDIV_A64()]
    issueLat = 3
    opLat = 3


class Rancho_MemFU(MinorFU):
    opClasses = minorMakeOpClassSet(
        ["MemRead", "MemWrite", "FloatMemRead", "FloatMemWrite"]
    )
    timings = [Rancho_DefaultMem(), Rancho_DefaultMem64()]
    opLat = 1
    cantForwardFromFUIndices = [5]  # Mem (this FU)


class Rancho_MiscFU(MinorFU):
    opClasses = minorMakeOpClassSet(["IprAccess", "InstPrefetch"])
    opLat = 1


class Rancho_FUPool(MinorFUPool):
    funcUnits = [
        Rancho_IntFU(),  # 0
        Rancho_Int2FU(),  # 1
        Rancho_IntMulFU(),  # 2
        Rancho_IntDivFU(),  # 3
        Rancho_FloatSimdFU(),  # 4
        Rancho_MemFU(),  # 5
        Rancho_MiscFU(),  # 6
    ]


class Rancho_MMU(ArmMMU):
    itb = ArmTLB(entry_type="instruction", size=256)
    dtb = ArmTLB(entry_type="data", size=256)


class Rancho_BTB(SimpleBTB):
    numEntries = 128
    associativity = 1
    tagBits = 18
    btbReplPolicy = NRURP()
    btbIndexingPolicy = SetAssociative(
        size="512B", entry_size=4, assoc=associativity, tag_bits=tagBits
    )


class Rancho_BP(TournamentBP):
    btb = Rancho_BTB()
    ras = ReturnAddrStack(numEntries=8)
    localPredictorSize = 64
    localCtrBits = 2
    localHistoryTableSize = 64
    globalPredictorSize = 1024
    globalCtrBits = 2
    choicePredictorSize = 1024
    choiceCtrBits = 2
    instShiftAmt = 2


class Rancho_ICache(Cache):
    data_latency = 1
    tag_latency = 1
    response_latency = 1
    mshrs = 2
    tgts_per_mshr = 8
    size = "32kB"
    assoc = 2
    # No prefetcher, this is handled by the core


class Rancho_DCache(Cache):
    data_latency = 1
    tag_latency = 1
    response_latency = 1
    mshrs = 4
    tgts_per_mshr = 8
    size = "32kB"
    assoc = 4
    write_buffers = 4
    prefetcher = StridePrefetcher(queue_size=4, degree=4)


class Rancho_L2(Cache):
    data_latency = 13
    tag_latency = 13
    response_latency = 5
    mshrs = 4
    tgts_per_mshr = 8
    size = "1024kB"
    assoc = 16
    write_buffers = 16
    # prefetcher FIXME


class Rancho(ArmO3CPU):
    # Inherit the doc string from the module to avoid repeating it
    # here.
    __doc__ = __doc__

    decodeToFetchDelay = 1
    renameToFetchDelay = 1
    iewToFetchDelay = 1
    commitToFetchDelay = 1
    fetchWidth = 8
    fetchBufferSize = 64
    fetchQueueSize = 32

    renameToDecodeDelay = 1
    iewToDecodeDelay = 1
    commitToDecodeDelay = 1
    fetchToDecodeDelay = 1
    decodeWidth = 8

    iewToRenameDelay = 1
    commitToRenameDelay = 1
    decodeToRenameDelay = 1
    renameWidth = 8

    commitToIEWDelay = 1
    renameToIEWDelay = 2
    issueToExecuteDelay = 1
    dispatchWidth = 8
    issueWidth = 8
    wbWidth = 8
    fuPool = DefaultFUPool()

    iewToCommitDelay = 1
    renameToROBDelay = 1
    commitWidth = 8
    squashWidth = 8
    trapLatency = 13
    fetchTrapLatency = 1

    backComSize = 8
    forwardComSize = 8
    LQEntries = 128
    SQEntries = 128
    LSQDepCheckShift = 4
    LSQCheckLoads = True
    store_set_clear_period = 250000
    LFSTSize = 1024
    SSITSize = 1024
    SSITAssoc = SSITSize
    SSITReplPolicy = LRURP()
    SSITIndexingPolicy = SetAssociative(
        size="4kB", entry_size=4, assoc=SSITAssoc
    )

    numPhysIntRegs = 512
    numPhysFloatRegs = 256
    numPhysVecRegs = 256
    numPhysVecPredRegs = Param.Unsigned(
        32, "Number of physical predicate registers"
    )
    # most ISAs don't use condition-code regs, so default is 0
    numPhysCCRegs = Param.Unsigned(32, "Number of physical cc registers")
    numIQEntries = Param.Unsigned(96, "Number of instruction queue entries")
    numROBEntries = Param.Unsigned(75, "Number of reorder buffer entries")

    smtCommitPolicy = "RoundRobin"

    branchPred = Rancho_BP()
    needsTSO = False

    mmu = Rancho_MMU()


__all__ = [
    "Rancho_BP",
    "Rancho_ITB",
    "Rancho_DTB",
    "Rancho_ICache",
    "Rancho_DCache",
    "Rancho_L2",
    "Rancho",
]
