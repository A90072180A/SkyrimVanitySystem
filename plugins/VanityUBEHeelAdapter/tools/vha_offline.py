#!/usr/bin/env python3
"""Run through MO2's Python executable, without starting Skyrim.

GUI: python vha_offline.py --data "D:/.../Skyrim Special Edition/Data"
                         --profile "D:/MO2/profiles/<your profile>" gui
CLI: append scan --recommend, or apply --report ... --indexes 0,2
Python 3.10+, standard library only; tkinter is needed for GUI, not for CLI.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import queue
import sys
import threading
import traceback
from offline.engine import Scanner, apply_report, atomic_json, read_json, validate_user, DEFAULT_ANCHOR, VERSION
from offline.catalog_query import pair_plan, select_rows
from vha_fileio import absolute_path


def gui(args, *, run_loop=True):
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    from offline.catalog_widget import CatalogPanel
    root=tk.Tk();root.title('VHA 离线高度扫描 / '+VERSION);root.geometry('1380x920');root.minsize(1120,820)
    data=tk.StringVar(value=str(args.data or ''))
    profile=tk.StringVar(value=str(args.profile or ''))
    mods_root=tk.StringVar(value=str(getattr(args,'mods_root',None) or ''))
    weight=tk.StringVar(value=str(args.weight));anchor=tk.StringVar(value=args.anchor)
    msg=tk.StringVar(value='在 MO2 中启动本程序。先扫描当前启用插件，再选择丝袜、计算高度建议。')
    allow=tk.BooleanVar(value=False)
    top=ttk.Frame(root,padding=10);top.pack(fill='x')
    for row,(label,var) in enumerate((('游戏 Data 目录',data),('MO2 当前 profile 目录',profile),('MO2 mods 根目录（可空，用于来源识别）',mods_root))):
        ttk.Label(top,text=label).grid(row=row,column=0,sticky='w')
        ttk.Entry(top,textvariable=var).grid(row=row,column=1,sticky='ew',padx=8,pady=3)
        ttk.Button(top,text='选择目录',command=lambda v=var:v.set(filedialog.askdirectory() or v.get())).grid(row=row,column=2)
    top.columnconfigure(1,weight=1)
    options=ttk.Frame(root,padding=(10,0));options.pack(fill='x')
    ttk.Label(options,text='源模型体重 0–100：').pack(side='left')
    ttk.Entry(options,textvariable=weight,width=6).pack(side='left')
    ttk.Label(options,text='   平脚参考（需本人确认）：').pack(side='left')
    anchorbox=ttk.Combobox(options,textvariable=anchor,width=70);anchorbox.pack(side='left',fill='x',expand=True)
    ttk.Label(root,text='只计算高度。默认使用已生成 NIF 的基准，不读取当前 OBody/动画；缺失或不支持的模型会列出原因。',padding=10).pack(fill='x')
    notebook=ttk.Notebook(root);notebook.pack(fill='both',expand=True,padx=10)
    sock_frame=ttk.Frame(notebook);shoe_frame=ttk.Frame(notebook)
    catalog_frame=ttk.Frame(notebook);candidate_frame=ttk.Frame(notebook)
    notebook.add(sock_frame,text='1. 筛选／选择丝袜');notebook.add(shoe_frame,text='2. 筛选／选择鞋子')
    notebook.add(catalog_frame,text='全部部件／诊断');notebook.add(candidate_frame,text='3. 配对与滑块建议')
    def table(frame,columns,widths):
        tree=ttk.Treeview(frame,columns=columns,show='headings',selectmode='extended')
        for c,w in zip(columns,widths):tree.heading(c,text=c);tree.column(c,width=w,minwidth=70,stretch=True)
        scroll=ttk.Scrollbar(frame,orient='vertical',command=tree.yview);tree.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right',fill='y');tree.pack(fill='both',expand=True);return tree
    sock_panel=CatalogPanel(sock_frame,role='stocking');sock_panel.pack(fill='both',expand=True)
    shoe_panel=CatalogPanel(shoe_frame,role='footwear');shoe_panel.pack(fill='both',expand=True)
    catalog=CatalogPanel(catalog_frame);catalog.pack(fill='both',expand=True)
    panels=(sock_panel,shoe_panel,catalog)
    candidates=table(candidate_frame,('丝袜','鞋子','NoHeel','Heel','原计算残差','状态'),(230,230,75,75,85,220))
    actions=ttk.Frame(root,padding=10);actions.pack(fill='x')
    q=queue.Queue();cancel=threading.Event();state={'scanner':None,'report':None,'busy':False}
    buttons=[];compute_button=None
    scope_text=tk.StringVar(value='本次计算：0 条丝袜 × 0 双鞋 = 0 对 / 上限 20,000。请分别选择；不会默认全选。')
    ttk.Label(root,textvariable=scope_text,padding=(10,3),wraplength=1300).pack(fill='x')
    def update_scope():
        rows=state['scanner'].rows if state['scanner'] else sock_panel.model.rows+shoe_panel.model.rows
        try:
            plan=pair_plan(rows,sock_panel.selected_keys(),shoe_panel.selected_keys())
            text=f'本次计算：{len(plan["stockings"])} 条丝袜 × {len(plan["shoes"])} 双鞋 = {plan["count"]:,} 对 / 上限 {plan["budget"]:,}。'
            if plan['reason']:text+=' '+plan['reason']
            if not state['scanner']:text+=' 当前无本次扫描实例；载入旧清单只供筛选预览。'
            scope_text.set(text)
            if compute_button:compute_button.configure(state='normal' if plan['allowed'] and state['scanner'] and not state['busy'] else 'disabled')
        except ValueError as exc:scope_text.set(str(exc))
    for panel in panels:panel.on_change=update_scope
    def populate(rows):
        for panel in panels:panel.set_rows(rows)
        anchorbox['values']=[r['key'] for r in rows if r['kind']=='footwear' and r['status']=='measurable']
        update_scope()
    def clear_candidates():
        state['report']=None
        children=candidates.get_children()
        if children:candidates.delete(*children)
    def browse_catalog():
        name=filedialog.askopenfilename(title='载入已保存清单（仅浏览筛选；计算需要重新扫描）',filetypes=[('JSON','*.json')])
        if not name:return
        def done(r):
            if r.get('schema')!=1 or not isinstance(r.get('entries'),list):raise ValueError('invalid catalog')
            state['scanner']=None;clear_candidates();populate(r['entries'])
            msg.set(f'已载入 {len(r["entries"]):,} 行供筛选预览。此模式不计算、不写入高度配置；计算前请运行扫描。')
        job(lambda:read_json(Path(name)),done)
    def save_filters():
        name=filedialog.asksaveasfilename(title='保存界面筛选（不是高度配置）',initialfile='VHA.filters.json',defaultextension='.json')
        if name:
            try:atomic_json(Path(name),{'schema':1,'filters':[{k:v.get() for k,v in panel.vars.items()} for panel in panels]})
            except (OSError,ValueError) as e:messagebox.showerror('筛选未保存',str(e))
    def load_filters():
        name=filedialog.askopenfilename(title='读取 VHA.filters.json',filetypes=[('JSON','*.json')])
        if not name:return
        try:
            r=read_json(Path(name));specs=r.get('filters')
            if r.get('schema')!=1 or not isinstance(specs,list) or len(specs)!=3:raise ValueError('不是界面筛选文件')
            if any(not isinstance(d,dict) or set(d)!=set(p.vars) or any(not isinstance(v,str) or len(v)>2048 for v in d.values()) for p,d in zip(panels,specs)):raise ValueError('无效筛选条件')
            for p,d in zip(panels,specs):
                for k,v in d.items():p.vars[k].set(v)
                p.refresh()
        except (OSError,ValueError) as e:messagebox.showerror('筛选未加载',str(e))
    def progress(text):
        if cancel.is_set():raise RuntimeError('用户取消；未应用任何配置')
        q.put(('progress',text))
    def job(fn,done):
        if state['busy']:return
        state['busy']=True;cancel.clear()
        for b in buttons:b.configure(state='disabled')
        def worker():
            try:q.put(('done',(done,fn())))
            except Exception as e:q.put(('error',(str(e),traceback.format_exc())))
        threading.Thread(target=worker,daemon=True).start()
    def scan():
        try:
            if not data.get() or not profile.get():raise ValueError('请选择 Data 和当前 profile；不能从全部 mods 文件夹猜启用列表。')
            d,p,w=Path(data.get()),Path(profile.get()),float(weight.get())
            output=args.output or d/'SKSE/Plugins/VanityUBEHeelAdapter/offline'
        except ValueError as e:messagebox.showerror('设置错误',str(e));return
        selected_mods=Path(mods_root.get()) if mods_root.get().strip() else None
        state['scanner']=None;state['report']=None
        populate([]);clear_candidates()
        def run():
            s=Scanner(d,p,output,w,args.preset,args.preset_name,args.archive_list,args.race,progress,mods_root=selected_mods)
            report=s.scan();return s,report
        def done(result):
            s,r=result;state['scanner']=s;state['report']=None
            populate(s.rows);clear_candidates()
            blocked=s.record_diagnostics.get('quarantinedWinnerCount',0)
            issues=s.record_diagnostics.get('issueCount',0)
            prefix='扫描完成（存在隔离记录）' if blocked else '扫描完成'
            msg.set(f'{prefix}：{len(s.plugins)} 个插件，{len(s.rows)} 个候选部件；'
                    f'{issues} 条引用诊断，{blocked} 条异常最终记录已隔离。详情：offline-records.json。'
                    '请在前两个标签页分别筛选、选择丝袜和鞋；隔离项不会产生配对。')
        job(run,done)
    def recommend():
        s=state['scanner']
        if not s:messagebox.showinfo('先扫描','请先运行扫描。');return
        keys=sock_panel.selected_keys();shoe_keys=shoe_panel.selected_keys()
        try:
            if (absolute_path(Path(data.get()))!=s.data or absolute_path(Path(profile.get()))!=s.profile
                    or float(weight.get())!=s.weight):raise ValueError('Data/profile/体重已改变，请重新扫描后计算。')
            plan=pair_plan(s.rows,keys,shoe_keys)
            if not plan['allowed']:raise ValueError(plan['reason'])
        except ValueError as e:messagebox.showinfo('请调整计算范围',str(e));return
        if plan['count']>1000 and not messagebox.askyesno('较大的计算任务',f'本次 {plan["count"]:,} 对，可能耗时较长。继续？'):return
        selected_anchor=anchor.get()
        clear_candidates()
        def done(r):
            state['report']=r;candidates.delete(*candidates.get_children())
            for row in r['entries']:
                fmt=lambda v:'' if v is None else f'{v:.5f}'
                candidates.insert('','end',iid=str(row['index']),values=(row['stockingName'],row['footwearName'],fmt(row['NoHeel']),fmt(row['Heel']),fmt(row.get('normalizedResidual')),row['status']))
            notebook.select(candidate_frame);msg.set(f'计算完成：{len(r["entries"])} 对。数值是离线建议；勾选并应用后作为手工搭配指令保存。')
        job(lambda:s.recommend(selected_anchor,keys,heel_max=args.heel_max,shoe_keys=shoe_keys),done)
    def select_safe():
        if state['report']:
            candidates.selection_set([str(r['index']) for r in state['report']['entries'] if r['status']=='within-mathematical-limits'])
    def apply():
        s=state['scanner'];r=state['report']
        if not s or not r:return
        indexes=[int(x) for x in candidates.selection()]
        if not indexes:messagebox.showinfo('尚未选择','请选择要应用的配对。');return
        if not messagebox.askyesno('写入手工配置',f'把选中的 {len(indexes)} 对建议写入用户配置？\n已有手工值、忽略项会保留。\n这些是源模型基准下的建议，不保证当前 OBody、动画和视觉效果。\n不会改写 NIF/TRI/ESP。'):return
        review=allow.get();plugins=args.plugins or s.data/'SKSE/Plugins'
        def done(result):
            msg.set(f'已写入 {len(result["appliedIndexes"])} 对；保留 {len(result["preservedExistingIndexes"])} 对已有设置。文件：{result["userFile"]}')
            messagebox.showinfo('已保存',msg.get()+'\n这是文件保存结果，不代表游戏已应用。')
        job(lambda:apply_report(s.output/'offline-candidates.json',plugins,indexes,review,args.user_file),done)
    def adjust():
        r=state['report'];selection=candidates.selection()
        if not r or len(selection)!=1:messagebox.showinfo('选择一对','先选择一条有数值的配对。');return
        row=r['entries'][int(selection[0])]
        if row.get('NoHeel') is None or row.get('Heel') is None:
            messagebox.showinfo('没有可靠来源','这个条目尚未完成源模型测量，不能伪造为扫描建议。');return
        window=tk.Toplevel(root);window.title('手工调整离线建议');window.geometry('450x245');window.transient(root);window.grab_set()
        n=tk.StringVar(value=str(row['NoHeel']));h=tk.StringVar(value=str(row['Heel']))
        for label,var in (('NoHeel（0–1）',n),('Heel（0–设定上限）',h)):
            f=ttk.Frame(window,padding=6);f.pack(fill='x');ttk.Label(f,text=label,width=24).pack(side='left');ttk.Entry(f,textvariable=var).pack(side='left')
        ttk.Label(window,text='保存修改的是建议报告。之后仍需点击“一键应用所选”。\n已有用户手工配置会保留，不会被扫描值覆盖。',padding=10).pack()
        def save_adjustment():
            try:
                import height_editor
                nn,hh=float(n.get()),float(h.get());height_editor.controls(nn,hh,args.heel_max)
                row.setdefault('originalSuggestion',{'NoHeel':row['NoHeel'],'Heel':row['Heel'],'status':row['status']})
                row.update(NoHeel=nn,Heel=hh,status='manual-adjusted',residualAtEditedValue=None)
                atomic_json(state['scanner'].output/'offline-candidates.json',r)
                candidates.item(selection[0],values=(row['stockingName'],row['footwearName'],f'{nn:.5f}',f'{hh:.5f}',f'{row.get("normalizedResidual",0):.5f}','manual-adjusted'))
                window.destroy()
            except Exception as ex:messagebox.showerror('未保存',str(ex),parent=window)
        ttk.Button(window,text='保存手工建议',command=save_adjustment).pack(pady=8)
    def mark():
        s=state['scanner']
        current=notebook.select()
        panel=sock_panel if current==str(sock_frame) else shoe_panel if current==str(shoe_frame) else catalog if current==str(catalog_frame) else None
        row=panel.active_row() if panel else None
        if not s or not row:messagebox.showinfo('选择一件','本次扫描后，在清单中指向一个部件。');return
        if not row.get('armor'):return
        window=tk.Toplevel(root);window.title('标记装备');window.geometry('490x205');window.transient(root);window.grab_set()
        kind=tk.StringVar(value='stocking' if row['kind']=='stocking' else 'footwear')
        ttk.Label(window,text=row['name']+'\n'+row['armor'],padding=10,wraplength=470).pack(fill='x')
        ttk.Combobox(window,textvariable=kind,values=('stocking','footwear','ignore','auto'),state='readonly').pack()
        ttk.Label(window,text='只写用户分类，不改 ESP，也不会制造不存在的滑块。',padding=10).pack()
        def save_mark():
            try:
                import height_editor
                e=height_editor.Editor(args.plugins or s.data/'SKSE/Plugins',args.user_file)
                e.mark(row['armor'],kind.get(),'Offline inventory: explicit user classification',row.get('addon') if kind.get()=='stocking' else None)
                validate_user(e.user,e.maximum);e.save();msg.set('已保存分类：'+str(e.path));window.destroy()
            except Exception as ex:messagebox.showerror('未保存',str(ex),parent=window)
        ttk.Button(window,text='保存分类',command=save_mark).pack()
    edit_actions=ttk.Frame(root,padding=(10,0));edit_actions.pack(fill='x')
    for label,fn in (('手工调整一条建议',adjust),('手动标记所指装备',mark),('载入旧清单（仅筛选预览）',browse_catalog),('保存筛选',save_filters),('读取筛选',load_filters)):
        b=ttk.Button(edit_actions,text=label,command=fn);b.pack(side='left',padx=3);buttons.append(b)
    for label,fn in (('1. 扫描启用装备',scan),('2. 计算高度配对',recommend),('选择误差门槛内建议',select_safe),('3. 一键应用所选',apply)):
        b=ttk.Button(actions,text=label,command=fn);b.pack(side='left',padx=3);buttons.append(b)
        if fn==recommend:compute_button=b
    ttk.Checkbutton(actions,text='允许本人复核过的超误差建议',variable=allow).pack(side='left',padx=8)
    ttk.Button(actions,text='取消任务',command=cancel.set).pack(side='right')
    ttk.Label(root,textvariable=msg,wraplength=1200,padding=10).pack(fill='x')
    def poll():
        try:
            # Bound draining; frequent worker progress does not starve the Tk loop.
            for _ in range(100):
                kind,value=q.get_nowait()
                if kind=='progress':msg.set(value)
                elif kind in ('done','error'):
                    state['busy']=False
                    for b in buttons:b.configure(state='normal')
                    if kind=='done':
                        try:value[0](value[1])
                        except Exception as exc:
                            msg.set(str(exc));messagebox.showerror('界面更新未完成',str(exc))
                            traceback.print_exc()
                    else:
                        msg.set(value[0]);messagebox.showerror('操作未完成',value[0]);print(value[1],file=sys.stderr)
                    update_scope()
        except queue.Empty:pass
        root.after(100,poll)
    def close():
        cancel.set()
        for identifier in root.tk.call('after','info'):
            root.after_cancel(identifier)
        root.destroy()
    update_scope()
    root.protocol('WM_DELETE_WINDOW',close);root.after(100,poll)
    # Test seam uses the same widgets/actions as the shipped entry point.
    root.vha={'panels':panels,'scan':scan,'recommend':recommend,'apply':apply,
              'browse':browse_catalog,'save_filters':save_filters,'load_filters':load_filters,
              'state':state,'candidates':candidates,'scope':scope_text,'message':msg,
              'compute_button':compute_button,'populate':populate,'close':close}
    if run_loop:root.mainloop()
    return root

def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--data',type=Path);p.add_argument('--profile',type=Path)
    p.add_argument('--plugins',type=Path,help='Actual SKSE/Plugins output parent; default virtual Data/SKSE/Plugins')
    p.add_argument('--mods-root',type=Path,help='MO2 mods directory for opened-handle provenance; never recursively scanned')
    p.add_argument('--output',type=Path);p.add_argument('--user-file',type=Path)
    p.add_argument('--weight',type=float,default=0.);p.add_argument('--heel-max',type=float,default=2.)
    p.add_argument('--anchor',default=DEFAULT_ANCHOR);p.add_argument('--preset',type=Path);p.add_argument('--preset-name')
    p.add_argument('--race',help='Optional exact stable RACE ID; additional-race inheritance is not guessed')
    p.add_argument('--archive-list',type=Path,help='Additional INI BSA filenames, low to high priority, one per line')
    sub=p.add_subparsers(dest='command')
    sub.add_parser('gui')
    scan=sub.add_parser('scan');scan.add_argument('--recommend',action='store_true');scan.add_argument('--stocking',action='append');scan.add_argument('--shoe',action='append');scan.add_argument('--all-measurable',action='store_true',help='Explicitly use all measurable items for any unspecified side')
    apply=sub.add_parser('apply');apply.add_argument('--report',type=Path,required=True);apply.add_argument('--indexes',required=True);apply.add_argument('--allow-reviewed-residual',action='store_true')
    args=p.parse_args()
    if sys.version_info<(3,10):p.error('Python 3.10 or later is required')
    if args.command in (None,'gui'):gui(args);return
    if args.command=='apply':
        plugins=args.plugins or (args.data/'SKSE/Plugins' if args.data else None)
        if plugins is None:p.error('apply requires --plugins or --data')
        result=apply_report(args.report,plugins,[int(x) for x in args.indexes.split(',')],args.allow_reviewed_residual,args.user_file)
    else:
        if not args.data or not args.profile:p.error('scan requires --data and --profile')
        output=args.output or args.data/'SKSE/Plugins/VanityUBEHeelAdapter/offline'
        s=Scanner(args.data,args.profile,output,args.weight,args.preset,args.preset_name,args.archive_list,args.race,lambda t:print(t,flush=True),mods_root=args.mods_root)
        result=s.scan()
        if args.recommend:
            if not args.all_measurable and (not args.stocking or not args.shoe):
                raise ValueError('Specify both --stocking and --shoe, or explicitly use --all-measurable.')
            def resolve_requested(requested,role):
                if requested is None:return None
                pool=select_rows(s.rows,None,role);keys=set()
                for name in requested:
                    matched=[r['key'] for r in pool if name.casefold() in (r['key'].casefold(),r['armor'].casefold())]
                    if not matched:raise ValueError(f'{role} not found/measurable: {name}')
                    keys.update(matched)
                return list(keys)
            result=s.recommend(args.anchor,resolve_requested(args.stocking,'stocking'),args.heel_max,
                shoe_keys=resolve_requested(args.shoe,'footwear'))
    print(json.dumps({k:v for k,v in result.items() if k not in ('entries','inputSources')},ensure_ascii=False,indent=2))

if __name__=='__main__':
    try:main()
    except KeyboardInterrupt:print('Cancelled; no configuration applied.',file=sys.stderr);sys.exit(130)
    except Exception as exc:print(f'ERROR: {exc}',file=sys.stderr);sys.exit(1)
