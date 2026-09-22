# 0.16.0 — asymmetric height limits, live configuration and reviewed profiles

## Scope

Height only. No tightening, body rescale, NIF/TRI/OSD rewrite or SVS GUI changes.
The SVS build13 render fix remains untouched. No new game or BodySlide rebuild.
This is an integrated update; no per-stage geometry collection is required.

`NoHeel` is strictly 0..1. `Heel` is 0..`heelMax` (default 2.0, adjustable 1..10).
The latter is a numerical guard, not proof that an author's mesh looks good at 2.
Only one positive branch is used, and both keys are submitted in ONE scoped
transaction. Above-one Heel values are retained, not clipped to 1. NoHeel >1 is
always rejected. Invalid configuration keeps the last good configuration.

## What is newly measured

An exact ordered component-subset matcher handles a narrow topology difference:
deleting whole small disconnected islands while retaining the vertex/face order
of the common surface. It never matches by proximity, accepts no more than 10%
vertex loss, requires a unique correspondence, and rejects ambiguous/reordered
or decimated topology. Unmatched vertices do NOT participate in the fit.
On the supplied snapshots Agata is exactly the two main foot components with
700 vertices in ten islands removed. This now permits a height measurement.
It does not certify the final rendered fit or support arbitrary different feet.

Replayed through the same C++ core using heelMax=2:
- Converse: NoHeel=1, Heel=0; this is the reference self-check.
- Glass: NoHeel=0, Heel≈1.106766; normalized residual≈0.195159.
- Agata: NoHeel=0, Heel≈0.491227; normalized residual≈0.167224.
The latter two still exceed the unchanged default residual limit 0.15. They are
persisted as measured candidates, NOT silently presented as successful automatic
fits. A user may deliberately review/approve a source-bound candidate or set a
manual value, without globally loosening automatic acceptance. Mesh clearance,
shoe-shell collisions and animations remain separate from this pose estimate.

## Files and hot reload

Base: `Data/SKSE/Plugins/VanityUBEHeelAdapter.json`
User overlay: `Data/SKSE/Plugins/VanityUBEHeelAdapter.user.json` (optional)

Both are checked in the background every 500 ms. Two equal, valid effective
readings are required before queueing a main-thread update. Expect roughly 1–3
seconds while the game is running, not a guaranteed scheduler deadline. No save
reload is required. Partial/invalid JSON, out-of-range values and duplicate pairs
are rejected; the old configuration and its controls remain. A successful change
restores old managed shapes, invalidates old tasks/cache context, and reevaluates
current shapes. All consumers use the same accepted configuration snapshot.
Changing OBody context schedules a new internal measurement.

User file example (explicit manual Glass height, NOT an automatically validated
installation preset; start with the empty example shipped in `examples/`):

```json
{
  "schema": 1,
  "settings": { "heelMax": 2.0 },
  "items": [],
  "pairs": [
    {
      "stocking": "[Caenarvon] Cosplay Basics.esp|00000D1A",
      "footwear": "Glass high-heeled shoes.esp|00000801",
      "mode": "manual", "NoHeel": 0.0, "Heel": 1.1068,
      "note": "Manual review of measured height; clearance not repaired"
    }
  ]
}
```

`items` rows use `armor`, `kind` (stocking/footwear/ignore), optional `note`,
and optionally `addons` (stable ARMA IDs for an explicitly registered donor).
Marking something a stocking does not manufacture missing NoHeel/TRI data.
`pairs` support manual values, `mode:"ignore"`, and optional source-bound approval
records written by the editor. Removing a pair returns control to the existing
manual reference or automatic rules. `<barefoot>` is supported only AFTER the
runtime positively confirms naked feet; it cannot turn unknown shoes into bare
feet. Explicit user pairs precede old `signedHeightOverrides`, old `heels`, and
measured profiles. Ignore/manual classifications never edit the game plugins.

Generated outputs under `Data/SKSE/Plugins/VanityUBEHeelAdapter/`:
- `height-profiles.json`: actual raw/limited branch values, source hashes, context,
  mapping method, residual and acceptance decision. Algorithm v2 invalidates old
  range-v1 caches. Reuse requires current source/context validation.
- `calibration-candidates.json`: full measurement evidence.
- `runtime-state.json`: currently declared/matched visuals, controls, authority,
  submission status, refusal reason and accepted configuration revision.
- `height-events.json`: last 64 coalesced control-state reports in this session.
  This is not a per-frame vertex audit or a guarantee every intermediate event
  was recorded. `submitted` means API submission, not visual proof.
- `observed-items.json`: bounded persistent inventory of encountered stable IDs,
  addons, model/BODYTRI and classifications. Historical entries are not live
  matching authority and are never automatically equipped.

Manual data is never written into generated files by the editor. Generated data
never overwrites the main/user configuration. Atomic output uses temp + replace.
Hot reload concerns JSON policy, not live replacement of mesh/TRI assets.

## Local editor

`tools/height_editor.py` uses Python 3.10+ and only the standard library. Optional
GUI requires tkinter (normally present with python.org Windows installations).
It is a separate local window, not an in-game/MCM menu. The plugin itself does not
require Python. Direct JSON editing works without it.

Point `--plugins` to the real MO2 output directory containing the generated folder,
usually `.../MO2/overwrite/SKSE/Plugins`, NOT an unrelated physical Skyrim/Data.
If MO2 routes new files to a dedicated output mod, use that mod's SKSE/Plugins.
The user overlay created beside that folder must win in MO2's virtual Data.
`--user-file` can select another explicit winning overlay; don't keep two competing
copies. The package deliberately does not install an empty overlay over your work.

```
python height_editor.py --plugins "D:/MO2/overwrite/SKSE/Plugins" gui
python height_editor.py --plugins "D:/MO2/overwrite/SKSE/Plugins" status
python height_editor.py --plugins "D:/MO2/overwrite/SKSE/Plugins" candidates
```

The GUI lists observed equipment and measured candidates. Select the actual UBE
addon when registering a stocking. Manual pair fields always set BOTH controls.
Approving a measured candidate binds it to its source/context fingerprints and
reference configuration; changed sources or body context invalidate approval.
Choosing a manual numeric override without approval is an intentional fixed
per-pair instruction, not portable mathematical calibration. Saves use a .bak
and atomic replacement; a concurrent edit is rejected rather than overwritten.

## One integrated acceptance session

1. Keep Automatic Stocking disabled, existing body builds/OBody/SVS unchanged.
   Update Adapter DLL and base config. Keep any user overlay.
2. Show CPB+Converse: NoHeel=1/Heel=0. Remove the actual shoes while CPB remains
   visible: confirmed barefoot should remain flat after the stable-state check.
3. Show CPB+Glass and CPB+Agata once to refresh v2 candidates. Default automatic
   policy may reject the above residuals; the runtime report must explain why.
4. In the editor review/approve a candidate OR deliberately set Glass to
   NoHeel=0, Heel=1.1. Save while the game is open. Verify `[config reload] accepted`
   then `[height apply]` from the user authority and visible change. Return to
   Converse and back; try one real equipment swap and one save/load.
5. Set NoHeel to 1.1 in JSON as a negative test: it must be rejected and the last
   good config retained (GUI itself rejects before writing). Restore valid JSON.
6. Ignore the pair, then remove the override; it should respectively restore the
   ordinary actor baseline and return to the original decision policy. Ignore
   is NOT equivalent to making a stocking flat.

Return only the log plus runtime-state.json, height-profiles.json and
calibration-candidates.json if something fails. Raw geometry export remains off;
no need to redo the previous microscopic acquisition procedure. Windows DLL build,
portable/real I/O tests and offline snapshot replay do not substitute for actual
Skyrim visual/animation validation. Do not report default Glass/Agata as fixed
until the selected policy and visual result have really been observed.
