"""Synthetic packing tests; no proprietary model bytes."""
from pathlib import Path
import io,sys,tempfile,unittest,zipfile
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
import numpy as np
from offline.formats import parse_tri,FormatError
from offline.mesh_partition_prep import duplicate_tri_shape
from test_offline import tri
from prepare_cpb_surface_patch import add_fit,assemble,read_member,MAIN,FOOT,FIT,fnv

class SurfacePreparationTests(unittest.TestCase):
 def setUp(self):
  self.original=tri(MAIN,{'NoHeel':{0:(0,0,1)},'Heel':{23339:(0,0,-1)},'Body':{5:(0,1,0)}})
  self.raw=duplicate_tri_shape(self.original,MAIN,FOOT)
  self.delta=np.zeros((23340,3));self.delta[:3228]=(.1,.05,-.08)
 def test_fit_quantization_and_seam(self):
  raw,report=add_fit(self.raw,self.delta);a,_=parse_tri(raw);before,_=parse_tri(self.raw)
  self.assertEqual(a[MAIN][FIT],a[FOOT][FIT]);self.assertEqual(len(a[MAIN][FIT]),3228)
  self.assertLess(report['maxComponentError'],5e-6)
  for shape in before:
   for name,values in before[shape].items():self.assertEqual(a[shape][name],values)
 def test_repeated_fit_refused(self):
  raw,_=add_fit(self.raw,self.delta)
  with self.assertRaisesRegex(FormatError,'already-present'):add_fit(raw,self.delta)
 def test_bad_domain_values_and_shape_refused(self):
  for bad in (np.zeros((23339,3)),np.full((23340,3),np.nan),np.full((23340,3),1.),np.zeros((23340,3))):
   with self.subTest(shape=bad.shape),self.assertRaises(FormatError):add_fit(self.raw,bad)
 def test_both_siblings_required(self):
  with self.assertRaisesRegex(FormatError,'both-prepared'):add_fit(self.original,self.delta)
 def test_unrelated_shapes_preserved(self):
  raw=duplicate_tri_shape(self.raw,MAIN,'Other');result,_=add_fit(raw,self.delta)
  before,_=parse_tri(raw);after,_=parse_tri(result);self.assertEqual(before['Other'],after['Other'])
 def test_existing_output_never_replaced(self):
  with tempfile.TemporaryDirectory() as d:
   out=Path(d)/'existing';out.mkdir();old=out/'keep.txt';old.write_text('keep')
   with self.assertRaisesRegex(FormatError,'new-directory'):assemble(Path('absent'),Path('absent'),Path('absent'),out)
   self.assertEqual(old.read_text(),'keep')
 def test_archive_member_ambiguity_refused(self):
  buffer=io.BytesIO()
  with zipfile.ZipFile(buffer,'w') as z:z.writestr('a/foo.nif',b'a');z.writestr('b/foo.nif',b'b')
  with zipfile.ZipFile(io.BytesIO(buffer.getvalue())) as z:
   with self.assertRaises(FormatError):read_member(z,'foo.nif')
   with self.assertRaises(FormatError):read_member(z,'missing.nif')
 def test_fingerprint_vector(self):self.assertEqual(fnv(b''),'cbf29ce484222325');self.assertEqual(fnv(b'a'),'af63dc4c8601ec8c')
if __name__=='__main__':unittest.main()
