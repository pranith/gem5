# Copyright (c) 2020 ARM Limited
# All rights reserved
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
# Copyright (c) 2018 The Regents of the University of California
# All Rights Reserved.
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

"""
Test file containing simple workloads to run on CPU models.
Each test takes ~10 seconds to run.
"""

import re

from testlib import *

workloads = ("Bubblesort", "FloatMM")

valid_isas = {
    constants.vega_x86_tag: (
        "X86AtomicSimpleCPU",
        "X86TimingSimpleCPU",
        "X86DerivO3CPU",
        "X86PO3CPU",
    ),
    constants.arm_tag: (
        "ArmAtomicSimpleCPU",
        "ArmTimingSimpleCPU",
        "ArmMinorCPU",
        "ArmDerivO3CPU",
        "ArmPO3CPU",
    ),
    constants.riscv_tag: (
        "RiscvAtomicSimpleCPU",
        "RiscvTimingSimpleCPU",
        "RiscvMinorCPU",
        "RiscvDerivO3CPU",
        "RiscvPO3CPU",
    ),
}


base_path = joinpath(config.bin_path, "cpu_tests")

base_url = config.resource_url + "/test-progs/cpu-tests/bin/"

isa_url = {
    constants.vega_x86_tag: base_url + "x86",
    constants.arm_tag: base_url + "arm",
    constants.riscv_tag: base_url + "riscv",
}

for isa in valid_isas:
    path = joinpath(base_path, isa.lower())
    for workload in workloads:
        ref_path = joinpath(getcwd(), "ref", workload)
        verifiers = (verifier.MatchStdout(ref_path),)

        url = isa_url[isa] + "/" + workload
        workload_binary = DownloadedProgram(url, path, workload)
        binary = joinpath(workload_binary.path, workload)

        for cpu in valid_isas[isa]:
            gem5_verify_config(
                name=f"cpu_test_{cpu}_{workload}",
                verifiers=verifiers,
                config=joinpath(getcwd(), "run.py"),
                config_args=[f"--cpu={cpu}", binary],
                valid_isas=(constants.all_compiled_tag,),
                fixtures=[workload_binary],
            )


# The 32-bit ARM hello binary contains a predicated load that does not start
# a translation when its predicate is false. PO3 must retire it without
# waiting indefinitely in the explicit TLB stage.
arm_hello = joinpath(
    getcwd(), "..", "..", "test-progs", "hello", "bin", "arm", "linux", "hello"
)
gem5_verify_config(
    name="cpu_test_ArmPO3CPU_predicated_load",
    verifiers=(verifier.MatchRegex(re.compile(r"Hello world!")),),
    config=joinpath(getcwd(), "run.py"),
    config_args=["--cpu=ArmPO3CPU", arm_hello],
    valid_isas=(constants.arm_tag,),
)

gem5_verify_config(
    name="cpu_test_ArmPO3CPU_decoupled_fetch_drain",
    verifiers=(verifier.MatchRegex(re.compile(r"Hello world!")),),
    config=joinpath(getcwd(), "run.py"),
    config_args=[
        "--cpu=ArmPO3CPU",
        "--decoupled-front-end",
        "--drain-at-ticks=200000",
        arm_hello,
    ],
    valid_isas=(constants.arm_tag,),
)

gem5_verify_config(
    name="cpu_test_ArmPO3CPU_pipelined_execute_drain",
    verifiers=(verifier.MatchRegex(re.compile(r"Hello world!")),),
    config=joinpath(getcwd(), "run.py"),
    config_args=[
        "--cpu=ArmPO3CPU",
        "--po3-reg-read-latency=2",
        "--po3-execute-latency=2",
        "--drain-at-ticks=200000",
        arm_hello,
    ],
    valid_isas=(constants.arm_tag,),
)

gem5_verify_config(
    name="cpu_test_ArmPO3CPU_delayed_writeback",
    verifiers=(verifier.MatchRegex(re.compile(r"Hello world!")),),
    config=joinpath(getcwd(), "run.py"),
    config_args=[
        "--cpu=ArmPO3CPU",
        "--po3-mem-pipeline-off",
        arm_hello,
    ],
    valid_isas=(constants.arm_tag,),
)

gem5_verify_config(
    name="cpu_test_ArmPO3CPU_merge_buffer_drain",
    verifiers=(verifier.MatchRegex(re.compile(r"Hello world!")),),
    config=joinpath(getcwd(), "run.py"),
    config_args=[
        "--cpu=ArmPO3CPU",
        "--merge-buffer",
        # At this tick the hello workload has committed stores whose merge
        # buffer entries have not yet completed their cache writes.
        "--drain-at-ticks=200000",
        "--recv-resp-buffer-size=16",
        arm_hello,
    ],
    valid_isas=(constants.arm_tag,),
)

for cpu, hello_binary in (
    ("X86PO3CPU", "x86"),
    ("RiscvPO3CPU", "riscv"),
):
    binary = joinpath(
        getcwd(),
        "..",
        "..",
        "test-progs",
        "hello",
        "bin",
        hello_binary,
        "linux",
        "hello",
    )
    gem5_verify_config(
        name=f"cpu_test_{cpu}_hello",
        verifiers=(verifier.MatchRegex(re.compile(r"Hello world!")),),
        config=joinpath(getcwd(), "run.py"),
        config_args=[f"--cpu={cpu}", binary],
        valid_isas=(constants.all_compiled_tag,),
    )

x86_hello = joinpath(
    getcwd(), "..", "..", "test-progs", "hello", "bin", "x86", "linux", "hello"
)
gem5_verify_config(
    name="cpu_test_X86PO3CPU_merge_buffer",
    verifiers=(verifier.MatchRegex(re.compile(r"Hello world!")),),
    config=joinpath(getcwd(), "run.py"),
    config_args=["--cpu=X86PO3CPU", "--merge-buffer", x86_hello],
    valid_isas=(constants.all_compiled_tag,),
)
