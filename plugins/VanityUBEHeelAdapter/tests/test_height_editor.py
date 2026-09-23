import importlib.util
import json
import tempfile
import unittest
import time
from pathlib import Path
spec=importlib.util.spec_from_file_location('editor',Path(__file__).parents[1]/'tools/height_editor.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Tests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)
 def tearDown(self):self.temp.cleanup()
 def test_set_reload_and_conflict(self):
  e=m.Editor(self.path);e.set_pair('Sock.esp|00000001','Shoe.esp|00000002',0,1.107);e.save()
  e1=m.Editor(self.path);e2=m.Editor(self.path);self.assertEqual(e1.user['pairs'][0]['Heel'],1.107)
  e1.mark('Sock.esp|00000001','stocking',addon='Sock.esp|00000003');e1.save()
  with self.assertRaises(RuntimeError):e2.save()
  self.assertTrue(e1.path.with_suffix('.json.bak').exists())
 def test_invalid_does_not_write(self):
  e=m.Editor(self.path)
  for a,b in [(1.1,0),(0,2.1),(0,float('nan')),(.1,.1)]:
   with self.assertRaises(ValueError):e.set_pair('Sock.esp|00000001','Shoe.esp|00000002',a,b)
  self.assertFalse(e.path.exists())
 def test_ignore_auto_mark(self):
  e=m.Editor(self.path);e.mark('Sock.esp|00000001','ignore');e.set_pair('Sock.esp|00000001','<barefoot>',0,0,mode='ignore');e.save()
  e=m.Editor(self.path);e.mark('Sock.esp|00000001','auto');e.set_pair('Sock.esp|00000001','<barefoot>',0,0,mode='auto');e.save()
  self.assertEqual(e.user['items'],[]);self.assertEqual(e.user['pairs'],[])
 def test_approval_binding(self):
  e=m.Editor(self.path);e.output.mkdir()
  p={'algorithm':'bounded-native-branches-v2','stocking':{'armor':'Sock.esp|00000001','addon':'Sock.esp|00000003'},'footwear':{'armor':'Shoe.esp|00000002','addon':'Shoe.esp|00000004'},'NoHeel':0,'Heel':1.107}
  p.update({key:'example-binding' for key in m.APPROVAL})
  (e.output/'height-profiles.json').write_text(json.dumps({'entries':[p]}))
  e.approve(0);e.save();self.assertIn('approval',e.user['pairs'][0]);self.assertEqual(e.user['pairs'][0]['approval']['stockingAddon'],'Sock.esp|00000003')
  self.assertEqual(json.loads((e.output/'height-profiles.json').read_text())['entries'][0],p)
 def receipt(self, path=None, token="game-A", **extra):
  output=self.path/'VanityUBEHeelAdapter';output.mkdir(exist_ok=True)
  data={"schema":1,"processToken":token,"heartbeatUnixMs":time.time()*1000,"userPath":str(path or self.path/'VanityUBEHeelAdapter.user.json'),"state":"pending","accepted":{}}
  data.update(extra);(output/'configuration-status.json').write_text(json.dumps(data))
  return data
 def test_resolved_new_overlay_and_real_ack(self):
  actual=self.path/'real-mod'/'VanityUBEHeelAdapter.user.json';self.receipt(actual)
  e=m.Editor(self.path);self.assertEqual(e.path,actual);e.set_pair('Sock.esp|00000001','Glass.esp|00000002',0,1.1);e.save()
  self.assertTrue(actual.exists());self.assertFalse((self.path/'VanityUBEHeelAdapter.user.json').exists())
  self.assertEqual(e.delivery()[0],'pending')
  accepted={"userPath":str(actual),"userFingerprint":m.fingerprint(actual.read_bytes()),"revision":7}
  self.receipt(actual,state="accepted",accepted=accepted);self.assertEqual(e.delivery()[0],'accepted')
  self.receipt(actual,token="game-B",state="accepted",accepted=accepted);self.assertEqual(e.delivery()[0],'accepted-after-restart')
 def test_stale_receipt_wrong_file_rejection(self):
  actual=self.path/'real-mod'/'VanityUBEHeelAdapter.user.json';self.receipt(actual)
  e=m.Editor(self.path,self.path/'wrong'/'VanityUBEHeelAdapter.user.json')
  with self.assertRaises(RuntimeError):e.save()
  self.assertFalse(e.path.exists())
  e=m.Editor(self.path);e.save();self.receipt(actual,error="NoHeel outside range",state="rejected")
  self.assertEqual(e.delivery()[0],'rejected')
  self.receipt(actual,heartbeatUnixMs=time.time()*1000-20000);self.assertEqual(e.delivery()[0],'no-live-receipt')
 def test_fingerprint_and_receipt_never_claims_submission(self):
  self.assertEqual(m.fingerprint(b"hello"),'a430d84680aabd0b')
  e=m.Editor(self.path);e.save();self.assertEqual(e.delivery()[0],'no-live-receipt')
  self.receipt(state="accepted",accepted={"userPath":str(e.path),"userFingerprint":"different","revision":100})
  self.assertEqual(e.delivery()[0],'pending')
 def test_invalid_advertised_path(self):
  self.receipt(self.path/'arbitrary.txt')
  with self.assertRaises(ValueError):m.Editor(self.path)
if __name__=='__main__':unittest.main()
