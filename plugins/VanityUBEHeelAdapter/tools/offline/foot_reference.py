"""Conservative reference-foot selection; no model renaming or proxy feet.

The additional names are only hints. Full ordered topology and transforms must
match the audited UBE reference domain. Hashes contain no proprietary mesh data.
Classic 'Feet' retains its old eligibility path (including supported subsets).
"""
from __future__ import annotations
import hashlib
import math
import struct

TOOL_VERSION = '0.20.1'
RUNTIME_VERSION = '0.20.0'
ALIASES = frozenset(('feeth', 'heelfeet', 'ube_feet', 'femalefeet'))
REFERENCE_VERTICES = 8837
REFERENCE_TRIANGLES = 16200
REFERENCE_TOPOLOGY = 'f2cde8398291c03d47dd2867ae0e82a31e3344e199c1e2f8cad03658ffad725e'
IDENTITY = {'rotation': (1., 0., 0., 0., 1., 0., 0., 0., 1.),
            'translation': (0., 0., 0.), 'scale': 1.}


def topology_digest(shape):
    points, triangles = shape.get('points', ()), shape.get('triangles', ())
    h = hashlib.sha256(struct.pack('<II', len(points), len(triangles)))
    for triangle in triangles:
        h.update(struct.pack('<3H', *triangle))
    return h.hexdigest()


def identity_transform(value):
    if not isinstance(value, dict):
        return False
    return (tuple(value.get('rotation', ())) == IDENTITY['rotation']
            and tuple(value.get('translation', ())) == IDENTITY['translation']
            and value.get('scale') == 1.)


def alias_validation(shape, body_morphs=None):
    if shape.get('status') != 'complete':
        return 'reference-geometry-unreadable'
    if shape.get('name', '').casefold() not in ALIASES:
        return 'not-a-reference-name'
    # Do not silently omit a preset on a foot whose BODYTRI is absent.
    if any(value != 0 for value in (body_morphs or {}).values()):
        return 'alias-preset-context-not-validated'
    points, triangles = shape.get('points', ()), shape.get('triangles', ())
    if len(points) != REFERENCE_VERTICES or len(triangles) != REFERENCE_TRIANGLES:
        return 'alias-topology-not-validated'
    if any(len(p) != 3 or not all(math.isfinite(x) for x in p) for p in points):
        return 'alias-nonfinite-geometry'
    if topology_digest(shape) != REFERENCE_TOPOLOGY:
        return 'alias-topology-not-validated'
    if (not identity_transform(shape.get('local')) or not identity_transform(shape.get('skin'))
            or not all(identity_transform(t) for t in shape.get('parentTransforms', ()))):
        return 'alias-transform-not-validated'
    return 'validated-full-UBE-reference'


def select_reference(shapes, body_morphs=None):
    """Return (eligible shapes, diagnostic); never manufacture a reference."""
    classic = [s for s in shapes if s.get('name', '').casefold() == 'feet']
    valid = [s for s in classic if s.get('status') == 'complete']
    if classic:
        return valid, {'method': 'classic-Feet', 'reason':
                       'classic-reference' if len(valid) == 1 else 'classic-reference-unreadable-or-ambiguous'}
    named = [s for s in shapes if s.get('name', '').casefold() in ALIASES]
    checked = [{'name': s['name'], 'reason': alias_validation(s, body_morphs)} for s in named]
    valid = [s for s, check in zip(named, checked) if check['reason'] == 'validated-full-UBE-reference']
    if len(named) > 1:
        # An unreadable competing candidate must not be silently ignored.
        return [], {'method': 'alias', 'reason': 'multiple-reference-candidates', 'candidates': checked}
    if len(valid) == 1:
        return valid, {'method': 'validated-alias', 'geometry': valid[0]['name'],
                       'reason': 'validated-full-UBE-reference', 'topologySHA256': REFERENCE_TOPOLOGY}
    reason = checked[0]['reason'] if checked else (
        'geometry-read-error-no-reference' if any(s.get('status') != 'complete' for s in shapes)
        else 'no-reference-feet')
    return [], {'method': 'none', 'reason': reason, 'candidates': checked,
                'shapeErrors': [{'name': s['name'], 'reason': s.get('status')} for s in shapes
                                if s.get('status') != 'complete']}


def failure_status(diagnostic):
    reason = diagnostic['reason']
    if reason == 'no-reference-feet':
        return reason
    if reason == 'geometry-read-error-no-reference':
        return reason
    if reason == 'multiple-reference-candidates':
        return 'multiple-eligible-shapes-needs-review'
    return 'reference-feet-not-validated'


def failure_text(diagnostic):
    reason = diagnostic['reason']
    labels = {
        'no-reference-feet': '已读取几何，但没有可识别的独立参考脚；可手工配对，不猜测高度。',
        'geometry-read-error-no-reference': '部分几何读取失败，且未取得参考脚；查看 shapeErrors。',
        'classic-reference-unreadable-or-ambiguous': 'Feet 读取失败或存在歧义；不能自动测量。',
        'multiple-reference-candidates': '存在多个参考脚候选；需要核对，未自动选择。',
        'alias-topology-not-validated': '别名脚的完整拓扑未通过核验，不能只凭名称或顶点数接受。',
        'alias-transform-not-validated': '别名脚的局部、蒙皮或父节点变换不符合已核验坐标系。',
        'alias-preset-context-not-validated': '别名脚暂限已生成 NIF 基准；非零额外 preset 未验证。',
        'reference-geometry-unreadable': '候选参考脚未能完整读取。',
    }
    return labels.get(reason, reason)
