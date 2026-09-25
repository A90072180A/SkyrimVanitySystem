"""Synthetic parser/quarantine regressions; no user load orders or game assets."""
import json
from pathlib import Path
import unittest
from unittest import mock
import test_offline as f
from offline.formats import read_plugin, load_records, FormatError
from offline.plugin_records import PluginRecordError, FormReferenceError
from offline.engine import Scanner, apply_report, atomic_json

class PluginRecordsTests(unittest.TestCase):
    setUp=f.OfflineTests.setUp
    tearDown=f.OfflineTests.tearDown
    write=f.OfflineTests.write
    fixture=f.OfflineTests.fixture

    def enable(self, name, data):
        self.write(name, data)
        with (self.profile/'plugins.txt').open('a', encoding='utf-8') as stream:
            stream.write('*'+name+'\n')
        with (self.profile/'loadorder.txt').open('a', encoding='utf-8') as stream:
            stream.write(name+'\n')

    def bad_armor(self, raw=0x02000800, flags=0):
        return f.plugin(['Skyrim.esm'],[f.armor(0x01000801,raw,'Broken Stocking')],flags)

    def scan(self):
        s=Scanner(self.data,self.profile,self.root/'out');s.scan();return s

    def test_strict_reference_error_has_exact_record_context(self):
        self.fixture();p=self.write('Broken.esp',self.bad_armor())
        with self.assertRaises(FormReferenceError) as caught:read_plugin(p,{})
        d=caught.exception.details
        self.assertEqual(d['plugin'],'Broken.esp');self.assertEqual(d['field'],'MODL')
        self.assertEqual(d['recordID'],'Broken.esp|00000801')
        self.assertEqual(d['rawFormID'],'0x02000800');self.assertEqual(d['masterIndex'],2)
        self.assertEqual(d['originCount'],2);self.assertEqual(d['masterCount'],1)
        self.assertEqual(d['rawBytes'],'00080002');self.assertEqual(d['editorID'],'Broken Stocking')
        self.assertGreater(d['recordOffset'],24);self.assertEqual(d['formVersion'],44)

    def test_raw_fe_and_ff_are_not_runtime_remapped_or_clamped(self):
        self.fixture()
        for raw in (0xFE001801,0xFF000001,0xFFFFFFFF,0x80000001):
            with self.subTest(raw=raw):
                p=self.write('Bad.esp',self.bad_armor(raw));report={}
                _,rows=read_plugin(p,{},quarantine_references=True,diagnostics=report)
                self.assertTrue(rows[0]['quarantined']);self.assertEqual(rows[0]['addons'],[])
                self.assertEqual(report['issues'][0]['rawFormID'],f'0x{raw:08X}')

    def test_null_link_and_normal_esl_local_formids(self):
        self.fixture();p=self.write('Light.esp',f.plugin(['Skyrim.esm'],[
            f.armor(0x01000801,0,'Null'),f.armor(0x01000802,0x01000800,'Valid')],0x200))
        _,rows=read_plugin(p,{})
        self.assertEqual(rows[0]['addons'],[])
        self.assertEqual(rows[1]['addons'],['Light.esp|00000800'])

    def test_quarantined_armor_does_not_stop_unrelated_pairing(self):
        self.fixture();self.enable('Broken.esp',self.bad_armor());s=self.scan()
        self.assertEqual(len(s.plugins),5);self.assertEqual(s.record_diagnostics['issueCount'],1)
        q=next(r for r in s.rows if r['armor']=='Broken.esp|00000801')
        self.assertEqual(q['status'],'quarantined-winning-record');self.assertNotIn(q['key'],s.private)
        r=s.recommend('Flat.esp|00000801');self.assertEqual(len(r['entries']),2)
        self.assertFalse(any('Broken.esp' in x['stocking']+x['footwear'] for x in r['entries']))
        receipt=apply_report(s.output/'offline-candidates.json',self.root/'dest',[0,1])
        self.assertEqual(len(receipt['appliedIndexes']),2)

    def test_invalid_override_blocks_previous_valid_armor(self):
        self.fixture();self.enable('Patch.esp',f.plugin(['Skyrim.esm','High.esp'],[
            f.armor(0x01000801,0x03000800,'Bad Override')]))
        s=self.scan();self.assertEqual(s.records['high.esp|00000801']['owner'],'Patch.esp')
        self.assertTrue(s.records['high.esp|00000801']['quarantined'])
        r=s.recommend('Flat.esp|00000801');self.assertEqual(len(r['entries']),1)
        self.assertEqual(r['entries'][0]['footwear'],'Flat.esp|00000801')

    def test_later_valid_override_recovers_without_using_earlier_bad_row(self):
        self.fixture();self.enable('Patch.esp',f.plugin(['Skyrim.esm','High.esp'],[
            f.armor(0x01000801,0x03000800,'Bad Override')]))
        self.enable('Later.esp',f.plugin(['Skyrim.esm','High.esp'],[
            f.armor(0x01000801,0x01000800,'Fixed')]))
        s=self.scan();self.assertFalse(s.records['high.esp|00000801']['quarantined'])
        self.assertFalse(s.record_diagnostics['issues'][0]['isWinningIssue'])
        self.assertEqual(len(s.recommend('Flat.esp|00000801')['entries']),2)

    def test_bad_addon_blocks_whole_armor_even_with_a_good_alternative(self):
        self.fixture()
        bad=f.addon(0x01000800,'!UBE/test/high_1.nif')
        # Append invalid ARMA additional race reference inside the framed record.
        payload=bad[24:]+f.subrec('MODL',f.pack('I',0x03000800))
        multi=f.armor(0x01000801,0x01000800,'Mixed')
        self.enable('Patch.esp',f.plugin(['Skyrim.esm','High.esp'],[
            f.record('ARMA',payload,0x01000800),
            f.record('ARMO',multi[24:]+f.subrec('MODL',f.pack('I',0x02000800)),0x01000801),
            f.addon(0x02000800,'!UBE/test/high_1.nif')]))
        s=self.scan();self.assertEqual(s.records['high.esp|00000801']['blockedBy'],['High.esp|00000800'])
        self.assertEqual(len(s.recommend('Flat.esp|00000801')['entries']),1)

    def test_template_quarantine_propagates_transitively(self):
        self.fixture()
        def templ(identity, target):
            a=f.armor(identity,0x01000800,'Inherited')
            return f.record('ARMO',a[24:]+f.subrec('TNAM',f.pack('I',target)),identity)
        self.enable('Patch.esp',f.plugin(['Skyrim.esm','High.esp'],[
            templ(0x02000803,0x02000802),templ(0x02000802,0x02000801),
            f.armor(0x02000801,0x03000800,'Broken')]))
        s=self.scan()
        for suffix in ('00000801','00000802','00000803'):
            self.assertIn('Patch.esp|'+suffix,s.record_diagnostics['blockedRecordIDs'])

    def test_invalid_header_identity_is_fatal_and_reported(self):
        self.fixture();self.enable('BadHeader.esp',f.plugin(['Skyrim.esm'],[
            f.armor(0xFE000800,0x01000800,'Bad Header')]))
        with self.assertRaises(FormReferenceError):self.scan()
        r=json.loads((self.root/'out/offline-records.json').read_text(encoding='utf-8'))
        self.assertEqual(r['state'],'failed');self.assertEqual(r['fatalError']['field'],'record-header')
        self.assertEqual(r['currentPlugin'],'BadHeader.esp')
        self.assertFalse((self.root/'out/offline-candidates.json').exists())

    def test_deleted_override_is_tombstone_without_parsing_stale_links(self):
        self.fixture();a=f.armor(0x01000801,0xFF123456,'Deleted')
        self.enable('Patch.esp',f.plugin(['Skyrim.esm','High.esp'],[f.record('ARMO',a[24:],0x01000801,0x20)]))
        s=self.scan();self.assertTrue(s.records['high.esp|00000801']['deleted'])
        self.assertEqual(s.record_diagnostics['issueCount'],0)
        self.assertEqual(len(s.recommend('Flat.esp|00000801')['entries']),1)

    def test_malformed_framing_not_recovered_as_record_quarantine(self):
        self.fixture();self.enable('Corrupt.esp',f.plugin(['Skyrim.esm'],[
            f.record('ARMO',b'MODL\x10\x00\x00',0x01000801)]))
        with self.assertRaisesRegex(PluginRecordError,'truncated-record'):self.scan()
        r=json.loads((self.root/'out/offline-records.json').read_text(encoding='utf-8'))
        self.assertEqual(r['fatalError']['plugin'],'Corrupt.esp')

    def test_invalid_reference_byte_count_not_guessed(self):
        self.fixture();self.enable('NewLayout.esp',f.plugin(['Skyrim.esm'],[
            f.record('ARMO',f.subrec('MODL',b'model.nif\0'),0x01000801)]))
        with self.assertRaisesRegex(PluginRecordError,'invalid-form-reference'):self.scan()
        r=json.loads((self.root/'out/offline-records.json').read_text(encoding='utf-8'))
        self.assertEqual(r['fatalError']['field'],'MODL')

    def test_missing_master_remains_fatal_even_after_quarantine(self):
        self.fixture();self.enable('Patch.esp',f.plugin(['Missing.esm'],[
            f.armor(0x01000801,0xFF000001,'Bad')]))
        with self.assertRaisesRegex(FormatError,'master-not-visible'):self.scan()

    def test_compressed_records_preserve_context_and_continue(self):
        self.fixture();self.enable('Compressed.esp',f.plugin(['Skyrim.esm'],[
            f.armor(0x01000801,0x02000800,'Compressed',compress=True)]))
        s=self.scan();d=s.record_diagnostics['issues'][0]
        self.assertTrue(d['compressed']);self.assertEqual(d['field'],'MODL')
        self.assertEqual(d['recordID'],'Compressed.esp|00000801')

    def test_same_plugin_good_record_survives_and_bad_link_array_is_not_partially_used(self):
        self.fixture();a=f.armor(0x01000802,0x01000800,'Partially Bad')
        self.enable('Mixed.esp',f.plugin(['Skyrim.esm'],[
            f.armor(0x01000801,0x01000800,'Good'),f.addon(0x01000800,'!UBE/test/high_1.nif'),
            f.record('ARMO',a[24:]+f.subrec('MODL',f.pack('I',0x02000800)),0x01000802)]))
        s=self.scan();self.assertTrue(s.records['mixed.esp|00000802']['quarantined'])
        self.assertEqual(s.record_diagnostics['issues'][0]['occurrence'],1)
        self.assertTrue(any(r['armor']=='Mixed.esp|00000801' and r['status']=='measurable' for r in s.rows))

    def test_vfs_stat_failure_and_manual_values_preserved(self):
        self.fixture();self.enable('Broken.esp',self.bad_armor())
        dest=self.root/'dest';atomic_json(dest/'VanityUBEHeelAdapter.user.json',{'pairs':[{
            'stocking':'Sock.esp|00000801','footwear':'High.esp|00000801','NoHeel':0,'Heel':1.1}]})
        before=(dest/'VanityUBEHeelAdapter.user.json').read_bytes()
        with mock.patch.object(Path,'stat',side_effect=FileNotFoundError('synthetic VFS')):
            s=self.scan();s.recommend('Flat.esp|00000801')
            receipt=apply_report(s.output/'offline-candidates.json',dest,[1])
        self.assertEqual(receipt['preservedExistingIndexes'],[1])
        self.assertEqual((dest/'VanityUBEHeelAdapter.user.json').read_bytes(),before)

    def test_deleted_later_row_does_not_leave_quarantined_winner(self):
        self.fixture();self.enable('Patch.esp',f.plugin(['Skyrim.esm','High.esp'],[
            f.armor(0x01000801,0x03000800,'Bad')]))
        self.enable('Later.esp',f.plugin(['Skyrim.esm','High.esp'],[
            f.record('ARMO',b'',0x01000801,0x20)]))
        s=self.scan();self.assertEqual(s.record_diagnostics['quarantinedWinnerCount'],0)
        self.assertFalse(s.record_diagnostics['issues'][0]['isWinningIssue'])

    def test_apply_guard_rejects_manually_relabeled_blocked_candidate(self):
        self.fixture();self.enable('Broken.esp',self.bad_armor());s=self.scan()
        r=s.recommend('Flat.esp|00000801')
        r['entries'][0].update(stocking='Broken.esp|00000801',status='manual-adjusted')
        atomic_json(s.output/'offline-candidates.json',r)
        with self.assertRaisesRegex(ValueError,'quarantined pair'):
            apply_report(s.output/'offline-candidates.json',self.root/'dest',[0])
        self.assertFalse((self.root/'dest/VanityUBEHeelAdapter.user.json').exists())

    def test_changed_bad_source_invalidates_old_recommendations(self):
        self.fixture();self.enable('Broken.esp',self.bad_armor());s=self.scan();s.recommend('Flat.esp|00000801')
        self.write('Broken.esp',f.plugin(['Skyrim.esm'],[]))
        with self.assertRaisesRegex(ValueError,'input changed'):
            apply_report(s.output/'offline-candidates.json',self.root/'dest',[0])

    def test_invalid_header_and_type_conflicts_never_merged(self):
        self.fixture();self.enable('Patch.esp',f.plugin(['Skyrim.esm','High.esp'],[
            f.addon(0x01000801,'!UBE/test/high_1.nif')]))
        with self.assertRaisesRegex(PluginRecordError,'form-type-conflict'):self.scan()

if __name__=='__main__':unittest.main(verbosity=2)
