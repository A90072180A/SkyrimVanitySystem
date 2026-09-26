# 0.18.0: indexed offline height library

Release artifacts are built by the repository Windows workflow. Match BUILD.json
to the exact successful commit before installation. Standalone/local test success
does not replace the complete SKSE DLL build or in-game visual validation.

## Purpose

Keep the small `.user.json` overlay for individual overrides/ignores. Bulk
suggestions no longer expand into its 2048 `pairs` entries. A separate explicitly
published library can contain at most 200,000 memberships, 4096 shoes and 20,000
members per shoe. These are bounded implementation limits, not engine limits.
The existing 20,000 calculations per report remains unchanged.

Priority: explicit user pair/ignore -> existing manual/signed/legacy shoe rule ->
applicable offline library member -> existing measured automatic plan. Item
ignores still prevent targeting. Barefoot remains the verified separate branch.
Unknown/missing/invalid geometry is not treated as barefoot.

## On-demand files and cache

The exporter writes under the actual user-overlay parent resolved by the existing
editor: `SKSE/Plugins/VanityUBEHeelAdapter/height-library/`.

- `index.vhi`: small versioned index of shoes, content-addressed shard filenames,
  generation and explicit stocking identities. Loaded on a background worker.
- `<sha256>.vhs`: one immutable shoe file containing a shared controls table,
  explicit stocking ARMO/ARMA/model members, source fingerprints, original values,
  original residual and warning/approximation flags.
- `receipt-<generation>.json`: human-readable export audit. The current receipt
  is also written to `offline/offline-library-receipt.json`.

The worker loads only a requested shoe shard, keeps 8 recent shoe files and up to
256 resolved queries. The main thread sends copied IDs/paths/context, not game
pointers, and does no file I/O. Repeated lookups use memory. Pending/invalid
members do not get substituted with a different shoe's old value. Source
validation uses the NIF/TRI bytes and paths; identical bytes in a different
physical provider are equivalent at runtime. Export itself still checks the
reported resource/provider and active plugin inputs.

An index is published only after all new shoe files have been written. A
compare-and-swap under `publish.lock` prevents concurrent writers from silently
losing each other's changes. Interrupted exports may leave unused immutable
files, not a half-written active index. Old files are deliberately retained; no
background destructive cleanup runs. If a crash leaves `publish.lock`, close all
export processes before manually removing that lock. Do not delete it while
another exporter is running.

`set VHA_Reload to 1` rereads the existing configuration and resets library index,
resolved-value and asset caches even when `.user.json` is unchanged. No native
full-library parse is performed for each equip event. A save/load or new game
also invalidates requests from the preceding session. Changing files while the
game is open requires this explicit reload; NIF/TRI rebuilds should be done with
the game closed so that actual in-game 3D is recreated too.

## Residual warnings

The default UI includes `review-residual` when saving selected applicable
results. This no longer claims a visual check by the user. The original error is
stored and returned as a warning. Unsupported topology, ambiguous references,
missing assets, non-finite values and range saturation remain excluded; this is
not a blanket override of all failures. NoHeel stays 0..1; Heel stays within the
configured maximum (default 2).

`applyHeightResidualWarnings` defaults to true in the native controller, applying
otherwise-valid measured high-residual profiles with a warning as well. Set it
to false in user settings for strict behavior. `useOfflineHeightLibrary` defaults
to true; false disables this layer, not user overrides or the barefoot branch.
Existing explicit manual values remain authoritative. Runtime-state records
original residuals, whether the applied value is a shared approximation and the
library generation. The residual at an edited/shared approximate value is null,
not falsely reported as the original fit error.

## Shared-value application

The group preview now has **保存选中共享组到高度库**. Selected group membership is
explicit, linked to the exact candidate SHA256, and recomputed/validated before
publication. Exact groups share one value without loss. Approximate groups use
the chosen representative and preserve every member's original values and an
approximation flag. Unselected groups and unknown future stockings are not
included. Single-pair `.user.json` exceptions continue to win.

Saving ordinary candidates also deduplicates exactly equal values per shoe, but
does not round nearly equal coefficients. The original candidate report is not
rewritten by a group export. Members already in the library are updated only if
explicitly selected; unrelated shoe/member entries remain.

## Reuse existing calculations

**载入已计算结果** accepts a saved offline6 candidate report, including its compact
schema. It allows export without running the original geometry scan again.
Export still checks current input and asset fingerprints in the MO2 environment.
**载入旧清单（仅筛选预览）** remains a different, read-only operation.

Old Python/API `apply_report` is preserved for <=2048 legacy user-pair edits. The
main UI's new bulk action is **保存所选到高度库**. Never remove the old DLL's range or
capacity checks merely to make a Python write appear successful.

## Current boundaries

- Player-only/female/UBE/current visible ARMO+ARMA+model matching remains.
- Source weight must agree with actor weight within 0.001. Multiple source-weight
  libraries for the same shoe are not merged; use a separate MO2 profile.
- These are explicit fixed *offline* suggestions, not live OBody calibration.
  A changed morph context gets a distinct query cache key but no new geometry fit.
- Current bulk source checking supports loose NIF/TRI. Archive-only pair sources
  are explicitly skipped in export. Existing parser BSA support is not falsely
  presented as native archive-loading support.
- Fingerprints in binary files use FNV-1a64 for integrity/cache validation (not
  cryptographic authenticity). Shard filenames and audit reports use SHA256.
- Binary strings/counts/references/numeric ranges are bounded and checked. No
  pickle, dynamic code, arbitrary file path or runtime FormID is accepted.
- Existing SVS rendering, scoped RaceMenu application and stocking tightness are
  not changed. General visual clipping remains outside this update.

## After Windows build and installation

Install the resulting new DLL and complete tools folder, keeping your existing
base and `.user.json` files. Keep the Commands ESP enabled. A source-only archive is not an MO2 installation package; use the built DLL
artifact/update package instead.

1. Through MO2, launch the updated tool. Load your saved `offline-candidates.json`;
   select applicable rows and export. An existing 2-pair user file remains 2 pairs;
   the large batch is counted in `offline-library-receipt.json` instead.
2. In the group window, select a small known group (e.g. Glass) and explicitly
   save it. Check preserved manual-pair count, original values and approximate
   flags. There is no automatic wildcard enrollment.
3. In-game select a library-only pair, not the CPB+Glass pair already manually
   overridden. Run `set VHA_Reload to 1`. Look for authority `offline-library` or
   `offline-library-residual-warning` and inspect both controls and the actual
   picture. Switch away/back; `offlineLibraryIO` counters should not repeatedly
   reread the same shoe until reset/eviction.
4. Verify CPB+Glass still uses manual Heel=1.1; barefoot still NoHeel=1/Heel=0.
   Re-export a changed library-only value and issue reload without restarting;
   verify both generation/state and actual picture, not only `submitted=true`.

Send current runtime-state, height-events, the library receipt and log for any
failure. No new full raw geometry capture is required by these changes.
