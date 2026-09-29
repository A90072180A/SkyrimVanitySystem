# 0.19.0 reversible prepared stocking-foot occlusion

The adapter can now hide only a locally prepared CPB foot-fabric shape for an
explicitly marked opaque closed shoe. Transparent, open, unknown, multiple-shoe
and barefoot states preserve the fabric. No automatic shoe-coverage classifier is
enabled.

Run `python tools/prepare_cpb_occlusion_patch.py gui` through MO2. Choose the
virtual Data folder and a new MO2 output mod. The generator accepts only the CPB
source hashes validated for this project, never edits Data in place, writes a new
BODYTRI resource and creates the shape `VHA_CPB_CoveredFoot`. Place the generated
mod after BodySlide Output and regenerate it after a BodySlide rebuild.

In the height editor select a footwear item and mark it as either
`opaque-closed` or `preserve`. Glass should remain preserve. The supplied Agata
sample is appropriate for an opaque-closed test. Only the prepared named geometry
is culled; the whole stocking and skin feet are not hidden.

Prepared CPB has two sibling shapes with the same BODYTRI. Runtime matching
coalesces them under the current active biped clone and sends one scoped RaceMenu
height transaction, preserving Heel/NoHeel control on both shapes.

Because prepared NIF/TRI bytes differ, old CPB offline-height-library source
fingerprints are stale by design. Re-export CPB library entries after enabling the
generated patch. Explicit manual pair values remain usable.

Glass local tightening is intentionally not shipped in 0.19.0: the current
prototype is source-weight-0 only and is not yet validated for arbitrary OBody
presets or animation.
