#!/usr/bin/env python3
"""Browse an existing VHA >=0.18 height library without Skyrim or a new scan.

Run through MO2 for optional source model collection. Read-only GUI by default.
Example: python vha_library.py --library "D:/MO2/overwrite/SKSE/Plugins/VanityUBEHeelAdapter/height-library" gui
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import queue
import threading
import sys
from offline.library_browser import LibraryBrowser, VERSION
from offline.bulk_library import identity


def library_location(plugins=None, user_file=None):
    if user_file:return Path(user_file).parent/'VanityUBEHeelAdapter/height-library'
    if plugins:
        from height_editor import Editor
        return Editor(Path(plugins)).path.parent/'VanityUBEHeelAdapter/height-library'
    return None


def gui(args, *, parent=None, rows=(), selected=(), run_loop=True):
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    win = tk.Toplevel(parent) if parent else tk.Tk()
    win.title('VHA 已保存高度库 / '+VERSION);win.geometry('1370x880');win.minsize(1000,680)
    state={'library':None, 'report':None, 'busy':False, 'closed':False, 'names':{}, 'visibleMembers':[]}
    for row in rows:
        try:state['names'][identity(row['armor'],row['addon'],row['model'])]=row.get('name','')
        except (KeyError,ValueError,TypeError):pass
    location=getattr(args,'library',None)
    if not location:
        try:location=library_location(getattr(args,'plugins',None),getattr(args,'user_file',None))
        except (OSError,ValueError):location=None
    root_path=tk.StringVar(value=str(location or ''));query=tk.StringVar(value=getattr(args,'shoe','') or '')
    member_query=tk.StringVar();message=tk.StringVar(value='选择已发布的 height-library 目录；不需要重新扫描。')
    detail=tk.StringVar(value='显示的是库内存储值，不是当前游戏最终值；手工配对／忽略项可能优先覆盖。')
    q=queue.Queue();buttons=[]
    top=ttk.Frame(win,padding=8);top.pack(fill='x');top.columnconfigure(1,weight=1)
    ttk.Label(top,text='高度库目录').grid(row=0,column=0)
    ttk.Entry(top,textvariable=root_path).grid(row=0,column=1,sticky='ew',padx=5)
    def pick():
        path=filedialog.askdirectory(parent=win,title='选择含 index.vhi 的 height-library 目录')
        if path:root_path.set(path);load()
    ttk.Button(top,text='选择目录',command=pick).grid(row=0,column=2)
    ttk.Label(top,text='查鞋（名称／插件／ID／模型）').grid(row=1,column=0)
    ttk.Entry(top,textvariable=query).grid(row=1,column=1,sticky='ew',padx=5,pady=4)
    ttk.Label(win,text='只读取当前索引指向的鞋文件；旧 .vhs、旧回执不会混入。近似值的当前误差未重新计算。',padding=6).pack(fill='x')
    def table(host, columns, widths, height):
        frame=ttk.Frame(host);frame.pack(fill='both',expand=True)
        tree=ttk.Treeview(frame,columns=columns,show='headings',selectmode='extended',height=height)
        for c,w in zip(columns,widths):tree.heading(c,text=c);tree.column(c,width=w,minwidth=65,stretch=False)
        y=ttk.Scrollbar(frame,orient='vertical',command=tree.yview)
        x=ttk.Scrollbar(frame,orient='horizontal',command=tree.xview)
        tree.configure(yscrollcommand=y.set,xscrollcommand=x.set)
        tree.grid(row=0,column=0,sticky='nsew');y.grid(row=0,column=1,sticky='ns');x.grid(row=1,column=0,sticky='ew')
        frame.columnconfigure(0,weight=1);frame.rowconfigure(0,weight=1)
        return tree
    shoes=table(win,('名称（可空）','鞋子 ARMO','部件 ARMA','实际模型','成员数'),(180,270,290,440,80),6)
    controls=ttk.Frame(win,padding=6);controls.pack(fill='x')
    group_host=ttk.LabelFrame(win,text='此鞋已存控制值组（组编号仅在这个鞋文件中有效）',padding=5);group_host.pack(fill='both',expand=True)
    groups=table(group_host,('组','NoHeel','Heel','成员数','警告成员','近似成员'),(65,105,105,105,105,105),4)
    mbar=ttk.Frame(win,padding=6);mbar.pack(fill='x')
    ttk.Label(mbar,text='成员筛选（名称／ID／模型）').pack(side='left')
    ttk.Entry(mbar,textvariable=member_query,width=62).pack(side='left',padx=5)
    member_host=ttk.Frame(win);member_host.pack(fill='both',expand=True)
    members=table(member_host,('成员','名称（可空）','丝袜 ARMO','NoHeel','Heel','原 NoHeel','原 Heel','原残差','标记','模型'),(65,180,300,85,85,90,90,95,230,400),8)
    bottom=ttk.Frame(win,padding=8);bottom.pack(fill='x')
    ttk.Label(win,textvariable=detail,wraplength=1300,padding=5).pack(fill='x')
    ttk.Label(win,textvariable=message,wraplength=1300,padding=5).pack(fill='x')
    def clear(tree):
        ids=tree.get_children()
        if ids:tree.delete(*ids)
    def job(fn, done):
        if state['busy'] or state['closed']:return
        state['busy']=True
        for b in buttons:b.configure(state='disabled')
        def work():
            try:q.put((done,fn(),None))
            except Exception as exc:q.put((None,None,exc))
        threading.Thread(target=work,daemon=True).start()
    def poll():
        if state['closed']:return
        try:
            done,result,error=q.get_nowait();state['busy']=False
            for b in buttons:b.configure(state='normal')
            if error:message.set(str(error));messagebox.showerror('读取／导出未完成',str(error),parent=win)
            else:
                try:done(result)
                except Exception as exc:message.set(str(exc));messagebox.showerror('界面错误',str(exc),parent=win)
        except queue.Empty:pass
        state['after']=win.after(100,poll)
    def filter_shoes(*unused):
        clear(shoes);state['shoeRows']=[]
        lib=state['library']
        if not lib:return
        state['shoeRows']=lib.shoes(query.get(),state['names'])
        for i,e in enumerate(state['shoeRows']):shoes.insert('','end',iid=str(i),values=(e['name'],*e['shoe'],e['count']))
        for i,e in enumerate(state['shoeRows']):
            if tuple(e['shoe']) in selected:shoes.selection_add(str(i));shoes.see(str(i))
    def load():
        if state['busy']:return
        if not root_path.get().strip():messagebox.showinfo('选择目录','选择含 index.vhi 的目录。',parent=win);return
        path=Path(root_path.get())
        state['report']=None;state['library']=None;clear(groups);clear(members);clear(shoes)
        detail.set('正在读取索引；不会读取所有鞋文件。')
        def done(lib):
            state['library']=lib;root_path.set(str(lib.root));filter_shoes()
            detail.set(f'代际：{lib.index["generation"]}；{len(lib.entries)} 双鞋，{sum(e["count"] for e in lib.entries.values()):,} 个成员。')
            message.set('选中一双鞋，点击“读取所选鞋”（或双击）；其他鞋文件不会读取。')
        job(lambda:LibraryBrowser(path),done)
    def read_selected():
        if state['busy']:return
        lib=state['library'];ids=shoes.selection()
        if not lib or len(ids)!=1:messagebox.showinfo('选一双鞋','请选择一个准确的鞋部件／模型。',parent=win);return
        key=state['shoeRows'][int(ids[0])]['shoe']
        state['report']=None;clear(groups);clear(members)
        job(lambda:lib.read_shoe(key),show_shoe)
    def show_shoe(report):
        state['report']=report;clear(groups)
        by_index={m['memberIndex']:m for m in report['members']}
        for g in report['groups']:
            ms=[by_index[i] for i in g['memberIndexes']]
            groups.insert('','end',iid=str(g['groupIndex']),values=(g['groupIndex'],f'{g["NoHeel"]:.8g}',f'{g["Heel"]:.8g}',len(ms),sum(m['residualWarning'] for m in ms),sum(m['sharedApproximation'] for m in ms)))
        show_members()
        detail.set(f'{report["shoe"][0]} | 源体重 {report["weight"]:g} | {len(report["groups"])} 个已用值组，{len(report["members"])} 个成员。文件：{report["shard"]}')
        message.set('读取成功。没有运行几何拟合或修改配置；双击成员可看 ARMA、来源和完整精度。')
    def show_members(*unused):
        from offline.catalog_query import text_match
        clear(members);r=state['report']
        if not r:return
        chosen={int(i) for i in groups.selection()}
        visible=[m for m in r['members'] if (not chosen or m['group'] in chosen) and text_match('\n'.join((*m['stocking'],state['names'].get(tuple(m['stocking']),''))),member_query.get())]
        state['visibleMembers']=visible
        # Do not render 20,000 Tk items in one event; make cap explicit.
        for m in visible[:2000]:
            flags=', '.join(s for s,on in [('超误差警告',m['residualWarning']),('近似共享',m['sharedApproximation']),('手工候选',m['manualValue'])] if on) or '原值'
            members.insert('','end',iid=str(m['memberIndex']),values=(m['memberIndex'],state['names'].get(tuple(m['stocking']),''),m['stocking'][0],f'{m["NoHeel"]:.8g}',f'{m["Heel"]:.8g}',f'{m["originalNoHeel"]:.8g}',f'{m["originalHeel"]:.8g}',f'{m["residual"]:.6g}',flags,m['stocking'][2]))
        if len(visible)>2000:message.set(f'匹配 {len(visible)} 个成员，界面显示前 2,000 个；请筛选，JSON 导出仍含全部成员。')
    def all_members():groups.selection_remove(groups.selection());show_members()
    def view_member(*unused):
        r=state['report'];ids=members.selection()
        if not r or len(ids)!=1:return
        m=r['members'][int(ids[0])]
        view=tk.Toplevel(win);view.title('存储值与来源（不是游戏实时状态）');view.geometry('1000x580')
        text=tk.Text(view,wrap='word');text.pack(fill='both',expand=True)
        text.insert('1.0',json.dumps(dict(m,sources=[r['assets'][i] for i in m['sources']],shoe=r['shoe'],generation=r['generation']),ensure_ascii=False,indent=2));text.configure(state='disabled')
    def names():
        name=filedialog.askopenfilename(parent=win,title='可选：读取 offline-catalog.json 名称（不是重新扫描）',filetypes=[('JSON','*.json')])
        if not name:return
        from offline.engine import read_json
        def done(r):
            if r.get('schema')!=1 or not isinstance(r.get('entries'),list):raise ValueError('不是装备名称清单')
            for row in r['entries']:
                try:state['names'][identity(row['armor'],row['addon'],row['model'])]=str(row.get('name',''))
                except (KeyError,TypeError,ValueError):pass
            filter_shoes();show_members();message.set('名称已用于显示／检索；旧清单不会覆盖高度库身份或数值。')
        job(lambda:read_json(Path(name)),done)
    def export():
        r=state['report'];lib=state['library']
        if not r:return
        p=filedialog.asksaveasfilename(parent=win,title='另存这双鞋的库内值（不修改库）',initialfile='library-shoe.json',defaultextension='.json')
        if p:job(lambda:lib.export_report(r['shoe'],Path(p)),lambda result:message.set('已导出此鞋全部成员：'+p))
    def bundle():
        r=state['report'];lib=state['library'];ids=[int(i) for i in members.selection()]
        if not r or not 1<=len(ids)<=16:messagebox.showinfo('选配对','请选择 1–16 个具体丝袜成员；只收集所选配对的模型。',parent=win);return
        if not messagebox.askyesno('只为诊断复制原始模型',
            '这会在本地创建包含所选鞋袜 NIF/TRI 的 ZIP，供私人穿模诊断；不会自动上传，不修改游戏文件。\n不含贴图、BodySlide 源工程，也不是安装补丁。继续？',parent=win):return
        d=getattr(args,'data',None)
        if not d:d=filedialog.askdirectory(parent=win,title='选择游戏 Data 目录（请通过 MO2 运行工具）')
        if not d:return
        p=filedialog.asksaveasfilename(parent=win,title='保存私人诊断包（库目录之外）',initialfile='VHA-clipping-models.zip',defaultextension='.zip')
        if p:job(lambda:lib.bundle_models(r['shoe'],ids,Path(d),Path(p)),lambda m:message.set(f'已核验并打包 {len(m["assets"])} 个模型资源：{p}。尚未修改贴合或可见性。'))
    def button(host,label,fn):
        b=ttk.Button(host,text=label,command=fn);b.pack(side='left',padx=3);buttons.append(b)
    button(controls,'刷新高度库索引',load);button(controls,'读取所选鞋',read_selected);button(controls,'载入名称清单（可选）',names)
    button(mbar,'显示全部组成员',all_members)
    button(bottom,'查看所选成员详情',view_member);button(bottom,'导出这双鞋的 JSON',export);button(bottom,'打包所选配对模型（仅本地诊断）',bundle)
    groups.bind('<<TreeviewSelect>>',show_members);members.bind('<Double-1>',view_member);shoes.bind('<Double-1>',lambda e:read_selected())
    query.trace_add('write',filter_shoes);member_query.trace_add('write',show_members)
    def close():
        state['closed']=True
        if state.get('after'):win.after_cancel(state['after'])
        win.destroy()
    win.protocol('WM_DELETE_WINDOW',close);state['after']=win.after(100,poll)
    win.vha={'state':state,'load':load,'read_selected':read_selected,'show_shoe':show_shoe,'show_members':show_members,
             'root_path':root_path,'query':query,'member_query':member_query,'shoes':shoes,'groups':groups,'members':members,
             'all_members':all_members,'export':export,'bundle':bundle,'message':message,'close':close}
    if location:load()
    if run_loop:win.mainloop()
    return win


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library',type=Path);p.add_argument('--plugins',type=Path);p.add_argument('--user-file',type=Path)
    p.add_argument('--data',type=Path);p.add_argument('--shoe',default='',help='Literal shoe name/ID/model filter')
    p.add_argument('command',nargs='?',choices=['gui','list'],default='gui');a=p.parse_args()
    if a.command=='gui':gui(a);return
    location=a.library or library_location(a.plugins,a.user_file)
    if not location:p.error('list requires --library or --plugins')
    lib=LibraryBrowser(location)
    print(json.dumps(dict(generation=lib.index['generation'],shoes=lib.shoes(a.shoe)),ensure_ascii=False,indent=2))

if __name__=='__main__':
    try:main()
    except Exception as exc:print('ERROR: '+str(exc),file=sys.stderr);sys.exit(1)
