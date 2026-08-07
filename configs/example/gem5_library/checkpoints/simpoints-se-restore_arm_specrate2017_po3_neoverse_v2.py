"""Run SPEC CPU2017 SimPoints with the Neoverse V2 PO3 model.

Select the memory-dependence predictor with ``SPEC_MEM_DEP_PREDICTOR``:
``store_set`` (the default), ``scbf``, ``phast``, or ``mdp_tage``. SCBF geometry is
configurable with ``SPEC_SCBF_SEGMENTS``,
``SPEC_SCBF_ENTRIES_PER_SEGMENT``, ``SPEC_SCBF_HISTORY_ENTRIES``, and
``SPEC_SCBF_FILTER_FORWARDED_NUKES``. PHAST uses the HPCA 2024 geometry by
default; its parameters use the ``SPEC_PHAST_`` prefix. MDP-TAGE uses the
standalone HPCA 2024 comparison geometry and the ``SPEC_MDP_TAGE_`` prefix.

Set ``SPEC_MEASURE_CYCLES`` to terminate the measured interval after an exact
number of CPU cycles instead of using the instruction-based SimPoint interval.
"""

import os
import runpy
from pathlib import Path

os.environ.setdefault("SPEC_CPU_MODEL", "neoverse-v2-po3")

runpy.run_path(
    Path(__file__).with_name("simpoints-se-restore_arm_specrate2017_po3.py"),
    run_name="__main__",
)
