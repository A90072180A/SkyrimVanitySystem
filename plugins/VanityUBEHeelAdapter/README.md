# Vanity UBE Heel Adapter — phase 3 scoped morph POC

Phase 3 adds the first real mesh mutation, but only through RaceMenu's public
BodyMorph API and only on an attachment that can be correlated to the configured
visual stocking.

## Important test condition

For this POC, disable Automatic Stocking's own `NoHeel` writer while testing
(or set its `noHighHeelSlideName` to an empty string). Otherwise a later
actor-wide RaceMenu refresh can legitimately overwrite this local test value.

## What changed

The adapter now:

- acquires RaceMenu/skee `BodyMorph` and `ActorUpdateManager` interfaces;
- observes third-person player armor attachment callbacks;
- caches the actual attached `NiAVObject` and its `BODYTRI` path;
- correlates a configured SVS stocking geometry to an attached node:
  1. exact replacement ARMA FormID;
  2. fallback model/BODYTRI path-stem match;
- applies a continuous target `NoHeel` value only to that node with
  `IBodyMorphInterface::ApplyVertexDiff`.

It does **not** call actor-wide `ApplyBodyMorphs` or `UpdateModelWeight`.

## Scoped morph mechanism

RaceMenu still stores morph values per actor, so the adapter uses a temporary
key named `VanityUBEHeelAdapter`:

1. read the actor's current total `NoHeel`;
2. subtract any pre-existing adapter key;
3. temporarily set the adapter key so the total equals the desired target;
4. call `ApplyVertexDiff(actor, stockingNode, false)`;
5. immediately restore/clear the temporary key.

This lets RaceMenu reset the node from its saved `SHAPEDATA` base and reapply
all OBody / RaceMenu body sliders while the selected stocking alone sees the
requested `NoHeel` total.

## Config

```json
{
  "applyMorph": true,
  "stockings": ["runtime:FE429D1A"],
  "heels": {
    "runtime:FE4AB80A": 1.0
  }
}
```

The included profile still uses the current test runtime FormIDs:

- `FE429D1A`: CPB stocking;
- `FE4AB80A`: Converse;
- Converse: `NoHeel = 1.0`.

## Expected log

A successful local pass should include:

```text
[RaceMenu attach] ... BODYTRI='...'
[stocking target] ... method=exact-arma|bodytri-stem ...
[local morph] applied ... targetNoHeel=1.000 ...
[heel plan] ... morphApplied=true
```

If no safe attachment match is found, the adapter does not mutate anything and
prints the current RaceMenu attachment cache.

## First game test

1. Start with Automatic Stocking's `NoHeel` output disabled.
2. Display the configured CPB stocking through SVS slot 32.
3. Display the configured Converse through slot 37.
4. Confirm the stocking visually becomes the flat-foot form.
5. Remove/re-add the stocking and switch outfits a few times.
6. Send the full `VanityUBEHeelAdapter.log`.

Do not test arbitrary high heels yet; the only configured footwear target in
this POC is the flat Converse profile.

## Phase 3.2 biped diagnostics

If DAVE visual replacements are absent from the ordinary player scenegraph BODYTRI walk, the adapter now inspects the third-person BipedAnim `objects` and `bufferedObjects` partClone trees for all 32 biped slots. It logs item/addon FormIDs, BODYTRI paths, node roots, and geometry names. A local NoHeel pass is attempted only when a partClone BODYTRI stem exactly matches the configured SVS stocking model; otherwise the build remains diagnostic-only for that target.


## Phase 4: stable IDs, continuous heel profiles, conservative auto-detection

Phase 4 removes the test dependency on runtime FormIDs. Config entries may now use
`Plugin.esp|localFormID`, which remains stable when the load order changes. Legacy
`runtime:XXXXXXXX` entries are still accepted.

Heel profiles remain continuous floats in the inclusive range `0.0..1.0`:
`0.0` is the high-heel/base posture and `1.0` is the flat-foot `NoHeel`
posture for the current UBE convention. Intermediate values are applied directly
through the already validated scoped RaceMenu morph path.

The adapter can also conservatively auto-detect obvious hosiery visual items from
their SVS model paths and visual slots. Auto-detection currently requires a high
confidence score; explicit config entries always win. Footwear target values are
still profile-driven in this phase. Automatic heel-height estimation is intentionally
left for the next phase so the validated local-morph path is not mixed with an
unverified estimator.

Set `debugDiagnostics` to `true` to restore the verbose logical-item and biped
partClone dumps used during the POC. Normal operation now keeps only target,
profile, and morph-result messages at info level.
