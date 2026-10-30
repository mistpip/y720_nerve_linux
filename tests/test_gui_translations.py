# SPDX-License-Identifier: MIT
"""Require English coverage for all Chinese source messages, including format strings."""
import ast
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
STRING = r'"(?:\\.|[^"\\\n])*"'
TOKEN = re.compile(r'(?P<string>' + STRING + r")|'(?:\\.|[^'\\\n])*'|//[^\n]*|/\*[\s\S]*?\*/")


class GuiTranslations(unittest.TestCase):
    def test_catalog_is_complete_and_format_safe(self):
        text = (ROOT / "gui/src/y720-i18n.c").read_text()
        pairs = [(ast.literal_eval(zh), ast.literal_eval(en)) for zh, en in
                 re.findall(r'\{(' + STRING + r'),\s*(' + STRING + r')\}', text)]
        catalog = dict(pairs)
        self.assertEqual(len(pairs), len(catalog), "duplicate translation keys")
        spec = re.compile(r'%(?:[0-9]+\$)?[-+#0 ]*\d*(?:\.\d+)?[a-zA-Z]')
        for zh, en in pairs:
            self.assertFalse(re.search(r'[\u4e00-\u9fff]', en), en)
            self.assertEqual(spec.findall(zh), spec.findall(en), zh)
        missing = []
        for path in (ROOT / "gui/src").glob("*.c"):
            if path.name == "y720-i18n.c":
                continue
            for token in TOKEN.finditer(path.read_text()):
                literal = token.group("string")
                if literal is None:
                    continue
                value = ast.literal_eval(literal)
                if re.search(r'[\u4e00-\u9fff]', value) and value != "中文" and value not in catalog:
                    missing.append(f"{path.name}: {value}")
        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main()
