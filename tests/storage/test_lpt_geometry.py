#!/usr/bin/env python3
import importlib.util
from pathlib import Path

path = Path(__file__).parents[2] / "tools" / "ubi" / "ubifs-lpt-geometry.py"
spec = importlib.util.spec_from_file_location("lpt_geometry", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

base = module.geometry(82, 82)
assert base["main_lebs"] == 73
assert base["lnum_bits"] == 7
assert base["pcnt_bits"] == 5
assert base["pnode_sz"] == 17
assert base["nnode_sz"] == 12
assert base["lpt_height"] == 3
assert base["pnode_cnt"] == 19
assert base["nnode_cnt"] == 8
assert base["required_lpt_lebs"] <= 2

# A patched but not yet grown filesystem retains the original pnode population.
patched = module.geometry(256, 82)
assert patched["lpt_height"] == 3
assert patched["pnode_sz"] == base["pnode_sz"]
assert patched["nnode_sz"] == base["nnode_sz"]
assert patched["pnode_cnt"] == base["pnode_cnt"]

# The capacity tree gains another level above 265 maximum LEBs.
assert module.geometry(265, 82)["lpt_height"] == 3
assert module.geometry(266, 82)["lpt_height"] == 4

for target in (82, 100, 128, 139, 160, 256, 400, 500, 600, 638):
    grown = module.geometry(target, target)
    assert grown["reservation_ok"]
    assert grown["small_lpt"]

print("lpt-geometry-tests-ok")
