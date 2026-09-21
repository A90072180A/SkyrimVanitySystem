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


## Phase 5: per-stocking morph profiles

The scoped morph engine is no longer hard-wired to `NoHeel`. A stocking can
select any RaceMenu/BodySlide morph through `stockingMorphProfiles`.

The footwear profile remains a normalized posture factor:
- `0.0` = heel/base posture
- `1.0` = flat posture

Each stocking morph profile maps that factor into the actual morph value:

`target = heelValue + posture * (flatValue - heelValue)`

Experimental profile for DX FetishFashion TooHotForYou Bodystocking:

```json
{
  "modelContains": "TooHotForYou Bodystocking",
  "morph": "HiHeelz_CBBE_to_UBE",
  "heelValue": 0.0,
  "flatValue": 1.0
}
```

The TRI contains `HiHeelz_CBBE_to_UBE`, but its physical direction is not yet
verified in game. If Converse with posture 1.0 bends the stocking the wrong way,
swap `heelValue` and `flatValue` and retest.

Items without a matching explicit morph profile continue to use the validated
default `NoHeel` mapping `0 -> heel, 1 -> flat`.


## Phase 5.1: profile classification and equip persistence

A matching `stockingMorphProfiles` entry now also counts as explicit evidence
that a visible SVS item is a stocking. This fixes profile-driven items such as
TooHotForYou that do not reach the generic hosiery classifier threshold.

The adapter also listens for player `TESEquipEvent` equip/unequip changes.
Because ordinary equipment changes can rebuild player 3D and RaceMenu can then
reapply actor-wide body morphs, the adapter performs a deferred SVS snapshot
after two task hops and reapplies the scoped stocking morph without requiring an
SVS rule change.

The local RaceMenu interface declarations were also aligned with the public
skee ABI by removing virtual destructors from callback-only interfaces that do
not have them upstream.


## Phase 6: validate NoHeel from the actual runtime TRI

Automatic stocking adaptation is now conservative by default. After SVS/DAVE
resolves the real rendered stocking node, the adapter reads that node's BODYTRI
file and checks whether the TRI actually contains the ASCII morph name
`NoHeel`.

Only a validated `NoHeel` stocking receives the default automatic heel
adaptation. A hosiery item without `NoHeel` remains visible but is logged as
non-morphable and left untouched.

The previous experimental TooHotForYou / `HiHeelz_CBBE_to_UBE` profile was
removed from the default config. `stockingMorphProfiles` remains available as
an advanced explicit override for a future item whose alternate morph semantics
are independently known and tested.

Capability checks are cached by BODYTRI path for the session and cleared when
the adapter config reloads, so rebuilding a TRI and reloading a save can be
tested without stale capability data.


## Phase 7: BodySlide footwear posture resolver POC

The adapter now builds a lazy index of the MO2-visible BodySlide files under
`Data/CalienteTools/BodySlide/SliderSets` and
`Data/CalienteTools/BodySlide/SliderPresets`.

For each visible footwear visual it matches the exact SVS model path against a
BodySlide `SliderSet` output path/file and looks for an explicit `NoHeel`
value. Resolution priority is:

1. manual `heels` config (authoritative);
2. an exact matching `-Zeroed Sliders-` preset with both small and big
   `NoHeel` values (high-confidence automatic posture);
3. the SliderSet's own small/big `NoHeel` defaults (diagnostic by default).

Small/big values are interpolated using the player's current Skyrim body weight
and converted from BodySlide percent to the adapter's normalized posture
`0..1`.

`allowSliderSetDefaultPosture` defaults to `false`: SliderSet defaults are
logged but not automatically trusted because an outfit may have been built with
a different preset. `HiHeelz_CBBE` is detected for diagnostics only and is
never converted into posture automatically.

The bundled Converse manual profile remains in place, so this phase can compare
the automatic BodySlide candidate against the known-good manual value without
risking the validated stocking behavior. Look for
`[footwear auto candidate]` in the log.


## Phase 7.1: non-blocking BodySlide indexing

The first Phase 7 implementation built the BodySlide index lazily on the game
thread. On a large setup this caused a visible freeze while thousands of OSP/XML
files were opened and parsed.

The index is now prewarmed on a background `std::jthread` after DataLoaded.
If footwear is evaluated before indexing finishes, the resolver returns
immediately instead of waiting. Manual footwear profiles continue to work during
that interval. When indexing completes, the worker queues a fresh SVS
evaluation so automatic candidates become available without another outfit
change.

Look for:
`[bodyslide] starting asynchronous index build`,
`[bodyslide posture] index not ready; returning without blocking the game thread`,
and finally
`[bodyslide] asynchronous index ready in ...s`.


## Phase 7.2: footwear resolver trace

When `logBodySlideCandidates=true`, the footwear resolver now emits a
one-time trace for each normalized visible footwear model. The trace records:

- the original SVS model path and normalized model stem;
- whether an exact SliderSet output-path match exists;
- up to eight same-basename alternatives when the exact path misses;
- every exact matching SliderSet's `NoHeel` and `HiHeelz_CBBE`
  small/big values;
- matching zeroed-preset entries and their `NoHeel` values;
- the precise reason a set yields no posture candidate;
- the final selected candidate, if any.

This is diagnostic-only and does not relax the current confidence policy.
Manual footwear profiles still win.


## Phase 8: runtime footwear geometry diagnostics and generated profiles

The adapter now captures the actual third-person BipedAnim partClone used by a
visible footwear visual. Matching prefers the SVS replacement ARMA and exact
model/BODYTRI stem, then records every BSGeometry under that runtime part.

For each geometry the diagnostic records the name, RTTI type, BSTriShape vertex
and triangle counts when available, skin-partition counts, model-space bound and
world-space bound. Geometry names that look foot-related are flagged as
`footLikeName` for the next embedded-foot fitting phase.

No geometry-derived posture is applied in this phase. The result is written to:

`Data/SKSE/Plugins/VanityUBEHeelAdapter.generated.json`

The generated file is intentionally separate from
`VanityUBEHeelAdapter.json`. Manual configuration remains authoritative.
Generated entries currently use `status: diagnostic-only`, `posture: null`,
and include a `geometryFit` placeholder for the later UBE-foot least-squares
solver.

Set `geometryDiagnostics=false` to suppress runtime capture, or
`writeGeneratedProfiles=false` to keep log-only diagnostics.
