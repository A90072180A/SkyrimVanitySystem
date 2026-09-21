# 0.12.0 — file-backed reference and measured native foot coefficients

No automatic NoHeel mapping is introduced. SVS GUI/render code, target selection,
RaceMenu morph application and equipment reapply code are unchanged. This release
changes background evidence processing, not scene geometry.

## Findings from the supplied files

The 0.11 archive contains no successful local morph application and still lacks a
bare-foot coordinate snapshot. Some snapshots labelled CPB stocking actually use
the DemonArmor boots ARMA; they are not valid stocking calibration donors. Other
CPB snapshots have a renamed runtime shape and no visible BODYTRI metadata.

The separately supplied UBE source NIF provides a usable 8837-vertex, 16200-triangle
reference. Its ordered topology matches Converse/Glass/Gala. Source OSD analysis
supports native HiHeelz_CBBE coefficients near 0, 1 and 0.6 respectively after
subtracting the recorded body morph responses; these are NOT NoHeel values.
Witchy's reduced topology can be explained by the supplied ToeNailsZap mask, but
runtime remapping is deliberately not implemented in this release.

## Implementation

- Read the actual declared ARMA model NIF on the existing writer thread. Support
  only SSE 20.2.0.7/user12/BS100, BSTriShape and one complete skinned partition.
  Resolve original shape names and BODYTRI links from file records, not basenames.
- Require a unique exact ordered-topology and local/bind-transform match with the
  observed mesh before using its source metadata. Record the source fingerprint.
- Require explicit ARMO/ARMA donor pairs before interpreting a stocking sample.
  The bundled allowlist contains the tested CPB/UBE-patch pair only. Rejected
  samples are quarantined as rejected-calibration-source and are not fitted.
  This is a calibration-donor safeguard, NOT a requirement to patch all stockings.
- Fit two distinct native coefficients using configured low/high reference NIFs,
  their actual reference BODYTRI, current actor weight and single-key body morph
  values. Multiple effective keys and unsupported/mismatched data are rejected.
- Save raw coefficients, RMS/max residual and all source evidence in each geometry
  JSON. Keep mappingToNoHeel=null and automaticApplicationAllowed=false. No
  1-HiHeelz shortcut, zero-clamping or fabricated confidence is used.
- Source and TRI I/O remains background-only with bounded session caches. The
  BodySlide persistent disk index and applied automatic-profile cache are still
  separate, unimplemented features. No UBE asset is distributed in this repository.

## Install and test

Keep existing BodySlide outputs, OBody preset and Converse manual 1.0. Keep the
export options enabled. Use the bundled config or add:

```json
"referenceFeetModels": [
  "!UBE\\Feet\\femalefeet_tangent_0.nif",
  "!UBE\\Feet\\femalefeet_tangent_1.nif"
],
"calibrationDonorAddons": {
  "[Caenarvon] Cosplay Basics.esp|00000D1A": [
    "[Caenarvon] Cosplay Basics UBE patch.esp|0000093B"
  ]
}
```

Show Converse + CPB once, then Glass or Gala + CPB once, waiting about three seconds
per combination. **No more barefoot capture is required for this test.** Do not
copy the supplied reference assets over your live meshes merely to run this test.
The code reads the paths already present in your virtual Data directory.

Expected new logs: [source geometry] with the original source shape/BODYTRI and
[native foot fit] with two raw coefficients and rms. New final JSON files have
schema=4, generatorVersion=0.12.0; captureGeneratorVersion records the unchanged
raw capture component. Files may reuse an existing capture-key filename and are
atomically replaced by the writer. Do not mix old analysis labels with new files.

Send the log and geometry folder ZIP. Unknown or unreadable NIFs produce explicit
sourceModelEvidence statuses; a read failure is not proof of a missing slider.
This release does not repair missing runtime BODYTRI or declare the old local
morph path revalidated. Those game-side changes require source-matched evidence.

## Tests

NifSourceCoreTests covers the synthetic source topology/BODYTRI reader, every
truncated prefix of the fixture, invalid header/trailer, and two-basis fitting
with out-of-range coefficients, singular bases and invalid input. Tests run with
GCC/ASan/UBSan and MSVC alongside the previous suites. Real supplied UBE files were
also checked locally without publishing them. Offline parser success is not an
in-game validation of each user's winning NIF or a proof of NoHeel semantics.
