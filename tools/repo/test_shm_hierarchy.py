"""Directed full/scoped hierarchy import tests; these do not qualify a vendor runtime."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ShmHierarchyTests(unittest.TestCase):
    def test_scoped_import_routes_and_listing(self):
        for defines in ([], ["-DONDAS_SHM_USE_ALIAS_IDENTITY"]):
            with self.subTest(defines=defines), tempfile.TemporaryDirectory(
                prefix="ondas-shm-hierarchy-"
            ) as temporary:
                executable = Path(temporary) / "hierarchy"
                database = Path(temporary) / "db"
                database.mkdir()
                subprocess.run([
                    "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-subobject-linkage", *defines,
                    str(ROOT / "tools/repo/shm_hierarchy_test.cpp"), "-o", str(executable),
                ], check=True)
                subprocess.run([str(executable), str(database)], check=True)
