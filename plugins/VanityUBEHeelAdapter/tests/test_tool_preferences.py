import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
from tool_preferences import Preferences, settings_path

class Tests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);self.path=self.root/'settings.json'
    def tearDown(self):self.temp.cleanup()
    def test_unicode_space_roundtrip_and_explicit_precedence(self):
        values=dict(data='D:/游戏 Data',profile='D:/MO2/profiles/A',mods_root='D:/MO2/mods',output='D:/结果',plugins='D:/写入/SKSE/Plugins',user_file='D:/例外/VanityUBEHeelAdapter.user.json')
        Preferences(self.path).save(values)
        prefs=Preferences(self.path)
        self.assertEqual(prefs.initial(SimpleNamespace()),values)
        self.assertEqual(prefs.initial(SimpleNamespace(output=Path('explicit')))['output'],'explicit')
    def test_profile_isolation(self):
        p=Preferences(self.path);p.save(dict(data='D:/Data',profile='D:/profiles/A',plugins='D:/A/plugins',user_file='D:/A/VanityUBEHeelAdapter.user.json'))
        q=Preferences(self.path)
        self.assertEqual(q.initial(SimpleNamespace(profile=Path('D:/profiles/B')))['plugins'],'')
        q.save(dict(data='D:/Data',profile='D:/profiles/B',plugins='D:/B/plugins'))
        self.assertEqual(Preferences(self.path).for_profile('D:\\profiles\\A')['plugins'],'D:/A/plugins')
    def test_new_game_does_not_reuse_old_write_paths(self):
        p=Preferences(self.path);p.save(dict(data='D:/old/Data',profile='D:/profile',plugins='D:/old/plugins'))
        self.assertEqual(p.initial(SimpleNamespace(data=Path('E:/other/Data')))['plugins'],'')
    def test_corrupt_and_future_settings_not_destroyed(self):
        for raw in (b'{',b'{"schema":2}',b'{"schema":1,"last":[],"profiles":{}}'):
            self.path.write_bytes(raw);p=Preferences(self.path)
            self.assertTrue(p.warning)
            with self.assertRaises(ValueError):p.save({})
            self.assertEqual(self.path.read_bytes(),raw)
            p.save({},repair=True);self.assertFalse(Preferences(self.path).warning)
    def test_failed_replace_preserves_old_file(self):
        p=Preferences(self.path);p.save(dict(data='A'));before=self.path.read_bytes()
        with mock.patch('tool_preferences.os.replace',side_effect=PermissionError('locked')):
            with self.assertRaises(OSError):p.save(dict(data='B'))
        self.assertEqual(self.path.read_bytes(),before);self.assertFalse(list(self.root.glob('*.tmp')))
    def test_other_window_profiles_are_merged(self):
        a=Preferences(self.path);b=Preferences(self.path)
        a.save(dict(profile='A',plugins='a'));b.save(dict(profile='B',plugins='b'))
        self.assertEqual(set(Preferences(self.path).document['profiles']),{'a','b'})
    def test_no_stat_precondition(self):
        p=Preferences(self.path);p.save(dict(data='D:/virtual',profile='P'))
        with mock.patch.object(Path,'stat',side_effect=AssertionError('No stat allowed')):
            self.assertEqual(Preferences(self.path).initial(SimpleNamespace())['data'],'D:/virtual')
    def test_memory_location_override(self):
        with mock.patch.dict(os.environ,{'VHA_TOOL_SETTINGS':str(self.path)}):self.assertEqual(settings_path(),self.path)

if __name__=='__main__':unittest.main()
