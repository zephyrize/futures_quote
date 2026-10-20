#!/usr/bin/env python3
"""验证核对工具不会把同版本号、错误产品或下载失败误报为协议一致。"""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/check_cme_schema.py"
SPEC = importlib.util.spec_from_file_location("check_cme_schema", TOOL)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)
SCHEMA = CHECKER.LOCAL_SCHEMA.read_bytes()


class SchemaCheck(unittest.TestCase):
    def run_candidate(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.xml"
            path.write_bytes(data)
            result = subprocess.run(
                [sys.executable, str(TOOL), "--official-file", str(path)],
                capture_output=True, text=True, check=False)
            return result.returncode, json.loads(result.stdout or result.stderr)

    def test_identical(self):
        code, result = self.run_candidate(SCHEMA)
        self.assertEqual(code, 0)
        self.assertEqual(result["status"], "identical")
        self.assertEqual(result["source_mode"], "offline_file_unverified_origin")

    def test_same_version_changed_content(self):
        candidate = SCHEMA.replace(b'description="20230411"', b'description="changed"', 1)
        self.assertNotEqual(SCHEMA, candidate)
        code, result = self.run_candidate(candidate)
        self.assertEqual(code, 1)
        self.assertEqual(result["local"]["version"], result["candidate"]["version"])
        self.assertNotEqual(result["local"]["sha256"], result["candidate"]["sha256"])

    def test_html_rejected(self):
        self.assertEqual(self.run_candidate(b"<html>Login required</html>")[0], 2)

    def test_other_sbe_product_rejected(self):
        candidate = SCHEMA.replace(b'package="mktdata"', b'package="iLinkBinary"', 1)
        self.assertEqual(self.run_candidate(candidate)[0], 2)

    def test_malformed_xml_rejected(self):
        self.assertEqual(self.run_candidate(SCHEMA[:100])[0], 2)

    def test_download_failure_not_identical(self):
        with mock.patch.object(sys, "argv", [str(TOOL)]), \
                mock.patch.object(CHECKER.urllib.request, "urlopen",
                                  side_effect=urllib.error.URLError("offline")), \
                mock.patch("sys.stderr"):
            self.assertEqual(CHECKER.main(), 2)


if __name__ == "__main__":
    unittest.main()
