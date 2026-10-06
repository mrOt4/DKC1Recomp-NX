import importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('windows_shaders',ROOT/'scripts/generate_windows_shaders.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class WindowsShaderContracts(unittest.TestCase):
    def test_all_mac_passes_are_translated(self):
        shaders=module.translate((ROOT/'runner/macos_graphics.metal').read_text())
        self.assertEqual(len(shaders),8)  # 7 CRT passes + dkc1_hd
        for shader in shaders:
            self.assertNotIn('[[',shader)
            self.assertNotIn('texture2d<',shader)
            self.assertNotIn('.sample(',shader)
            self.assertIn('uniform float u[27]',shader)
            self.assertIn('4.0 * df(F, G)',shader)
    def test_changed_pass_contract_fails_closed(self):
        with self.assertRaises(ValueError):
            module.translate((ROOT/'runner/macos_graphics.metal').read_text().replace('dkc1_compose','unexpected'))
