#!/usr/bin/env python3
"""Local height-profile editor. Only writes the explicit .user.json overlay.
No game assets, generated measurements, OBody keys or main config are modified.
Run `python height_editor.py --plugins <MO2 overwrite/SKSE/Plugins> gui` or --help.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile
import time
from typing import Any

ID = re.compile(r"^.+\.(?:esp|esm|esl)\|[0-9a-f]{8}$", re.I)
APPROVAL = ("context", "targetPositionFingerprint", "donorSourceFingerprint", "bodyTriFingerprint", "referenceConfiguration")

def read_json(path: Path, default: Any = None) -> Any:
    def no_constant(text: str) -> None:
        raise ValueError(f"Non-finite JSON value: {text}")
    # A brief sharing violation during a Windows atomic replace is not an empty
    # JSON document. Reopen briefly; invalid JSON itself is never swallowed.
    for attempt in range(4):
        try:
            with path.open("rb") as stream:
                payload = stream.read(16 * 1024 * 1024 + 1)
            break
        except FileNotFoundError:
            return default
        except OSError:
            if attempt == 3:
                raise
            time.sleep(0.01 * (attempt + 1))
    if len(payload) > 16 * 1024 * 1024:
        raise ValueError(f"File too large: {path}")
    return json.loads(payload.decode("utf-8-sig"), parse_constant=no_constant)


def controls(noheel: float, heel: float, maximum: float) -> None:
    if not all(math.isfinite(v) for v in (noheel, heel, maximum)):
        raise ValueError("Values must be finite")
    if not 1 <= maximum <= 10 or not 0 <= noheel <= 1 or not 0 <= heel <= maximum:
        raise ValueError(f"NoHeel must be 0..1; Heel must be 0..{maximum:g}")
    if noheel > 0 and heel > 0:
        raise ValueError("Only one height direction may be nonzero")


def fingerprint(payload: bytes) -> str:
    # Diagnostic delivery identity, same FNV-1a64 as the plugin, not a security hash.
    h = 14695981039346656037
    for byte in payload:
        h = ((h ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{h:016x}"


def recent(receipt: dict) -> bool:
    try:
        return -2000 <= time.time() * 1000 - float(receipt["heartbeatUnixMs"]) < 10000
    except (KeyError, TypeError, ValueError):
        return False


def same_path(a: str | Path, b: str | Path) -> bool:
    def key(p: str | Path) -> str:
        text = os.path.normcase(str(Path(p).resolve()))
        if os.name == "nt":
            if text.startswith("\\\\?\\unc\\"):
                text = "\\\\" + text[8:]
            elif text.startswith("\\\\?\\"):
                text = text[4:]
        return text
    return key(a) == key(b)


class Editor:
    def __init__(self, plugins: Path, user_file: Path | None = None):
        self.plugins = plugins.resolve()
        self.output = self.plugins / "VanityUBEHeelAdapter"
        self.connection = read_json(self.output / "configuration-status.json", {})
        advertised = self.connection.get("userPath") if self.connection.get("schema") == 1 else None
        if advertised and (not Path(advertised).is_absolute() or Path(advertised).name != "VanityUBEHeelAdapter.user.json"):
            raise ValueError("Invalid advertised user configuration path")
        self.path = Path(user_file or advertised or self.plugins / "VanityUBEHeelAdapter.user.json").resolve()
        self.saved_fingerprint: str | None = None
        self.saved_time_ms = 0.0
        self.saved_process = self.connection.get("processToken") if recent(self.connection) else None
        self.before = self.path.read_bytes() if self.path.exists() else None
        self.user = read_json(self.path, {"schema": 1, "settings": {}, "items": [], "pairs": []})
        if not isinstance(self.user, dict) or self.user.get("schema", 1) != 1:
            raise ValueError("Unsupported user configuration")
        self.runtime = read_json(self.output / "runtime-state.json", {})
        self.base = read_json(self.plugins / "VanityUBEHeelAdapter.json", {})

    @property
    def maximum(self) -> float:
        return float(self.user.get("settings", {}).get("heelMax", self.runtime.get("heelMax", self.base.get("heelMax", 2.0))))

    def candidates(self) -> list[dict]:
        return read_json(self.output / "height-profiles.json", {"entries": []}).get("entries", [])

    def inventory(self) -> list[dict]:
        return list(read_json(self.output / "observed-items.json", {"entries": {}}).get("entries", {}).values())

    def mark(self, armor: str, kind: str, note: str = "", addon: str | None = None) -> None:
        if not ID.fullmatch(armor) or kind not in ("stocking", "footwear", "ignore", "auto"):
            raise ValueError("Invalid stable armor ID or classification")
        rows = [v for v in self.user.get("items", []) if v["armor"] != armor]
        if kind != "auto":
            row = {"armor": armor, "kind": kind, "note": note}
            if addon:
                if not ID.fullmatch(addon):
                    raise ValueError("Invalid addon ID")
                row["addons"] = [addon]
            rows.append(row)
        self.user["items"] = rows

    def set_pair(self, stocking: str, footwear: str, noheel: float, heel: float,
                 note: str = "", mode: str = "manual", approval: dict | None = None) -> None:
        if not ID.fullmatch(stocking) or not (ID.fullmatch(footwear) or footwear == "<barefoot>"):
            raise ValueError("Use plugin.esp|00000001 stable IDs")
        if mode not in ("manual", "ignore", "auto"):
            raise ValueError("Unknown pair mode")
        if mode == "manual":
            controls(noheel, heel, self.maximum)
        rows = [v for v in self.user.get("pairs", [])
                if (v["stocking"], v["footwear"]) != (stocking, footwear)]
        if mode != "auto":
            row = {"stocking": stocking, "footwear": footwear, "mode": mode, "note": note}
            if mode == "manual":
                row.update({"NoHeel": noheel, "Heel": heel})
            if approval:
                row["approval"] = approval
            rows.append(row)
        self.user["pairs"] = rows

    def approve(self, index: int, note: str = "Explicitly reviewed by user", expected: dict | None = None) -> None:
        entries = self.candidates()
        if not 0 <= index < len(entries):
            raise ValueError("Candidate index out of range; refresh candidates")
        p = entries[index]
        if expected is not None and p != expected:
            raise RuntimeError("Candidate file changed; refresh and select again")
        if p.get("algorithm") != "bounded-native-branches-v2":
            raise ValueError("Old candidate algorithm; needs a fresh 0.16 measurement")
        approval = {key: p[key] for key in APPROVAL}
        approval.update({"stockingAddon": p["stocking"]["addon"], "footwearAddon": p["footwear"]["addon"]})
        if not all(isinstance(v, str) and v for v in approval.values()):
            raise ValueError("Incomplete candidate provenance")
        self.set_pair(p["stocking"]["armor"], p["footwear"]["armor"], float(p["NoHeel"]), float(p["Heel"]), note, approval=approval)

    def save(self) -> None:
        # An explicit --user-file must not silently edit a different file from
        # the running plugin. The actual backing path is supplied in its receipt.
        connection = read_json(self.output / "configuration-status.json", {})
        if recent(connection) and connection.get("userPath") and not same_path(connection["userPath"], self.path):
            raise RuntimeError(f"当前游戏监视的是 {connection['userPath']}，不是 {self.path}。请移除 --user-file 或选择游戏实际监视的文件。")
        self.saved_process = connection.get("processToken") if recent(connection) else None
        # A second editor's newer changes are never silently overwritten.
        current = self.path.read_bytes() if self.path.exists() else None
        if current != self.before:
            raise RuntimeError("User file changed in another editor; reload before saving")
        for p in self.user.get("pairs", []):
            if p.get("mode", "manual") == "manual":
                controls(float(p["NoHeel"]), float(p["Heel"]), self.maximum)
        payload = (json.dumps(self.user, indent=2, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if current is not None:
            self.path.with_suffix(self.path.suffix + ".bak").write_bytes(current)
        fd, name = tempfile.mkstemp(prefix=self.path.name + ".", suffix=".tmp", dir=self.path.parent)
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(payload); f.flush(); os.fsync(f.fileno())
            os.replace(name, self.path)
        finally:
            if os.path.exists(name): os.unlink(name)
        self.before = payload
        self.saved_fingerprint = fingerprint(payload)
        self.saved_time_ms = time.time() * 1000

    def delivery(self) -> tuple[str, str]:
        if self.saved_fingerprint is None:
            return "not-saved", "尚未保存本次修改"
        c = read_json(self.output / "configuration-status.json", {})
        if not recent(c):
            return "no-live-receipt", "文件已保存，但没有新鲜的游戏回执；不能确认热重载。"
        if not c.get("userPath") or not same_path(c["userPath"], self.path):
            return "wrong-file", f"游戏监视的文件不同：{c.get('userPath', '未知')}"
        if c.get("heartbeatUnixMs", 0) < self.saved_time_ms:
            return "pending", "文件已保存，等待下一次游戏回执。"
        if c.get("error"):
            return "rejected", f"游戏拒绝修改，仍保留上一份配置：{c['error']}"
        a = c.get("accepted", {})
        if c.get("state") == "accepted" and a.get("userFingerprint") == self.saved_fingerprint and same_path(a.get("userPath", ""), self.path):
            if not self.saved_process or c.get("processToken") != self.saved_process:
                return "accepted-after-restart", f"配置已被当前游戏接受（修订 {a.get('revision')}），但不是同一次运行的热重载证明。"
            return "accepted", f"游戏已热重载本次文件（修订 {a.get('revision')}）；是否正确变形仍以实际画面为准。"
        return "pending", "文件已保存，游戏尚未确认接受本次内容；请保持游戏运行。"


def gui(plugins: Path, user_file: Path | None) -> None:
    import tkinter as tk
    from tkinter import ttk, messagebox
    root = tk.Tk(); root.title("UBE 高度配置编辑器 — 只写手工覆盖"); root.geometry("1150x660")
    def editor() -> Editor:
        return Editor(plugins, user_file)
    last_save: list[Editor] = []
    receipt_label = tk.StringVar(value="保存只表示写入文件；游戏回执会在此显示。")
    def act(fn):
        try:
            e = editor(); fn(e); e.save(); last_save[:] = [e]; refresh()
            receipt_label.set(f"已保存 {e.path}；等待游戏接受回执。")
        except Exception as ex: messagebox.showerror("未保存", str(ex))
    def check_receipt():
        try:
            if last_save:
                state, message = last_save[0].delivery()
                receipt_label.set(message)
        except Exception as ex: receipt_label.set(f"无法读取回执：{ex}")
        root.after(500, check_receipt)
    ttk.Label(root, textvariable=receipt_label, wraplength=1100, justify="left").pack(anchor="w", padx=8, pady=6)
    root.after(500, check_receipt)
    ttk.Label(root, text=f"输出目录：{plugins.resolve()}\n选择带 UBE 路径的记录；这些是历史观察，不保证当前仍穿着。候选审批是手工认可，不是自动验收。", justify="left").pack(anchor="w",padx=8,pady=6)
    watch_label = tk.StringVar()
    ttk.Label(root, textvariable=watch_label, wraplength=1100, justify="left").pack(anchor="w", padx=8)
    tabs=ttk.Notebook(root); tabs.pack(fill="both", expand=True)
    frame=ttk.Frame(tabs); cf=ttk.Frame(tabs); tabs.add(frame,text="观察到的装备 / 手工调整");tabs.add(cf,text="自动计算候选")
    tree=ttk.Treeview(frame,columns=("armor","addon","model"),show="headings",height=11)
    for col in tree["columns"]: tree.heading(col,text=col); tree.column(col,width=340)
    tree.pack(fill="both",expand=True)
    stock=tk.StringVar();shoe=tk.StringVar();n=tk.StringVar(value="0");h=tk.StringVar(value="0");note=tk.StringVar()
    row=ttk.Frame(frame);row.pack(fill="x")
    def chosen():
        selected=tree.selection()
        if not selected: raise ValueError("请先选择装备")
        return tree.item(selected[0],"values")
    ttk.Button(row,text="设为当前编辑的丝袜",command=lambda:stock.set(chosen()[0])).pack(side="left")
    ttk.Button(row,text="设为当前编辑的鞋",command=lambda:shoe.set(chosen()[0])).pack(side="left")
    ttk.Button(row,text="无鞋",command=lambda:shoe.set("<barefoot>")).pack(side="left")
    for text,kind in (("标记丝袜","stocking"),("标记鞋子","footwear"),("忽略该装备","ignore"),("恢复自动分类","auto")):
        ttk.Button(row,text=text,command=lambda k=kind:act(lambda e:e.mark(chosen()[0],k,addon=chosen()[1] if k=="stocking" else None))).pack(side="left")
    for label,var in (("丝袜 ID",stock),("鞋 ID",shoe),("NoHeel [0,1]",n),("Heel [0,heelMax]",h),("备注",note)):
        r=ttk.Frame(frame);r.pack(fill="x");ttk.Label(r,text=label,width=23).pack(side="left");ttk.Entry(r,textvariable=var).pack(side="left",fill="x",expand=True)
    row=ttk.Frame(frame);row.pack(fill="x",pady=8)
    ttk.Button(row,text="保存手动数值",command=lambda:act(lambda e:e.set_pair(stock.get(),shoe.get(),float(n.get()),float(h.get()),note.get()))).pack(side="left")
    ttk.Button(row,text="忽略此搭配",command=lambda:act(lambda e:e.set_pair(stock.get(),shoe.get(),0,0,note.get(),"ignore"))).pack(side="left")
    ttk.Button(row,text="清除覆盖 / 恢复自动",command=lambda:act(lambda e:e.set_pair(stock.get(),shoe.get(),0,0,"","auto"))).pack(side="left")
    ct=ttk.Treeview(cf,columns=("shoe","stock","n","h","residual","decision"),show="headings")
    for col in ct["columns"]:ct.heading(col,text=col);ct.column(col,width=160)
    ct.pack(fill="both",expand=True)
    shown_candidates = []
    def approve():
        if not ct.selection(): return
        if messagebox.askyesno("手工认可候选", "即使自动策略拒绝了该值，仍按当前来源/身材绑定认可它？\n这不是已验证的无穿模配置。"):
            act(lambda e:e.approve(int(ct.selection()[0]), expected=shown_candidates[int(ct.selection()[0])] ))
    ttk.Button(cf,text="手动认可选中候选（来源绑定）",command=approve).pack()
    def refresh():
        try:
            e=editor();watch_label.set(f"游戏监视文件：{e.path}（{'运行中' if recent(e.connection) else '暂无实时回执'}）");tree.delete(*tree.get_children());ct.delete(*ct.get_children())
            for i,v in enumerate(e.inventory()):tree.insert("","end",iid=str(i),values=(v.get("armor"),v.get("addon"),v.get("model")))
            shown_candidates[:]=e.candidates()
            for i,v in enumerate(shown_candidates):ct.insert("","end",iid=str(i),values=(v["footwear"]["armor"],v["stocking"]["armor"],v["NoHeel"],v["Heel"],v["normalizedResidual"],v.get("policyDecision")))
        except Exception as ex:messagebox.showerror("读取失败",str(ex))
    ttk.Button(root,text="刷新文件",command=refresh).pack(pady=4);refresh();root.mainloop()


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugins",type=Path,required=True,help="Directory containing generated output, usually MO2 overwrite/SKSE/Plugins")
    parser.add_argument("--user-file",type=Path,help="Optional explicit winning user overlay path")
    sub=parser.add_subparsers(dest="command",required=True)
    for cmd in ("status","list","candidates","gui"):sub.add_parser(cmd)
    mark=sub.add_parser("mark");mark.add_argument("armor");mark.add_argument("kind",choices=["stocking","footwear","ignore","auto"]);mark.add_argument("--addon");mark.add_argument("--note",default="")
    pair=sub.add_parser("set");pair.add_argument("stocking");pair.add_argument("footwear");pair.add_argument("--noheel",type=float,default=0);pair.add_argument("--heel",type=float,default=0);pair.add_argument("--mode",choices=["manual","ignore","auto"],default="manual");pair.add_argument("--note",default="")
    approve=sub.add_parser("approve");approve.add_argument("index",type=int)
    args=parser.parse_args()
    if args.command=="gui":gui(args.plugins,args.user_file);return
    e=Editor(args.plugins,args.user_file)
    if args.command in ("status","list","candidates"):
        result={"status":lambda:{"runtime":e.runtime,"configuration":e.connection},"list":e.inventory,"candidates":e.candidates}[args.command]()
        print(json.dumps(result,indent=2,ensure_ascii=False));return
    if args.command=="mark":e.mark(args.armor,args.kind,args.note,args.addon)
    elif args.command=="set":e.set_pair(args.stocking,args.footwear,args.noheel,args.heel,args.note,args.mode)
    elif args.command=="approve":e.approve(args.index)
    e.save();print(f"Saved {e.path}; fingerprint={e.saved_fingerprint}. Waiting for a game receipt; a file save is not proof of hot reload.")

if __name__=="__main__":
    try:main()
    except (ValueError,RuntimeError,OSError,KeyError,json.JSONDecodeError) as e:raise SystemExit(f"No changes applied: {e}")
