"""Edit the real manual overlay without scanning or requiring a candidate report."""
from __future__ import annotations
import copy
import json
from pathlib import Path
import height_editor
from offline.engine import validate_user
from offline.foot_reference import TOOL_VERSION


def decode(text: str) -> dict:
    if len(text.encode('utf-8')) > 16 * 1024 * 1024:
        raise ValueError('JSON 文件超过 16 MiB')
    def pairs(rows):
        result = {}
        for key, value in rows:
            if key in result:
                raise ValueError('重复 JSON 键：' + key)
            result[key] = value
        return result
    def reject(value):
        raise ValueError('不允许非有限数值：' + value)
    document = json.loads(text, object_pairs_hook=pairs, parse_constant=reject)
    if not isinstance(document, dict):
        raise ValueError('用户覆盖必须是 JSON 对象')
    return document


class OverlaySession:
    def __init__(self, plugins: Path, user_file: Path | None = None):
        if user_file is not None and user_file.name.casefold() != 'vanityubeheeladapter.user.json':
            raise ValueError('请选择 VanityUBEHeelAdapter.user.json，不是候选报告或主配置。')
        self.editor = height_editor.Editor(plugins, user_file)
        self.document = copy.deepcopy(self.editor.user)
        self.validate(self.document)

    @property
    def path(self):
        return self.editor.path

    def maximum(self, document):
        base = self.editor.base
        runtime = self.editor.runtime
        return document.get('settings', {}).get('heelMax', runtime.get('heelMax', base.get('heelMax', 2.0)))

    def validate(self, document):
        if not isinstance(document, dict) or not isinstance(document.get('settings', {}), dict):
            raise ValueError('无效用户配置')
        validate_user(document, self.maximum(document))
        return document

    def parse(self, text):
        return self.validate(decode(text))

    def text(self):
        return json.dumps(self.document, indent=2, ensure_ascii=False, allow_nan=False) + '\n'

    def put_pair(self, document, stocking, footwear, noheel, heel, mode='manual', note='', index=None):
        result = copy.deepcopy(document)
        self.validate(result)
        rows = result.setdefault('pairs', [])
        if index is not None and not 0 <= index < len(rows):
            raise ValueError('所选配对已改变；请刷新列表')
        old = copy.deepcopy(rows[index]) if index is not None else {}
        if old and (old['stocking'], old['footwear']) != (stocking, footwear) and 'approval' in old:
            raise ValueError('来源绑定配对不能直接换身份。请另建配对，或在 JSON 中明确移除 approval。')
        row = {**old, 'stocking': stocking, 'footwear': footwear, 'mode': mode, 'note': note}
        if mode == 'manual':
            row.update(NoHeel=noheel, Heel=heel)
        else:
            row.pop('NoHeel', None); row.pop('Heel', None)
        if index is None:
            rows.append(row)
        else:
            rows[index] = row
        self.validate(result)
        return result

    def save(self, text):
        document = self.parse(text)
        self.editor.user = copy.deepcopy(document)
        self.editor.save()  # actual destination + concurrency check + backup + atomic replacement
        self.document = document
        return self.path


def gui(plugins: Path, user_file: Path | None = None, *, parent=None, run_loop=True, seed=None):
    import tkinter as tk
    from tkinter import ttk, messagebox, filedialog
    session = OverlaySession(plugins, user_file)
    window = tk.Toplevel(parent) if parent else tk.Tk()
    window.title(f'VHA 工具 {TOOL_VERSION} — 手工覆盖 JSON（无需扫描）')
    window.geometry('1160x760'); window.minsize(900, 620)
    if parent:
        window.transient(parent)
    status = tk.StringVar(master=window, value='尚未保存修改。列表与 JSON 是同一份编辑内容。')
    target = tk.StringVar(master=window)
    ttk.Label(window, textvariable=target, wraplength=1120, padding=8).pack(fill='x')
    ttk.Label(window, text='只修改手工覆盖；不改高度库、候选报告、NIF/TRI 或 OBody。旧 coverage/隐藏开关可保留，但 0.20.0 不执行隐藏。',
              wraplength=1120, padding=(8, 0)).pack(fill='x')
    tabs = ttk.Notebook(window); tabs.pack(fill='both', expand=True, padx=8, pady=6)
    form = ttk.Frame(tabs); raw_frame = ttk.Frame(tabs)
    tabs.add(form, text='已有手工配对／新增／修改'); tabs.add(raw_frame, text='原始 JSON／设置／分类')
    columns = ('stocking', 'footwear', 'mode', 'NoHeel', 'Heel', 'note')
    tree = ttk.Treeview(form, columns=columns, show='headings', selectmode='browse', height=11)
    for key, width in zip(columns, (270, 270, 75, 65, 65, 220)):
        tree.heading(key, text=key); tree.column(key, width=width, minwidth=45)
    scroll = ttk.Scrollbar(form, orient='vertical', command=tree.yview)
    tree.configure(yscrollcommand=scroll.set); scroll.pack(side='right', fill='y'); tree.pack(fill='both', expand=True)
    edit = ttk.Frame(form, padding=6); edit.pack(fill='x')
    fields = {k: tk.StringVar(master=window, value='0' if k in ('NoHeel', 'Heel') else 'manual' if k == 'mode' else '') for k in columns}
    for row, key in enumerate(columns):
        ttk.Label(edit, text=key, width=13).grid(row=row, column=0, sticky='w')
        widget = ttk.Combobox(edit, textvariable=fields[key], values=('manual', 'ignore'), state='readonly') if key == 'mode' else ttk.Entry(edit, textvariable=fields[key])
        widget.grid(row=row, column=1, sticky='ew', pady=2)
    edit.columnconfigure(1, weight=1)
    raw = tk.Text(raw_frame, wrap='none', undo=True)
    sy = ttk.Scrollbar(raw_frame, orient='vertical', command=raw.yview)
    sx = ttk.Scrollbar(raw_frame, orient='horizontal', command=raw.xview)
    raw.configure(yscrollcommand=sy.set, xscrollcommand=sx.set)
    sy.pack(side='right', fill='y'); sx.pack(side='bottom', fill='x'); raw.pack(fill='both', expand=True)
    state = {'session': session, 'selected': None, 'listedText': '', 'baselineText': '', 'poll': None}

    def text():
        return raw.get('1.0', 'end-1c')

    def show_document(document, *, baseline=False):
        serialized = json.dumps(document, ensure_ascii=False, indent=2, allow_nan=False) + '\n'
        raw.delete('1.0', 'end'); raw.insert('1.0', serialized)
        if baseline:
            state['baselineText'] = serialized
        rebuild(document)

    def rebuild(document):
        children = tree.get_children()
        if children:
            tree.delete(*children)
        for index, row in enumerate(document.get('pairs', [])):
            tree.insert('', 'end', iid=str(index), values=tuple(row.get(k, '') for k in columns))
        state['selected'] = None; state['listedText'] = text()
        target.set('实际编辑文件：' + str(state['session'].path))

    def guarded(action):
        try:
            return action()
        except (OSError, ValueError, RuntimeError, TypeError, KeyError) as exc:
            status.set('未保存：' + str(exc)); messagebox.showerror('未保存', str(exc), parent=window)
            return None

    def refresh_table():
        return guarded(lambda: rebuild(state['session'].parse(text())))

    def choose(_=None):
        chosen = tree.selection()
        if not chosen:
            return
        if text() != state['listedText']:
            state['selected'] = None
            status.set('JSON 已改动；请先“校验 JSON／刷新列表”，避免用旧行号修改。')
            return
        index = int(chosen[0]); document = state['session'].parse(text())
        state['selected'] = index
        row = document['pairs'][index]
        for key in columns:
            fields[key].set(str(row.get(key, 'manual' if key == 'mode' else '0' if key in ('NoHeel', 'Heel') else '')))
    tree.bind('<<TreeviewSelect>>', lambda event: guarded(choose))

    def new_pair():
        state['selected'] = None
        tree.selection_remove(*tree.selection())
        for key in columns:
            fields[key].set('manual' if key == 'mode' else '0' if key in ('NoHeel', 'Heel') else '')
        if seed:
            for key in columns:
                if key in seed:fields[key].set(str(seed[key]))
        status.set('未验证手工试调起点，不是 Witchy 自动测量值；尚未写文件，也不会覆盖已有配对。' if seed and 'UNVALIDATED' in seed.get('note','') else '正在新建配对；“应用此条到编辑区”后，再保存文件。')

    def put():
        def run():
            if text() != state['listedText']:
                raise ValueError('JSON 已改变，请先校验并刷新列表。')
            s = state['session']; doc = s.parse(text())
            mode = fields['mode'].get()
            doc = s.put_pair(doc, fields['stocking'].get().strip(), fields['footwear'].get().strip(),
                float(fields['NoHeel'].get()) if mode == 'manual' else 0.,
                float(fields['Heel'].get()) if mode == 'manual' else 0., mode, fields['note'].get(), state['selected'])
            show_document(doc); status.set('已更新编辑区，尚未写入磁盘；点击“保存手工覆盖 JSON”。')
        return guarded(run)

    def delete():
        def run():
            if text() != state['listedText'] or state['selected'] is None:
                raise ValueError('先刷新列表并选择一条配对。')
            if not messagebox.askyesno('删除手工覆盖', '从编辑区删除这一对？保存后恢复库／自动规则的选择。', parent=window):
                return
            doc = state['session'].parse(text()); del doc['pairs'][state['selected']]
            show_document(doc); status.set('已从编辑区移除；尚未保存文件。')
        return guarded(run)

    def save():
        def run():
            s = state['session']; path = s.save(text())
            show_document(s.document, baseline=True)
            status.set(f'已保存：{path}。原文件已备份为 .json.bak；游戏中 set VHA_Reload to 1。')
            return path
        return guarded(run)

    def discard_ok():
        return text() == state['baselineText'] or messagebox.askyesno('放弃未保存修改', '当前编辑区有未保存修改。放弃这些修改？', parent=window)

    def reload_file(path=None):
        def run():
            if not discard_ok():
                return
            replacement = OverlaySession(plugins, Path(path) if path else state['session'].path)
            state['session'] = replacement
            show_document(replacement.document, baseline=True)
            status.set('已重新读取磁盘文件。')
        return guarded(run)

    def browse_file():
        path = filedialog.askopenfilename(parent=window, title='选择现有手工覆盖文件',
            initialdir=str(state['session'].path.parent), initialfile='VanityUBEHeelAdapter.user.json', filetypes=[('JSON', '*.json')])
        if path:
            reload_file(path)

    def poll():
        try:
            e = state['session'].editor
            if e.saved_fingerprint:
                code, message = e.delivery()
                if code in ('accepted', 'accepted-after-restart', 'rejected', 'wrong-file'):
                    status.set(message)
        except (OSError, ValueError, TypeError) as exc:
            status.set('回执不可读：' + str(exc))
        state['poll'] = window.after(700, poll)

    def close():
        if not discard_ok():
            return
        if state['poll']:
            window.after_cancel(state['poll'])
        window.destroy()

    buttons = ttk.Frame(form, padding=6); buttons.pack(fill='x')
    for label, action in (('新建配对', new_pair), ('设为裸脚', lambda: fields['footwear'].set('<barefoot>')),
                          ('应用此条到编辑区', put), ('删除所选覆盖', delete)):
        ttk.Button(buttons, text=label, command=action).pack(side='left', padx=3)
    bottom = ttk.Frame(window, padding=8); bottom.pack(fill='x')
    for label, action in (('校验 JSON／刷新列表', refresh_table), ('保存手工覆盖 JSON', save),
                          ('重新读取磁盘', reload_file), ('选择其他覆盖文件', browse_file)):
        ttk.Button(bottom, text=label, command=action).pack(side='left', padx=3)
    ttk.Label(window, textvariable=status, wraplength=1120, padding=8).pack(fill='x')
    show_document(session.document, baseline=True); poll()
    window.protocol('WM_DELETE_WINDOW', close)
    window.vha = dict(state=state, raw=raw, tree=tree, fields=fields, put=put, save=save, delete=delete,
        reload=reload_file, close=close, refresh=refresh_table, new_pair=new_pair, status=status, tabs=tabs)
    if seed:
        new_pair()
    if run_loop:
        window.mainloop()
    return window
