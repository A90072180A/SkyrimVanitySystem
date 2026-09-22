import importlib.util
import json
import tempfile
import unittest
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
if __name__=='__main__':unittest.main()
