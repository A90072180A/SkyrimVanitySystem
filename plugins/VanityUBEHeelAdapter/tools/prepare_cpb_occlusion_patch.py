#!/usr/bin/env python3
"""Create a reversible CPB split-foot compatibility patch from the user's own current loose meshes."""
from __future__ import annotations
import argparse,hashlib,json
from pathlib import Path
from offline.formats import parse_nif,parse_tri,FormatError
from offline import mesh_partition_prep as part
REL=Path("Meshes/!UBE/Caenarvon/Cosplay")
EXPECTED={"CPB_SB_0.nif":"c952f853f1b0ee3cf7d0b81e8863c20dee3d9dc902f5a148446ee58f48eea6f0","CPB_SB_1.nif":"40ee7753774279600de4a55f6795deea94934b4f566c325c7f0565939f722bfa","CPB_SB.tri":"a427574ba6bb52ba84649a04433a94e20a3ac370de1c57c72761044071cc4cda"}
OUT_TRI="!UBE\\Caenarvon\\Cosplay\\VHA\\CPB_SB_split.tri";SOURCE="SB_bodystocking";FOOT="VHA_CPB_CoveredFoot"
def sha(b):return hashlib.sha256(b).hexdigest()
def selected_faces(n0,n1,tri,coef=.47,zmax=8.0):
 s0=parse_nif(n0);s1=parse_nif(n1);m,_=parse_tri(tri)
 if len(s0)!=1 or len(s1)!=1 or s0[0]["name"]!=SOURCE or s1[0]["name"]!=SOURCE:raise FormatError("unexpected-CPB-shape")
 if s0[0]["triangles"]!=s1[0]["triangles"]:raise FormatError("weight-topology-mismatch")
 heel=m.get(SOURCE,{}).get("Heel")
 if heel is None:raise FormatError("Heel-morph-missing")
 def z(s,i):return s["points"][i][2]+coef*heel.get(i,(0.,0.,0.))[2]
 return [i for i,t in enumerate(s0[0]["triangles"]) if all(z(s0[0],v)<zmax and z(s1[0],v)<zmax for v in t)]
def prepare(data,out,replace=False):
 src={n:(data/REL/n).read_bytes() for n in EXPECTED};bad={n:sha(src[n]) for n,h in EXPECTED.items() if sha(src[n])!=h}
 if bad:raise RuntimeError("CPB source hashes differ from the validated sample: "+json.dumps(bad,ensure_ascii=False))
 faces=selected_faces(src["CPB_SB_0.nif"],src["CPB_SB_1.nif"],src["CPB_SB.tri"])
 if len(faces)!=5656:raise RuntimeError(f"validated foot face count changed: {len(faces)}")
 low,tr0,r0=part.split(src["CPB_SB_0.nif"],src["CPB_SB.tri"],faces,source=SOURCE,new=FOOT,tri_resource=OUT_TRI)
 high,tr1,r1=part.split(src["CPB_SB_1.nif"],src["CPB_SB.tri"],faces,source=SOURCE,new=FOOT,tri_resource=OUT_TRI)
 if tr0!=tr1:raise RuntimeError("prepared TRI differs between source weights")
 targets={out/REL/"CPB_SB_0.nif":low,out/REL/"CPB_SB_1.nif":high,out/"Meshes/!UBE/Caenarvon/Cosplay/VHA/CPB_SB_split.tri":tr0}
 if not replace and any(p.exists() for p in targets):raise FileExistsError("output already exists; choose a new mod directory or use --replace")
 for p,b in targets.items():p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b)
 report={"schema":1,"source":EXPECTED,"preparedBodyTri":OUT_TRI,"footShape":FOOT,"faceCount":len(faces),"outputs":{str(p.relative_to(out)):sha(b) for p,b in targets.items()},"low":r0,"high":r1}
 (out/"VHA_CPB_OcclusionPatch.json").write_text(json.dumps(report,indent=2,ensure_ascii=False)+"\n",encoding="utf8")
 (out/"README_VHA_CPB_Occlusion.txt").write_text("Place this generated mod after BodySlide Output. Mark only fully opaque closed shoes as opaque-closed. Re-export CPB height-library entries after enabling this patch.\n",encoding="utf8")
 return report
def gui():
 import tkinter as tk
 from tkinter import ttk,filedialog,messagebox
 root=tk.Tk();root.title("VHA CPB 脚部袜面准备工具");root.geometry("900x230");data=tk.StringVar();out=tk.StringVar()
 for label,var in (("游戏 Data 目录",data),("输出 MO2 模组目录",out)):
  row=ttk.Frame(root);row.pack(fill="x",padx=8,pady=8);ttk.Label(row,text=label,width=20).pack(side="left");ttk.Entry(row,textvariable=var).pack(side="left",fill="x",expand=True);ttk.Button(row,text="选择",command=lambda v=var:v.set(filedialog.askdirectory())).pack(side="left")
 def run():
  try:r=prepare(Path(data.get()),Path(out.get()));messagebox.showinfo("完成",f"已生成 {r['faceCount']} 个脚部面。")
  except Exception as e:messagebox.showerror("未生成",str(e))
 ttk.Button(root,text="生成兼容补丁",command=run).pack(pady=12);root.mainloop()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument("mode",choices=["prepare","gui"]);p.add_argument("--data",type=Path);p.add_argument("--output",type=Path);p.add_argument("--replace",action="store_true");a=p.parse_args()
 if a.mode=="gui":gui();return
 if not a.data or not a.output:p.error("prepare requires --data and --output")
 print(json.dumps(prepare(a.data,a.output,a.replace),indent=2,ensure_ascii=False))
if __name__=="__main__":main()
