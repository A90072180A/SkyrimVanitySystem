"""Synthetic official-manifest/load-order regression tests, no game assets."""
import json
import os
from pathlib import Path
import unittest
from unittest import mock
import test_offline as fixtures
from offline.formats import FormatError, active_plugins, load_records
from offline.engine import Scanner, apply_report

plugin = fixtures.plugin

class LoadOrderTests(unittest.TestCase):
    setUp = fixtures.OfflineTests.setUp
    tearDown = fixtures.OfflineTests.tearDown
    write = fixtures.OfflineTests.write
    fixture = fixtures.OfflineTests.fixture

    def official_fixture(self):
        self.fixture()
        names=['ccBGSSSE001-Fish.esm','ccQDRSSE001-SurvivalMode.esl',
               'ccBGSSSE037-Curios.esl','ccBGSSSE025-AdvDSGS.esm','_ResourcePack.esl']
        for name in names:self.write(name,plugin(['Skyrim.esm'],[],1))
        self.write('USSEP.esp',plugin(['Skyrim.esm',*names],[],1))
        # Typical force-loaded entries: present, but omitted from the * list.
        with (self.profile/'plugins.txt').open('a') as f:
            f.write('*USSEP.esp\n'+''.join(n+'\n' for n in names))
        with (self.profile/'loadorder.txt').open('a') as f:
            f.write('USSEP.esp\n'+''.join(n+'\n' for n in reversed(names)))
        (self.data.parent/'Skyrim.ccc').write_text('\n'.join(names)+'\n')
        return names

    def test_ccc_primary_entries_precede_ussep_without_profile_edits(self):
        names=self.official_fixture()
        paths=[self.profile/'plugins.txt',self.profile/'loadorder.txt',self.data.parent/'Skyrim.ccc']
        before={p:p.read_bytes() for p in paths}
        s=Scanner(self.data,self.profile,self.root/'out');s.scan()
        self.assertEqual(s.plugins[:6],['Skyrim.esm',*names])
        self.assertEqual(s.plugins.count('USSEP.esp'),1)
        self.assertNotIn('Inactive.esp',s.plugins)
        receipt=json.loads((self.root/'out/offline-loadorder.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['includedCCC'],names)
        self.assertEqual(receipt['state'],'masters-validated')
        self.assertEqual(receipt['effectiveOrder'],s.plugins)
        self.assertEqual(before,{p:p.read_bytes() for p in paths})

    def test_ccc_deduplicates_against_base_and_starred_lists(self):
        names=self.official_fixture()
        (self.data.parent/'Skyrim.ccc').write_text('Skyrim.esm\n'+
            '\n'.join(names+[names[0].lower()])+'\n')
        with (self.profile/'plugins.txt').open('a') as f:f.write('*'+names[0]+'\n')
        order=active_plugins(self.data,self.profile)
        self.assertEqual(order[:6],['Skyrim.esm',*names])
        self.assertEqual(len(order),len(set(n.casefold() for n in order)))
        load_records(self.data,order)

    def test_ccc_bom_comments_and_no_starred_ordinary_plugins(self):
        self.write('Skyrim.esm',plugin([],[],1))
        self.write('Fishing.esm',plugin(['Skyrim.esm'],[],1))
        (self.profile/'plugins.txt').write_text('# forced-only profile\n')
        (self.data.parent/'Skyrim.ccc').write_bytes(b'\xef\xbb\xbf# comment\r\n\r\nFishing.esm\r\n')
        s=Scanner(self.data,self.profile,self.root/'out')
        self.assertEqual(s.plugins,['Skyrim.esm','Fishing.esm'])

    def test_ccc_missing_installed_entry_is_not_invented(self):
        names=self.official_fixture();(self.data/names[0]).unlink()
        with self.assertRaisesRegex(FormatError,'master-not-visible'):
            Scanner(self.data,self.profile,self.root/'out')
        receipt=json.loads((self.root/'out/offline-loadorder.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['state'],'failed')
        self.assertIn(names[0],[r['name'] for r in receipt['absentImplicit']])
        self.assertFalse((self.root/'out/offline-candidates.json').exists())

    def test_ccc_not_present_does_not_enable_all_cc_files(self):
        self.official_fixture();(self.data.parent/'Skyrim.ccc').unlink()
        with self.assertRaisesRegex(FormatError,'master-not-active'):
            Scanner(self.data,self.profile,self.root/'out')
        receipt=json.loads((self.root/'out/offline-loadorder.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['cccState'],'absent')
        self.assertEqual(receipt['includedCCC'],[])

    def test_ccc_empty_is_not_replaced_by_a_hardcoded_extra_list(self):
        self.official_fixture();(self.data.parent/'Skyrim.ccc').write_text('')
        with self.assertRaisesRegex(FormatError,'master-not-active'):
            Scanner(self.data,self.profile,self.root/'out')
        receipt=json.loads((self.root/'out/offline-loadorder.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['cccState'],'present')
        self.assertEqual(receipt['cccEntries'],[])

    def test_ordinary_late_master_remains_an_error(self):
        self.fixture();self.write('High.esp',plugin(['Skyrim.esm','Flat.esp'],[]))
        (self.profile/'loadorder.txt').write_text('Skyrim.esm\nHigh.esp\nFlat.esp\nSock.esp\n')
        before=(self.profile/'loadorder.txt').read_bytes()
        with self.assertRaisesRegex(FormatError,'late-active-master'):
            Scanner(self.data,self.profile,self.root/'out')
        self.assertEqual(before,(self.profile/'loadorder.txt').read_bytes())

    def test_available_but_disabled_master_is_not_activated(self):
        self.fixture();self.write('High.esp',plugin(['Skyrim.esm','Inactive.esp'],[]))
        self.write('Inactive.esp',plugin(['Skyrim.esm'],[],1))
        with self.assertRaisesRegex(FormatError,'master-not-active'):
            Scanner(self.data,self.profile,self.root/'out')

    def test_ccc_edit_invalidates_batch_apply(self):
        self.official_fixture();s=Scanner(self.data,self.profile,self.root/'out')
        s.scan();s.recommend('Flat.esp|00000801')
        ccc=self.data.parent/'Skyrim.ccc';payload=ccc.read_bytes();stamp=ccc.stat()
        ccc.write_bytes(payload.replace(b'Fish',b'FISH'));os.utime(ccc,ns=(stamp.st_atime_ns,stamp.st_mtime_ns))
        with self.assertRaisesRegex(ValueError,'input changed'):
            apply_report(s.output/'offline-candidates.json',self.root/'dest',[0])
        self.assertFalse((self.root/'dest/VanityUBEHeelAdapter.user.json').exists())

    def test_ccc_creation_invalidates_report_that_observed_absence(self):
        self.fixture();s=Scanner(self.data,self.profile,self.root/'out');s.scan();s.recommend('Flat.esp|00000801')
        (self.data.parent/'Skyrim.ccc').write_text('')
        with self.assertRaisesRegex(ValueError,'input appeared'):
            apply_report(s.output/'offline-candidates.json',self.root/'dest',[0])

    def test_previously_absent_manifest_plugin_appearance_invalidates(self):
        self.fixture();(self.data.parent/'Skyrim.ccc').write_text('OfficialExtra.esl\n')
        s=Scanner(self.data,self.profile,self.root/'out');s.scan();s.recommend('Flat.esp|00000801')
        self.write('OfficialExtra.esl',plugin(['Skyrim.esm'],[],1))
        with self.assertRaisesRegex(ValueError,'input appeared'):
            apply_report(s.output/'offline-candidates.json',self.root/'dest',[0])

    def test_ccc_read_is_open_first_under_path_stat_false_negative(self):
        self.official_fixture()
        with mock.patch.object(Path,'stat',side_effect=FileNotFoundError('synthetic VFS stat miss')):
            s=Scanner(self.data,self.profile,self.root/'out');s.scan();s.recommend('Flat.esp|00000801')
            receipt=apply_report(s.output/'offline-candidates.json',self.root/'dest',[0])
            self.assertEqual(receipt['appliedIndexes'],[0])

    def test_ccc_access_denied_is_not_treated_as_absent(self):
        self.fixture();real_open=Path.open
        def denied(path,*a,**kw):
            if path.name=='Skyrim.ccc':raise PermissionError('CCC read denied')
            return real_open(path,*a,**kw)
        with mock.patch.object(Path,'open',denied),self.assertRaises(PermissionError):
            Scanner(self.data,self.profile,self.root/'out')

    def test_ccc_unsafe_path_and_bad_encoding_rejected(self):
        self.fixture()
        for name in ('../Outside.esm','C:\\Outside.esm','\\Outside.esm','bad:stream.esm','wrong.txt'):
            with self.subTest(name=name):
                (self.data.parent/'Skyrim.ccc').write_text(name)
                with self.assertRaisesRegex(FormatError,'unsafe-plugin-name'):
                    Scanner(self.data,self.profile,self.root/'out')
        (self.data.parent/'Skyrim.ccc').write_bytes(b'\xff\xff')
        with self.assertRaisesRegex(FormatError,'invalid-list-encoding'):
            Scanner(self.data,self.profile,self.root/'out')


if __name__=='__main__':unittest.main(verbosity=2)
