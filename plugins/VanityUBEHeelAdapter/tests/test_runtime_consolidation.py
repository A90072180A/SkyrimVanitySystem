"""Source-level integration invariants, in addition to the native policy tests."""
from pathlib import Path
import json
import re
import unittest
ROOT=Path(__file__).parents[1]
class Tests(unittest.TestCase):
    def test_controller_has_no_stocking_visibility_writes(self):
        source=(ROOT/'src/main.cpp').read_text(encoding='utf-8')
        for removed in ('SetAppCulled','GetAppCulled','hiddenFeet','HidePreparedFoot','claimedHidden'):
            self.assertNotIn(removed,source)
        self.assertIn('"stockingOcclusion","removed"',source)
        self.assertIn('"agataOcclusionEligible",false',source)
    def test_height_barefoot_and_glass_still_connected(self):
        source=(ROOT/'src/main.cpp').read_text(encoding='utf-8')
        for required in ('plan=UserPair(', 'plan=OfflineLibrary(', 'Plan{{1,0},"confirmed-barefoot-flat"',
                         'prepared_cpb::FitMorph', 'PreparedContext(context,prepared->glassBlockedMorphs)',
                         'RestoreScopedMorphs', '1.1)&&PreparedContext'):
            self.assertIn(required,source)
    def test_defaults_retire_occlusion_without_removing_legacy_parser(self):
        config=json.loads((ROOT/'config/VanityUBEHeelAdapter.json').read_text())
        self.assertFalse(config['enableStockingFootOcclusion']);self.assertTrue(config['enableGlassFootFit'])
        header=(ROOT/'src/ConfigurationCore.h').read_text()
        self.assertIn('"enableStockingFootOcclusion"',header);self.assertIn('"enableGlassFootFit"',header)
    def test_binary_build_version_matches_runtime_header(self):
        build=(ROOT.parent.parent/'xmake.lua').read_text(encoding='utf-8')
        target=re.search(r'target\("VanityUBEHeelAdapter"\)\s+set_version\("([0-9.]+)"\)',build)
        self.assertIsNotNone(target)
        header=(ROOT/'src/Plugin.h').read_text(encoding='utf-8')
        version=re.search(r'VERSION\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}',header)
        self.assertIsNotNone(version)
        self.assertEqual(target.group(1),'.'.join(version.groups()[:3]))
        self.assertEqual(version.group(4),'0')
if __name__=='__main__':unittest.main()
