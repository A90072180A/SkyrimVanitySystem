# 0.13.0 - stocking-specific surface calibration, report only

This release connects measured shoe geometry to a particular stocking's actual
NoHeel displacement. It does NOT apply automatic values. The SVS GUI/render,
attachment target selection, RaceMenu local application and equipment reapply
code are unchanged. Keep the validated Converse manual value 1.0.

## Evidence and limitations

The supplied 0.12 test includes one successful NoHeel=1 call. Two source-verified
CPB snapshots differ by 1.0000000004 times their 3368-entry NoHeel delta, with
per-vertex RMS residual approximately 1.57e-7 model units. This verifies a relative
one-unit local change; actor-wide morph keys alone do not establish a local state.
Converse and Glass native foot fits also ran. These are measured facts from that
test, not a guarantee about all future attachment timing or all outfits.

An offline surface-correspondence experiment, anchored to Converse + CPB at
NoHeel=1, gave a raw CPB NoHeel candidate of about -0.7385 for Glass. It is OUTSIDE
the currently accepted 0..1 interval and is NOT a value to install. This experiment
uses a constant reference cloth-gap vector, not a physically exact cloth solver;
residuals and correspondence errors are material. In particular,
NoHeel=1-HiHeelz is not used or justified.

An older Gala snapshot was also passed through the numerical kernel as an
exploratory calculation, but its actor-morph context differs from this reference.
It is excluded from calibration evidence. The background integration requires
full context equality and will not pair it with this anchor. Test Gala again in
the same session/context to obtain a valid new comparison.

## New background processing

1. Require source-verified active geometry and identical full actor-morph context,
   race, sex, weight, local transform and bind transform when pairing measurements.
2. Reconstruct a stocking's baseline from its actual winning _0/_1 NIF pair and
   that stocking's OWN TRI responses. Estimate the observed local NoHeel and
   reject a baseline with excessive all-vertex/affected-vertex residual. It does
   not treat an arbitrary captured stocking as an undeformed endpoint.
3. Construct the stocking's reference state at the explicit reference NoHeel.
   Map its materially affected vertices to exact closest triangles on the manual
   reference shoe's embedded foot using a bounded BVH. Preserve barycentric
   correspondences and the measured reference gap; do not equate vertex IDs
   across the stocking and foot topologies.
4. Transfer a new shoe foot's displacement through those correspondences and fit
   the scalar NoHeel shift. A target foot must have the same ordered topology as
   the reference foot. Keep raw estimates, gaps, coverage, RMS/max residuals and
   rejection reasons. Values outside 0..1 are not clamped or applied.
5. Measure the stocking's exact native Heel record separately for later analysis.
   No semantic alias, inverse, extrapolation or application of Heel is enabled.

The manual reference pairing is calibration input. Returning 1 for that same
Converse anchor is a self-check, NOT independent proof of automatic accuracy.
Persistent outputs remain reports, not loaded/applied configuration authority.
No UBE or user mesh assets are redistributed. All new I/O, pairing and fitting
runs on the existing snapshot writer, without game-object access.

## Install/test

Use the bundled config, or keep your configuration and add:

```json
"measureSurfaceCalibration": true,
"surfaceCalibrationReference": {
  "armor": "[AFxII] Converse AS.esp|0000080A",
  "addon": "[AFxII] Converse AS.esp|00000809",
  "noHeel": 1.0
}
```

Keep exportFootGeometry/exportStockingCalibration enabled and the existing donor
ARMO/ARMA allowlist. Do not rebuild BodySlide, change OBody or add guessed Glass
values. No barefoot capture is needed.

Show Converse + CPB first, then Glass or Gala + CPB. Wait for the new
[stocking local calibration] and [surface posture] log lines; first-time TRI
reconstruction can take several seconds in the background. A rejected baseline
is a useful explicit result; do not weaken its thresholds to force a candidate.

Send the full adapter log, geometry directory and the NEW sibling file:

`Data/SKSE/Plugins/VanityUBEHeelAdapter/calibration-candidates.json`

MO2 may redirect outputs to Overwrite or a configured output mod. Zip the whole
VanityUBEHeelAdapter output folder rather than only its geometry subdirectory.
Snapshot files now have schema 5 / generatorVersion 0.13.0 and include
stockingLocalCalibration/surfaceCalibration as applicable. The raw collector is
unchanged, so captureGeneratorVersion remains its earlier version.

## Tests

SurfacePostureCoreTests has 53 checks: triangle interiors/edges/degeneracy, exact
BVH versus exhaustive search, positive/negative scalar fits, residual rejection,
invalid coordinates/indices/weights, topology mismatch and singular donor data.
It runs with GCC ASan/UBSan and MSVC alongside the existing tests. The portable
core was also exercised locally on the supplied snapshots. Full source-baseline
reconstruction and in-game scheduling still require the installation test.
