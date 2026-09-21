# 0.9.0 — read-only active foot snapshots

This phase adds real coordinate/index export. It does NOT estimate or apply a geometry-derived posture. Keep the validated manual Converse profile. The SVS GUI/render source is unchanged.

## Test

Install the new DLL and retain your existing config, adding `"exportFootGeometry": true` (the bundled config already contains it). Do not rebuild BodySlide or change your OBody preset for this test.

1. Display Converse once through SVS, then wait a few seconds for the file-write log.
2. Temporarily unequip the real slot-37 item so SVS footwear disappears and the character's bare feet attach. Wait a few seconds, then restore it. This is a reference *observation*, not a certified flat/heel reference endpoint.
3. Optionally display one UBE high-heel pair with a separate `Feet` mesh. No manual posture entry is required to capture it. No geometry-derived value is applied.
4. Send the full adapter log, the ordinary generated JSON, and a ZIP of the new `SKSE/Plugins/VanityUBEHeelAdapter/geometry` output directory.

MO2 may redirect newly created files into Overwrite or a configured output mod. The relative in-game output is:

`Data/SKSE/Plugins/VanityUBEHeelAdapter/geometry/foot-<capture-key>.json`

No files are created there until an eligible active foot is observed. Setting `exportFootGeometry=false` disables this new exporter independently of the older geometry diagnostics.

## What the snapshot means

The exporter reads only `BipedAnim::objects[7]` (active slot 37), never `bufferedObjects`. It records that actual part's stable ARMO and ARMA identifiers, declared ARMA model, BODYTRI paths, sex, race and body weight. The current exact geometry name filter is `Feet`.

Only one full, non-strip skin partition is currently supported. The encoded stride, position layout, vertex count, GPU allocation capacity (GetDesc only; no readback), and triangle indices must agree. An existing vertex map must be identity. Unknown layouts, multi-partition meshes, unavailable CPU data, nonidentity maps and invalid buffers produce an explicit status with empty coordinate arrays, not guessed geometry.

`extraction.status=complete` means validated arrays were copied and decoded; it does not establish that the foot matches UBE reference topology or is a neutral unmorphed mesh. Shape-reported counts and skin-partition counts remain distinct. Model-space positions and local/bind transforms are preserved. World-space animated foot placement is not used as a heel estimate.

The file contains ordered triangle and position fingerprints, the current RaceMenu morph values, and a morph-state fingerprint. Fingerprints are diagnostic FNV1a64 values, not security hashes. Topology-hash equality alone is not proof of semantic vertex correspondence. Future fitting must also validate reference identity, coordinate transforms, neutral shape/morph state and slider direction.

All engine access and copying occurs in SKSE tasks. The background writer receives owned JSON values only, uses a bounded queue and deduplication, and replaces a snapshot through a flushed temporary file with MoveFileExW. It never overwrites `VanityUBEHeelAdapter.json` or promotes the known manual `1.0` into an estimated result.

The exporter is runtime-gated to Skyrim AE 1.6.1170. `geometryFit` stays `not-run` and `posture` stays null. There is no vertex mutation, GPU staging/map operation, topology remapping or actor-wide morph refresh in the exporter.

## Verification

`FootSnapshotCoreTests.cpp` exercises float32 and float16 decoding, truncation, zero/oversized counts, invalid stride/type, out-of-range indices, NaN/infinity rejection, degenerate index lists and separate ordered-topology/position hashes. These tests run on GCC with ASan/UBSan and on MSVC before packaging. They cannot replace an in-game test of the runtime buffer layout.

The previous generated JSON remains a metadata report. These heavier snapshot files are separate. BodySlide disk-index caching is not implemented by this phase; its existing background scan remains unchanged.
