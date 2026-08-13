#! /usr/bin/env python3
"""Unit tests for harness_registry.py.

Run directly with:
    python3 scripts/tasks/test_harness_registry.py
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import harness_registry as hr

MINIMAL = """
[[harness]]
name = "alpha"
path = "harness/rust/alpha"
kind = "differential"
status = "gate"
gates = "alpha must equal beta"
build = ["cargo", "build"]
run = ["cargo", "test"]
requires = ["cargo"]
ci = "pr"
runtime = "~1m"
"""


class LoadRegistryTest(unittest.TestCase):
    def _root_with(self, text):
        root = Path(tempfile.mkdtemp())
        (root / "harness").mkdir()
        (root / "harness" / "registry.toml").write_text(text)
        return root

    def test_loads_all_fields(self):
        entries = hr.load_registry(self._root_with(MINIMAL))
        self.assertEqual(len(entries), 1)
        h = entries[0]
        self.assertEqual(h.name, "alpha")
        self.assertEqual(h.kind, "differential")
        self.assertEqual(h.run, ["cargo", "test"])
        self.assertEqual(h.requires, ["cargo"])
        self.assertEqual(h.ci, "pr")

    def test_env_defaults_to_empty_dict(self):
        h = hr.load_registry(self._root_with(MINIMAL))[0]
        self.assertEqual(h.env, {})

    def test_missing_gates_is_rejected(self):
        text = MINIMAL.replace('gates = "alpha must equal beta"\n', "")
        with self.assertRaises(ValueError) as cm:
            hr.load_registry(self._root_with(text))
        self.assertIn("gates", str(cm.exception))

    def test_bad_status_is_rejected(self):
        text = MINIMAL.replace('status = "gate"', 'status = "probably fine"')
        with self.assertRaises(ValueError) as cm:
            hr.load_registry(self._root_with(text))
        self.assertIn("status", str(cm.exception))

    def test_bad_ci_tier_is_rejected(self):
        text = MINIMAL.replace('ci = "pr"', 'ci = "sometimes"')
        with self.assertRaises(ValueError) as cm:
            hr.load_registry(self._root_with(text))
        self.assertIn("ci", str(cm.exception))

    def test_unknown_requirement_is_rejected(self):
        text = MINIMAL.replace('requires = ["cargo"]', 'requires = ["a-pony"]')
        with self.assertRaises(ValueError) as cm:
            hr.load_registry(self._root_with(text))
        self.assertIn("a-pony", str(cm.exception))

    def test_duplicate_names_are_rejected(self):
        with self.assertRaises(ValueError) as cm:
            hr.load_registry(self._root_with(MINIMAL + MINIMAL))
        self.assertIn("alpha", str(cm.exception))


class ProbeTest(unittest.TestCase):
    def test_cargo_is_present(self):
        self.assertTrue(hr.probe("cargo", Path.cwd()))

    def test_every_requirement_key_has_a_fix_hint(self):
        for key in hr.PROBES:
            self.assertTrue(hr.PROBES[key], f"{key} has no fix hint")

    def test_unknown_requirement_probes_false(self):
        self.assertFalse(hr.probe("a-pony", Path.cwd()))

    def test_song_corpus_via_deluge_backup(self):
        # Test that song-corpus probe returns True when DELUGE_BACKUP points
        # at a real directory and DELUGE_SONG_CORPUS is unset.
        backup_dir = Path(tempfile.mkdtemp())
        root = Path("/home/kate/GitHub/DelugeFirmware")
        with mock.patch.dict(
            os.environ, {"DELUGE_SONG_CORPUS": "", "DELUGE_BACKUP": str(backup_dir)}
        ):
            self.assertTrue(hr.probe("song-corpus", root))


class MarkdownTableTest(unittest.TestCase):
    def test_table_has_a_row_per_harness(self):
        entries = hr.load_registry(self._root())
        table = hr.markdown_table(entries)
        self.assertIn("| alpha |", table)
        self.assertIn("alpha must equal beta", table)
        self.assertTrue(table.startswith("|"))

    def test_table_escapes_pipes_in_gates(self):
        # Ensure pipes in gates text are escaped to preserve table structure
        entries = hr.load_registry(self._root_with_pipe())
        table = hr.markdown_table(entries)
        # The pipe in "alpha|beta" should be escaped as "alpha\|beta"
        self.assertIn("alpha\\|beta", table)
        # Count pipes in header row and data row to verify structure is preserved
        lines = table.split("\n")
        header_row = lines[0]
        data_row = lines[2]
        # Both should have the same number of pipes (7: leading + 5 separators + trailing)
        self.assertEqual(header_row.count("|"), 7)
        # Data row has 8 because of the escaped pipe, but that's just the escaped
        # version, not an extra separator
        self.assertGreaterEqual(data_row.count("|"), 7)

    def _root(self):
        root = Path(tempfile.mkdtemp())
        (root / "harness").mkdir()
        (root / "harness" / "registry.toml").write_text(MINIMAL)
        return root

    def _root_with_pipe(self):
        root = Path(tempfile.mkdtemp())
        (root / "harness").mkdir()
        toml_with_pipe = MINIMAL.replace(
            'gates = "alpha must equal beta"', 'gates = "alpha|beta must equal charlie"'
        )
        (root / "harness" / "registry.toml").write_text(toml_with_pipe)
        return root


if __name__ == "__main__":
    unittest.main()
