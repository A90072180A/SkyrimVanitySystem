# 0.15.0 — transactional height controller and persistent measured profiles

This consolidates height work into a running controller rather than adding another
clearance preview. No clothing tightening, NIF/OSD rewrite, or foot/body rescale is
performed. The SVS API, SVS DLL, GUI and render implementation are untouched.

## Runtime behavior

- One signed control for a particular stocking: +q means NoHeel=q / Heel=0;
  -q means NoHeel=0 / Heel=q. Both are set together before exactly one local
  RaceMenu call, and both temporary adapter keys are restored afterwards. Other
  mods' keys are not deleted. The actual effective values are verified; unknown
  non-additive aggregation that cannot reach the target is refused.
- Legacy `heels` values remain positive NoHeel values. Explicit `signedHeightOverrides`
  for a stocking/footwear pair have precedence. No global 0.6673 conversion or
  `NoHeel=1-HiHeelz` assumption is introduced.
- The structured packed TRI parser checks the native shape/morph, not an ASCII
  substring. Missing/unsupported resources are not treated as present. This I/O
  runs in a bounded background capability queue.
- Only current active biped roots with an exact BODYTRI/model-stem match are used.
  Buffered objects and stale attachment-cache pointers cannot win. Nested copies
  of a marker are one attachment; multiple distinct matches are ambiguous.
  A direct geometry root is refused for a multi-shape TRI because RaceMenu can
  otherwise apply unrelated shape records to that one geometry.
- An equip/SVS/RaceMenu notification queues an update. A 1-second watchdog handles
  delayed rebuilds and condition changes. Unchanged context, controls and skin
  partition identity do not incur another morph call.
- Switching to unknown footwear, disabling the controller or removing the visual
  restores currently live formerly managed objects to the actor's normal morphs.
  Detached objects are released without writes. New/load sessions invalidate old
  tasks and revalidate cached sources. No permanent actor key is saved.

## Automatic measured branch selection

The existing source-verified calibration pipeline now measures both *actual*
stocking deltas, reconstructs the original baseline, and compares two bounded
single-branch fits. It does not solve an ill-conditioned free two-slider problem.
A profile has context, source hashes, raw and bounded coefficients, residuals and
saturation status. Returning the manual reference is only a consistency check.

`height-profiles.json` is separate from manual configuration. It is written using
a temp file plus atomic replacement. Loading requires schema/algorithm/field
checks and unchanged source files. Using it additionally requires a freshly
observed current foot fingerprint, verified stocking source, matching actor
context, and the runtime stocking TRI fingerprint. Changed BodySlide builds,
missing inputs, different presets and unsupported topology fail closed.

Defaults remain conservative: normalized residual <=0.15, no saturated endpoints.
The actual supplied Glass replay selects NoHeel=0 / Heel=1 but its residual is
about 0.20777 and its unconstrained Heel is about 1.10677. It is therefore recorded
but NOT automatically applied with default settings. No threshold was relaxed to
hide this limitation. The example manual pair override is included separately;
it is not loaded and is not a claim that the endpoint is an exact fit.

A measured interior profile passing these checks is automatically applied. There
is no claim that a mathematical acceptance substitutes for visual/animation
validation on the user's machine. Unsupported renamed/missing BODYTRI targets
remain skipped rather than being repaired by unverified metadata insertion.

## Configuration and installation

Replace the adapter DLL and use/merge the bundled configuration; retain your
working SVS build13-compatible DLL, UBE, RaceMenu, OBody and BodySlide outputs.
Automatic Stocking remains disabled. No BodySlide rebuild or new game is needed.
Save/load or restart after changing configuration (no per-frame config file scan).

Important new fields:

```json
"automaticHeight": true,
"heightMaxNormalizedResidual": 0.15,
"allowHeightEndpointApproximation": false,
"signedHeightOverrides": []
```

A deliberate manual override uses stable IDs, for example the separately supplied
`examples/signed-height-example.json`. Signed values are strictly in [-1,1].
Invalid or duplicate pair entries are refused, not clipped. Legacy `heels` entries
are still strictly in [0,1] and higher priority than measured profiles.

The full BodySlide XML scan is retired from the active controller; it did not
provide the required posture data in this setup. Old XML-diagnostic settings may
remain in an old config but do not become height authority. Arbitrary experimental
`stockingMorphProfiles` from 0.5 are not the height controller's interface.

The configured source/donor/reference declarations from 0.14 remain necessary for
new automatic calibration. The verified manual anchor is Converse + CPB. Showing
it during normal play supplies calibration context; no barefoot capture is needed.
Cached results are reused after input and current-context validation. This is a
height-profile cache, not a complete BodySlide-library cache.

Raw geometry dumping is off by default to avoid accumulating megabytes per load;
internal geometry measurement still runs when automaticHeight is enabled. Set
`exportFootGeometry=true` when a full diagnostic snapshot is needed. Clearance
and tightening stages are paused even if an old config still enables them.

Useful outputs:
- `height-profiles.json`: persisted measured branch candidates (including rejections).
- `calibration-candidates.json`: detailed source/calibration evidence.
- log `[height apply]`: both values, authority, and submission result.
- log `[height reset]`: restoration after losing a valid target/plan.
- `BUILD.json` in the package: exact source commit and DLL SHA-256.

## Verification scope

Portable tests exercise branch bounds, missing/singular morphs, negative/free
coefficients, residual rejection, 60 repeated alternating transactions, key
restoration on exceptions, duplicate/NaN rejection and non-additive refusal.
The existing parser/BVH/decoder suites remain. Every portable test is also built
with Windows headers and NOMINMAX. The full DLL is built separately in CI.

HeightProfileCacheTests exercises the actual store and packed TRI capability reader
on temporary synthetic files: malformed input, invalid controls, source hash
changes, atomic replacement, reload, current-context gating and stale-generation
rejection. This is not a game or scheduler timing test.

The supplied 0.14 snapshots are replayed offline through the SAME C++ height
kernel (tools/replay_height.py + replay_height.cpp). Meshes are not committed.
This replay checks the measurement path, not Skyrim event timing or visual fit.
The controller's full game behavior is still an integrated in-game acceptance
item; there is no request for a sequence of separate micro-tests.
