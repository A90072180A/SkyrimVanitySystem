"""Pure catalog filtering/selection. Filters never change measurement eligibility."""
from __future__ import annotations
from dataclasses import dataclass
import re

PAIR_BUDGET = 20000


def normalized(value):
    return str(value or '').replace('/', '\\').casefold()


def text_match(value, query):
    """Literal, case-insensitive: spaces=AND; | / comma / semicolon=OR.

    ! is a literal character (not NOT), so !UBE works without escaping.
    No regex, shell parsing or code evaluation; backslashes remain literal.
    """
    haystack = normalized(value)
    groups = [g.strip() for g in re.split(r'[|,;，；]', query or '') if g.strip()]
    return not groups or any(all(normalized(t.strip('"')) in haystack
        for t in re.findall(r'"[^"]*"|\S+', g)) for g in groups)


def origin(stable_id):
    return str(stable_id or '').split('|', 1)[0]


def eligible(row, role=None):
    return (row.get('status') == 'measurable'
            and row.get('kind') in ('stocking', 'footwear')
            and (role is None or row.get('kind') == role)
            and not row.get('blockedBy') and row.get('eligibleForHeight', True) is not False)


def kind_match(row, kind):
    if kind in ('', 'all'): return True
    if kind in ('raised-foot-pose', 'flat-like-foot-pose'):
        return row.get('kind') == 'footwear' and row.get('footPose', {}).get('kind') == kind
    return row.get('kind') == kind


def status_match(row, status):
    value = row.get('status', '')
    if status in ('', 'all'): return True
    if status == 'measurable': return eligible(row)
    if status == 'not-measurable': return not eligible(row)
    if status == 'source-error': return value.startswith('source-error:')
    if status == 'quarantine': return bool(row.get('blockedBy')) or 'quarantin' in value
    return value == status


def fields(row):
    plugins = [origin(row.get('armor')), origin(row.get('addon')),
               row.get('sourcePlugin', ''), row.get('armorWinnerPlugin', ''),
               row.get('addonWinnerPlugin', '')]
    mods = row.get('modelProviderMods', [])
    physical = row.get('modelPhysicalPaths', [])
    return {'name': row.get('name', '') + ' ' + row.get('editorID', ''),
            'plugin': '\n'.join(plugins), 'model': row.get('model', ''),
            'provider': '\n'.join(mods + physical),
            'id': row.get('key', '')}


@dataclass(frozen=True)
class CatalogFilter:
    kind: str = 'all'
    status: str = 'measurable'
    name: str = ''
    plugin: str = ''
    model: str = ''
    provider: str = ''
    search: str = ''

    def matches(self, row):
        if not kind_match(row, self.kind) or not status_match(row, self.status): return False
        f = fields(row)
        return (all(text_match(f[k], getattr(self, k)) for k in ('name', 'plugin', 'model', 'provider'))
                and text_match('\n'.join(f.values()), self.search))


def select_rows(rows, keys, role):
    pool = [r for r in rows if eligible(r, role)]
    if keys is None: return pool  # Explicit API/CLI all-mode only. [] means NONE.
    requested = set(keys)
    chosen = [r for r in pool if r['key'] in requested]
    unknown = requested - {r['key'] for r in chosen}
    if unknown: raise ValueError(f'{role}: selected key is missing/ineligible: {sorted(unknown)[0]}')
    return chosen


def pair_plan(rows, stocking_keys, shoe_keys, max_pairs=PAIR_BUDGET):
    if isinstance(max_pairs, bool) or not isinstance(max_pairs, int) or max_pairs < 1:
        raise ValueError('invalid pair budget')
    socks = select_rows(rows, stocking_keys, 'stocking')
    shoes = select_rows(rows, shoe_keys, 'footwear')
    count = len(socks) * len(shoes)
    reason = ('请分别选择至少一条可测丝袜和一双可测鞋；空选择不会计算全部。' if not count else
              f'{len(socks)} × {len(shoes)} = {count:,} 对，超过 {max_pairs:,} 对上限；请缩小任意一侧的筛选／选择范围。'
              if count > max_pairs else '')
    return {'stockings': socks, 'shoes': shoes, 'count': count,
            'budget': max_pairs, 'allowed': bool(count and count <= max_pairs), 'reason': reason}


class CatalogSelection:
    """Persistent across pages, never across a filter that hides an item."""
    def __init__(self, rows=()):
        self.rows = list(rows)
        self.selected = set()
        self.visible = list(self.rows)

    def filter(self, spec):
        self.visible = [r for r in self.rows if spec.matches(r)]
        allowed = {r['key'] for r in self.visible if eligible(r)}
        self.selected.intersection_update(allowed)
        return self.visible

    def update_page(self, page_keys, selected_keys):
        self.selected.difference_update(page_keys)
        allowed = {r['key'] for r in self.visible if eligible(r)}
        self.selected.update(set(selected_keys) & allowed)

    def select_filtered(self):
        self.selected = {r['key'] for r in self.visible if eligible(r)}

    def selected_keys(self):
        return [r['key'] for r in self.visible if r['key'] in self.selected]
