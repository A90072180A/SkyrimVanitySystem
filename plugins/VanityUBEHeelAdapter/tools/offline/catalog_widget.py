"""Tk catalog browser: independent filters, paged rendering and explicit scopes."""
from __future__ import annotations
import json
import tkinter as tk
from tkinter import ttk, messagebox
from .catalog_query import CatalogFilter, CatalogSelection, eligible, fields, origin

KINDS = {'全部类型':'all','丝袜（有滑块）':'stocking','鞋（全部脚姿）':'footwear',
         '鞋：高跟脚姿':'raised-foot-pose','鞋：平脚脚姿':'flat-like-foot-pose',
         '待核查部件':'stocking-candidate','未知／诊断':'unknown'}
STATUSES = {'可测（measurable）':'measurable','全部状态':'all','不可测':'not-measurable',
            '资源读取错误':'source-error','隔离记录／依赖':'quarantine'}
COLUMNS = ('name','kind','status','plugin','provider','model')
HEADINGS = ('名称 / EDID','类型 / 脚姿','状态','来源插件（ARMO / ARMA）','模型提供模组','模型路径')


class CatalogPanel(ttk.Frame):
    PAGE_SIZE = 300

    def __init__(self, master, *, role=None, on_change=lambda:None):
        super().__init__(master)
        self.role=role; self.on_change=on_change
        self.model=CatalogSelection(); self.by_key={}; self.page=0
        self.sort_key='name';self.reverse=False;self.pending=None;self.rendering=False
        self.vars={k:tk.StringVar(value='') for k in ('kind','status','name','plugin','model','provider','search')}
        self.vars['kind'].set('丝袜（有滑块）' if role=='stocking' else '鞋（全部脚姿）' if role=='footwear' else '全部类型')
        self.vars['status'].set('可测（measurable）')
        filt=ttk.Frame(self,padding=6);filt.pack(fill='x')
        for col,(key,title) in enumerate((('kind','类型'),('status','状态'),('name','名称 / EDID 关键词'))):
            ttk.Label(filt,text=title).grid(row=0,column=col*2,sticky='w',padx=3)
            if key in ('kind','status'):
                choices = (list(KINDS) if role is None else ['全部类型','丝袜（有滑块）','待核查部件'] if role=='stocking' else ['全部类型','鞋（全部脚姿）','鞋：高跟脚姿','鞋：平脚脚姿']) if key=='kind' else list(STATUSES)
                widget=ttk.Combobox(filt,textvariable=self.vars[key],values=choices,state='readonly',width=23)
                if key=='status':self.status_box=widget
            else:widget=ttk.Entry(filt,textvariable=self.vars[key])
            widget.grid(row=0,column=col*2+1,sticky='ew',padx=3,pady=3)
        for row,items in enumerate(((('plugin','插件关键词'),('model','模型路径包含'),('provider','模型模组／物理路径')),
                                     (('search','所有字段搜索'),)),1):
            for col,(key,title) in enumerate(items):
                ttk.Label(filt,text=title).grid(row=row,column=col*2,sticky='w',padx=3)
                ttk.Entry(filt,textvariable=self.vars[key]).grid(row=row,column=col*2+1,sticky='ew',padx=3,pady=3,
                    columnspan=5 if key=='search' else 1)
        for col in (1,3,5):filt.columnconfigure(col,weight=1)
        ttk.Label(self,text='字段之间同时满足；同一框内空格=同时包含，| 或逗号=任一词。例如 heel | 高跟 | 丝袜 | cosplay。!UBE 中的 ! 是普通字符。',padding=(8,0),wraplength=1220).pack(fill='x')
        bar=ttk.Frame(self,padding=6);bar.pack(fill='x')
        ttk.Button(bar,text='仅 !UBE 路径',command=lambda:self.vars['model'].set('!UBE')).pack(side='left')
        ttk.Button(bar,text='清除文字筛选',command=self.clear_text).pack(side='left',padx=4)
        ttk.Button(bar,text='选择筛选结果中的全部可测项',command=self.select_filtered).pack(side='left',padx=4)
        ttk.Button(bar,text='清空本侧选择',command=self.clear_selection).pack(side='left',padx=4)
        ttk.Button(bar,text='查看所指行详情',command=self.details).pack(side='left',padx=4)
        self.count=tk.StringVar();ttk.Label(self,textvariable=self.count,padding=(8,3)).pack(fill='x')
        holder=ttk.Frame(self);holder.pack(fill='both',expand=True)
        self.tree=ttk.Treeview(holder,columns=COLUMNS,show='headings',selectmode='extended')
        for key,title,width in zip(COLUMNS,HEADINGS,(230,140,180,300,210,480)):
            self.tree.heading(key,text=title,command=lambda k=key:self.sort(k))
            self.tree.column(key,width=width,minwidth=90,stretch=False)
        vs=ttk.Scrollbar(holder,orient='vertical',command=self.tree.yview)
        hs=ttk.Scrollbar(holder,orient='horizontal',command=self.tree.xview)
        self.tree.configure(yscrollcommand=vs.set,xscrollcommand=hs.set)
        self.tree.grid(row=0,column=0,sticky='nsew');vs.grid(row=0,column=1,sticky='ns');hs.grid(row=1,column=0,sticky='ew')
        holder.rowconfigure(0,weight=1);holder.columnconfigure(0,weight=1)
        self.tree.bind('<<TreeviewSelect>>',self.changed)
        self.tree.bind('<Double-1>',lambda e:self.details())
        self.tree.bind('<Control-a>',self.select_page)
        pages=ttk.Frame(self,padding=5);pages.pack(fill='x')
        ttk.Button(pages,text='上一页',command=lambda:self.turn(-1)).pack(side='left')
        self.page_text=tk.StringVar();ttk.Label(pages,textvariable=self.page_text,padding=(10,0)).pack(side='left')
        ttk.Button(pages,text='下一页',command=lambda:self.turn(1)).pack(side='left')
        ttk.Label(pages,text='筛选隐藏的旧选择会移除；翻页保留选择。Ctrl+A 只选当前页；上方按钮可选全部筛选结果。').pack(side='left',padx=12)
        for var in self.vars.values():var.trace_add('write',self.schedule)
        self.refresh()

    def spec(self):
        v={k:x.get() for k,x in self.vars.items()}
        v['kind']=KINDS.get(v['kind'],v['kind']);v['status']=STATUSES.get(v['status'],v['status'])
        return CatalogFilter(**v)

    def set_rows(self, rows):
        rows=[r for r in rows if self.role is None or
              (r.get('kind') in ('stocking','stocking-candidate') if self.role=='stocking' else r.get('kind')=='footwear')]
        self.model=CatalogSelection(rows);self.by_key={r['key']:r for r in rows}
        if len(self.by_key)!=len(rows):raise ValueError('duplicate catalog keys')
        statuses=sorted({r.get('status','') for r in rows if not r.get('status','').startswith('source-error:')})
        self.status_box['values']=list(STATUSES)+statuses
        self.page=0;self.refresh()

    def schedule(self,*unused):
        if self.pending:self.after_cancel(self.pending)
        self.pending=self.after(180,self.refresh)

    def refresh(self):
        if self.pending:self.after_cancel(self.pending);self.pending=None
        self.model.filter(self.spec())
        self.page=0;self._sort();self.render();self.on_change()

    def _sort(self):
        def value(row):
            if self.sort_key=='plugin':return fields(row)['plugin'].casefold()
            if self.sort_key=='provider':return fields(row)['provider'].casefold()
            return str(row.get(self.sort_key,'')).casefold()
        self.model.visible.sort(key=value,reverse=self.reverse)

    def sort(self,key):
        self.reverse=not self.reverse if key==self.sort_key else False
        self.sort_key=key;self._sort();self.render()

    def render(self):
        self.rendering=True
        old=self.tree.get_children()
        if old:self.tree.delete(*old)
        start=self.page*self.PAGE_SIZE
        for r in self.model.visible[start:start+self.PAGE_SIZE]:
            pose=r.get('footPose',{}).get('kind','')
            wanted=pose if pose in ('raised-foot-pose','flat-like-foot-pose') else r.get('kind')
            kind=next((k for k,v in KINDS.items() if v==wanted),'未知')
            plugin=origin(r.get('armor'))+' / '+origin(r.get('addon'))
            mods=' / '.join(r.get('modelProviderMods',[])) or '未知（未确认物理来源）'
            if r.get('modelProviderState')=='partial':mods+='（部分未知）'
            self.tree.insert('','end',iid=r['key'],values=(r.get('name',''),kind,r.get('status',''),plugin,mods,r.get('model','')))
        self.tree.selection_set([k for k in self.tree.get_children() if k in self.model.selected])
        # Discard queued selection events caused by rebuilding the page.
        self.after_idle(self._render_done)
        self.update_counts()

    def _render_done(self):self.rendering=False

    def changed(self,event=None):
        if self.rendering:return
        self.model.update_page(self.tree.get_children(),self.tree.selection())
        if self.role:
            self.model.selected={k for k in self.model.selected if eligible(self.by_key[k],self.role)}
        self.update_counts();self.on_change()

    def update_counts(self):
        rows=self.model.visible
        possible=sum(eligible(r,self.role) for r in rows)
        self.count.set(f'筛选结果 {len(rows):,} / 本页类别总数 {len(self.model.rows):,}；其中可测 {possible:,}；已选可测 {len(self.model.selected):,}。')
        pages=max(1,(len(rows)+self.PAGE_SIZE-1)//self.PAGE_SIZE)
        self.page_text.set(f'{self.page+1} / {pages} 页，每页最多 {self.PAGE_SIZE} 行')

    def selected_keys(self):
        # Flush pending filter edits before freezing a compute scope.
        if self.pending:self.refresh()
        return self.model.selected_keys()

    def select_filtered(self):
        if self.pending:self.refresh()
        self.model.select_filtered()
        if self.role:self.model.selected={k for k in self.model.selected if eligible(self.by_key[k],self.role)}
        self.render();self.on_change()

    def clear_selection(self):self.model.selected.clear();self.render();self.on_change()

    def clear_text(self):
        for k in ('name','plugin','model','provider','search'):self.vars[k].set('')

    def select_page(self,event=None):
        self.tree.selection_set(self.tree.get_children());self.changed();return 'break'

    def turn(self,delta):
        self.page=max(0,min(self.page+delta,max(0,(len(self.model.visible)-1)//self.PAGE_SIZE)))
        self.render()

    def active_row(self):
        return self.by_key.get(self.tree.focus())

    def details(self):
        row=self.active_row()
        if not row:return
        win=tk.Toplevel(self);win.title('部件详情 — 插件来源不等于模型文件提供者');win.geometry('880x570')
        txt=tk.Text(win,wrap='word');txt.pack(fill='both',expand=True)
        txt.insert('1.0',json.dumps(row,ensure_ascii=False,indent=2));txt.configure(state='disabled')
        ttk.Button(win,text='复制记录与路径',command=lambda:(win.clipboard_clear(),win.clipboard_append(json.dumps(row,ensure_ascii=False,indent=2)))).pack(pady=5)
