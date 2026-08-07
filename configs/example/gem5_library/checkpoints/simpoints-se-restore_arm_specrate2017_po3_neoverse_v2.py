"""Run SPEC CPU2017 SimPoints with the Neoverse V2 PO3 model.

Select the memory-dependence predictor with ``SPEC_MEM_DEP_PREDICTOR``:
``store_set`` (the default) or ``scbf``. SCBF geometry is configurable with
``SPEC_SCBF_SEGMENTS``, ``SPEC_SCBF_ENTRIES_PER_SEGMENT``, and
``SPEC_SCBF_HISTORY_ENTRIES``.
"""

import os
import runpy
from pathlib import Path

os.environ.setdefault("SPEC_CPU_MODEL", "neoverse-v2-po3")

runpy.run_path(
    Path(__file__).with_name("simpoints-se-restore_arm_specrate2017_po3.py"),
    run_name="__main__",
)
