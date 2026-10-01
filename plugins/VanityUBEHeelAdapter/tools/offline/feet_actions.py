"""Explicit UI scopes and an opt-in unmeasured manual trial, never auto rules."""
from __future__ import annotations
from .engine import DEFAULT_ANCHOR

RETRY_STATUSES = frozenset(('no-usable-Feet', 'no-reference-feet',
    'geometry-read-error-no-reference', 'reference-feet-not-validated',
    'multiple-eligible-shapes-needs-review'))


def rescan_keys(rows, stocking_keys, anchor=DEFAULT_ANCHOR):
    """Freeze exact keys only; new Scanner resolves CURRENT winning records."""
    requested = set(stocking_keys)
    socks = [r['key'] for r in rows if r.get('key') in requested and r.get('kind') == 'stocking']
    if not socks or set(socks) != requested:
        raise ValueError('先在丝袜页明确选择至少一件已可测丝袜；不会默认重算全部丝袜。')
    shoes = [r['key'] for r in rows if r.get('kind') == 'footwear'
             and r.get('UBE') and r.get('status') in RETRY_STATUSES]
    if not shoes:
        raise ValueError('当前清单没有待复查的参考脚问题；先载入旧 offline-catalog.json。')
    anchors = [r['key'] for r in rows if r.get('kind') == 'footwear'
               and anchor.casefold() in (r.get('key', '').casefold(), r.get('armor', '').casefold())
               and r.get('status') == 'measurable']
    if len(anchors) != 1:
        raise ValueError('平脚参考不唯一或未找到；请指定准确的参考鞋 armor::addon。')
    return sorted(set(socks + shoes + anchors))


def witchy_cpb_trial(stocking, footwear):
    """Return editable form fields only. No measured/source-approval claims."""
    norm = lambda value: value.replace('/', '\\').casefold()
    if (not stocking or not footwear or not stocking.get('UBE') or not footwear.get('UBE')
            or norm(stocking.get('model', '')) != '!ube\\caenarvon\\cosplay\\cpb_sb_1.nif'
            or norm(footwear.get('model', '')) != '!ube\\[spaz490]\\witchy agata heels\\witchy_1.nif'
            or footwear.get('armor', '').casefold() != 'witchy agata heels.esp|00000800'
            or footwear.get('addon', '').casefold() != 'witchy agata heels ube patch.esp|00000801'
            or not stocking.get('armor', '').casefold().startswith('[caenarvon] cosplay basics.esp|')
            or not stocking.get('addon', '').casefold().startswith('[caenarvon] cosplay basics ube patch.esp|')):
        raise ValueError('此试调仅用于准确 Witchy 鞋＋CPB bodystocking。请在两侧各指向一条对应记录。')
    return {'stocking': stocking['armor'], 'footwear': footwear['armor'],
            'NoHeel': '0', 'Heel': '0.47', 'mode': 'manual',
            'note': 'UNVALIDATED Witchy + CPB manual trial; Agata 0.47 starting value, not measured for Witchy. Visual review required.'}
