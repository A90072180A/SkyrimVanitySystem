# 0.10.0 - live bare-foot capture and real NoHeel basis

This phase does not apply geometry-estimated posture. Keep the validated manual Converse value 1.0 and the existing OBody/BodySlide build. The SVS GUI/render source and the scoped stocking morph implementation are unchanged.

## Why this phase exists

0.9.0 can export valid shoe coordinates, but a bare-foot attachment can be missed when active biped slot 37 does not provide the complete clone/item/addon tuple. The new foot-specific attachment observer retains the event identity and accepts its exact Feet geometry only if it is still in the CURRENT player scenegraph or active biped trees. Buffered parts are not searched. This fallback needs an in-game test; it is not an unconditional acceptance of cached attachments.

We also need the actual NoHeel unit displacement, not an arbitrary pair of shoe shapes labelled heel/flat. The snapshot writer now parses the exact Feet/NoHeel position record from the actual BODYTRI files and from an optional referenceBodyTri resource. It records sparse offsets, source file fingerprint, available shape/morph names and explicit failure reasons.

## Install and test

Install the DLL and use the bundled config, or add these to the config that wins MO2 conflicts:

```json
"exportFootGeometry": true,
"referenceBodyTri": "!UBE\\Feet\\femalefeet_tangent.tri"
```

That reference resource path came from the tested UBE bare-foot attachment. An empty string disables the additional reference read. Do not change heel values or rebuild meshes for this test.

1. Display Converse through SVS and wait a few seconds.
2. Unequip the real footwear so bare feet are visible. Wait at least three seconds before re-equipping. This repeats the previous correctly performed test to check the collector fix.
3. Display a UBE Glass or Gala heel once, then wait a few seconds. A matching Feet geometry is required. No additional manual heel entry is needed for capture.
4. Send the complete adapter log and a ZIP of the geometry directory. Existing 0.9 snapshots may stay there; new files have generatorVersion 0.10.0 and schema 2.

Output remains `Data/SKSE/Plugins/VanityUBEHeelAdapter/geometry/foot-<key>.json`. MO2 may place new files in Overwrite or a configured output mod.

Expected new messages include `[foot snapshot] ... selection=live-attachment` and `[NoHeel basis] ... status=present ...`. A successful reference may instead report a precise unsupported/absent/read-error status; please send it unchanged.

## Interpretation and limits

- `extraction.status=complete` means checked coordinate/index arrays were decoded, not that they are neutral or semantically matched to UBE reference vertices.
- `triMorphData` describes the rendered part's BODYTRI resources. `referenceTriBasis` is a separate optional measurement, not proof of matching reference topology or correct morph direction.
- `present`, `morph-absent`, `shape-not-found`, `empty-morph`, `malformed`, `unsupported-format` and resource I/O errors are distinct. Only packed BodySlide BODYTRI (on-disk PIRT) is supported in this phase. Do not interpret an unreadable or unsupported file as a missing morph.
- TRI reading happens on the writer thread and supports MO2-visible loose files. BSA-only resources are reported unreadable; there is no engine resource access from the worker.
- The portable least-squares kernel returns a raw, unclamped candidate and residual. It is tested but deliberately not connected to gameplay until a valid basis and reference correspondence have been measured.
- FNV fingerprints are diagnostic, not cryptographic. Exact ordered triangles alone are insufficient to certify semantic correspondence. Actor morph values and transforms must also be checked.
- No runtime vertex mutation, actor-wide morph update, automatic profile promotion, topology remapping, GPU readback or SVS GUI change is introduced by the exporter.
- BodySlide disk-index caching is still not implemented. The existing background index remains separate from the snapshot files.

The included `tools/audit_foot_snapshots.py` (Python 3.10+, standard library only) can independently verify snapshot hashes, bounds, indices and pairwise ordered topology without modifying any game configuration.

CI retains the previous decoder tests and adds packed TRI parser, malformed-input and raw least-squares tests on GCC/ASan/UBSan and MSVC. These tests do not replace verification of live attachments and real TRI resources in game.
