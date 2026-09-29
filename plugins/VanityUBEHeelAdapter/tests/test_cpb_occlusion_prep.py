import importlib.util,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parents[1]/"tools"))
spec=importlib.util.spec_from_file_location("prep",Path(__file__).parents[1]/"tools/prepare_cpb_occlusion_patch.py");m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class T(unittest.TestCase):
 def test_expected_constants_are_specific(self):
  self.assertEqual(len(m.EXPECTED),3);self.assertEqual(m.FOOT,"VHA_CPB_CoveredFoot");self.assertTrue(m.OUT_TRI.endswith("CPB_SB_split.tri"))
if __name__=="__main__":unittest.main()
