# 0.14.0 - separate stocking clearance from foot posture

## Build repair

The 0.13.0 run #28 failed in the full DLL, although standalone MSVC tests passed.
Windows min/max macros reached SurfacePostureCore through the plugin PCH.
The adapter target now defines NOMINMAX before its PCH. CI also compiles every
portable test with /DNOMINMAX /FIWindows.h so the platform include is exercised.

## Measured evidence, not an installed repair

No new 0.13 in-game data was supplied. We re-used the verified 0.12 Converse,
Glass and CPB snapshots with identical actor context. Across 3228 selected
NoHeel-affected vertices, the CPB-to-Converse-embedded-foot gap has RMS 0.548026,
median 0.489624, p95 0.889443 and max 1.236376 model units. Normal-component RMS
is 0.544985; tangential RMS is 0.057655. These distances are not centimetres and
do not establish CPB's separation from the live naked body.

A triangle-frame rotation of that gap changes the Glass raw candidate from
-0.738496 to -0.748113. RMS residual changes from 0.697500 to 0.668409. Both
remain rejected. Thus clearance transport matters, but this experiment does not
support blaming the entire negative result on looseness or using 1-HiHeelz.

## Implementation

The existing fixed-gap calibration report is preserved. A new independent
background stage, run after source/local-baseline verification, measures normal
and tangential clearance and compares fixed-gap with rotating-frame transfer.
No fit is selected merely because it has a smaller residual or enters 0..1.
Rigid rotation carries the reference gap with the triangle; this is an
approximation, not a cloth simulation, nor a collision/inside-outside oracle.

Inputs must have verified source identity, current graph membership, identical
actor context and matching foot topology. Stocking coordinates are moved to the
explicit reference NoHeel using the prior stage's certified local coefficient;
the small accepted reconstruction residual is retained and recorded.

A separate bounded tightening proposal moves only mapped, positive-normal-side
foot vertices towards the reference embedded foot. It preserves tangential
components, caps each step at 0.25 model units, targets at least 0.15 normal
clearance to its matched plane, and tapers over three mesh rings at the boundary.
Rim-like correspondences and negative-normal-side points are excluded. Triangle
flips reject the proposal. These numeric settings are illustrative preview
parameters, not a universal fabric thickness or an approved repair preset.

All results are report-only. There is no live vertex write, no NIF/OSP/OSD/TRI
rewrite, no body/foot rescale, no new actor-wide morph, and no correction applied
to shoes. UVs, topology, skin weights and the existing NoHeel implementation are
not changed. The preview does not certify global intersections, animation,
other body presets or compatibility with the shoe's outer shell.

## Outputs and test

Keep the existing BodySlide outputs, OBody preset and Converse manual 1.0.
Use the bundled config or add:

```json
"measureStockingClearance": true,
"writeStockingFitPreview": true
```

These depend on measureSurfaceCalibration and the existing reference/donor config.
Show Converse + CPB, then Glass or Gala + CPB; wait a few seconds for each.
No barefoot test is needed. The original appearance remains unchanged by this
new stage: preview files are not read as applied configuration.

Zip the entire VanityUBEHeelAdapter output folder. In addition to geometry/ and
calibration-candidates.json, it may now contain:

- stocking-clearance.json: gap statistics and both pose estimates;
- fit-proposals/clearance-*.json: sparse, reference-context-specific tightening
  offsets, topology and source identity. These are NOT installable mods or
  ready-to-import universal BodySlide sliders.

A failed source reconstruction yields an explicit refusal, not a guessed
correction. To produce a proper editable CPB patch later, use its winning _0/_1
NIFs, TRI and BodySlide OSP/OSD so all relevant endpoints and body morphs can be
checked; do not replace a source NIF with an OBody-morphed runtime snapshot.

The new core has 110 deterministic checks (finite data, topology, gap components,
translation and rotation, seam taper, capped motion, invalid inputs). It is also
exercised locally on the supplied real snapshots without publishing their assets.
Full DLL CI and actual in-game export remain separate validation steps.
