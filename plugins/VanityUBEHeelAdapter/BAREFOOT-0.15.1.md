# 0.15.1: confirmed barefoot is not unknown footwear

The 0.15.0 controller only selected a height plan with exactly one matched shoe.
With none, it restored normal actor morphs, which can leave a stocking's *built*
heel pose. Restoring actor keys does not imply NoHeel=1 or a physically flat mesh.

## Change

`barefootFlatFeet` defaults to true. A separate branch requests **NoHeel=1,
Heel=0**, together in the existing one-pass scoped transaction. It does not write
to the body, shoes, skeleton, NIF/TRI, OBody keys or the persisted measured profile.
Stocking morph capability and the existing live target checks are still required.
Missing NoHeel, wrong shape/unsupported TRI, or a removed stocking never receive
this policy. Disabling the controller still restores normal actor morphs.

A positive naked-foot classification requires all of:
- a valid live SVS snapshot, with no advertised or live non-stocking foot visual;
- no actually worn slot-37 armor;
- the current, non-buffered slot-37 part belongs to the actor's skin armor;
- its ARMA is owned by that skin armor, and the sex-specific model exactly matches
  one of `referenceFeetModels` (the bundled UBE naked-foot models);
- current skinned geometry exists; the state remains stable for at least 250 ms.

The existing one-second watchdog rechecks without sleeping on the game thread.
This handles missing BODYTRI/name changes on the naked-foot node without guessing
from the names "Feet" or "skin". The stocking's actual BODYTRI remains mandatory.
A physically worn shoe hidden by a mod is deliberately not treated as barefoot by
this narrow fallback. Unknown skin models also remain unconfirmed. These cases
can be extended later with verified visible-state information, not lack of a fit.

Advertised shoes still loading, shoes without BODYTRI, unsupported shoes, multiple
shoes and a temporary empty biped during rebuilding do NOT activate the flat rule.
Repeated identical states do not resubmit the morph. Equipment/session changes
reset the settling window and already-managed objects retain the existing restore
semantics on loss of a valid plan. No footwear height threshold has been relaxed.

## Diagnosis retained from the supplied 0.15.0 run

- Converse+CPB submitted NoHeel=1/Heel=0 from the legacy manual profile.
- Glass was measured at bounded NoHeel=0/Heel=1, raw Heel=1.1067679, normalized
  residual=0.20775995, saturated=true. It remains rejected by the default 0.15
  residual/no-extrapolation policy. This release does not claim Glass is fixed.
- Agata's 8137-vertex foot differs from the 8837-vertex reference. The report says
  target-topology-mismatch; it remains unresolved, not a barefoot state.

New state-change-only `[height decision]` lines distinguish unknown/ambiguous
footwear, measured residual/range refusals, naked-skin verification and settling.
A successful flat call has `shoe='<barefoot>' source=confirmed-barefoot-flat`.

## Install and scope

Update only the Adapter DLL/config, keep the build13-compatible SVS DLL and current
BodySlide/OBody output. Existing configs get the new true default; optionally add
`"barefootFlatFeet": true`. `referenceFeetModels` already exists in the 0.15 config.
No changes to fit/clearance/height-mapping math, no extra raw geometry collection,
no new per-shoe manual override, no new game or BodySlide rebuild is required.
The added exhaustive policy and repeated transaction checks run in Linux sanitizer
and Windows-with-headers CI alongside the existing suites. Actual engine skin
selection and rendered results remain an integrated game acceptance boundary.
