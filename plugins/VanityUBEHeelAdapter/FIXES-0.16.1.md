# 0.16.1 — backing-file reload receipts and positive barefoot identity

This bugfix changes the height controller and external editor, not the SVS DLL,
GUI/render, meshes, body presets, native morph ranges, or fit thresholds.
Keep the user-validated Glass manual NoHeel=0 / Heel=1.1 value. NoHeel remains
strictly [0,1]; Heel remains [0,heelMax]. No tightening is performed.

## Evidence from the supplied 0.16.0 run

Glass was submitted successfully with the explicit user pair Heel=1.1. The log
has no configuration-reload acceptance/rejection during the run; the returned
runtime state remains at configurationRevision=2. The ZIP contains no base/user
configuration or physical path receipt, so it does NOT prove exactly which file
was edited or why that path was not seen. The ordinary-filesystem tests did not
cover a newly-created overlay outside an already-running MO2 virtual mapping.

Every reported barefoot attempt stops at real-footwear-still-worn even though the
active slot-37 skin ARMA points to the known UBE femalefeet model. Previously any
non-null GetWornArmor result was an unconditional veto; the returned form ID was
not logged. A non-null result alone is not sufficient evidence of a visible shoe.

## Barefoot correction

The final decision now requires affirmative current skin evidence: a valid live
SVS snapshot with no declared or matched foot visual, the non-buffered active
skin part, an ARMA owned by that skin, an exact known naked-foot model, and live
skinned geometry. A 250-ms settling window still applies. GetWornArmor's returned
ID and whether it equals the skin are diagnostic metadata, not an independent
veto over this fully verified visible skin state. A physically equipped item
hidden behind this positively verified naked-foot presentation can therefore
coexist with the barefoot decision; mere absence of a resolved shoe never can.

The same scoped transaction applies NoHeel=1 / Heel=0 to the visible stocking
only. Missing/unknown skin, buffered parts, ambiguous visuals, advertised shoes
with no BODYTRI, and rebuilding empty slots remain unconfirmed. The new
runtime-state.json barefootEvidence fields explain every guard. No undocumented
metadata is inserted into game nodes. No global morph key is retained.

## Hot reload correction

At first configuration access, Windows resolves the actual backing file path
from a freshly opened handle (GetFinalPathNameByHandleW). Existing winning base
and user paths are pinned for this process. If the optional user file is absent,
the reader watches its physical sibling beside the actual generated output
folder, rather than waiting for a new virtual-file mapping to appear. A temporary
output probe is used only if no runtime report exists; it never overwrites a
configuration and is removed immediately. Paths do not follow later CWD changes.

Polling still uses bounded complete reads and fresh handles across atomic editor
replacements. Content is compared even if size/mtime are unchanged. Two stable
valid observations precede a main-thread update. A malformed, partly written or
now-missing previously present overlay preserves the last good configuration;
write {} to intentionally clear it. Pending delivery is retried until the main
thread acknowledges; invalid input withdraws undelivered pending content.

New output: SKSE/Plugins/VanityUBEHeelAdapter/configuration-status.json
- actual base/user/output paths and their virtual names;
- current-process token and watcher heartbeat;
- observed source byte fingerprints and effective-config fingerprint;
- main-thread accepted revision and source fingerprints, or pending/error state.
The fingerprints are FNV-1a64 diagnostic identities, not security signatures.
Configuration accepted does NOT itself prove the geometry changed.

The updated tools/height_editor.py follows the advertised user backing path.
It refuses a live --user-file pointing elsewhere, displays the actual file, and
reports saved/pending separately from a fresh acknowledgement of those exact
bytes in the SAME game process. A restart acceptance is labelled separately.
The previous unconditional 'saved, reload in 1-3 seconds' success popup is removed.
Editing JSON directly at the reported physical path is still supported.

Mod priority changes, moving the pinned source mod, or live replacement of NIF/TRI
files are not configuration hot reload. Restart after those operations. These
paths never change the MO2 mod order and never copy the user overlay into a second
mod. Use the new editor bundled with the new DLL, not an old loose script.

## Installation and one combined acceptance check

Update only the Adapter DLL and tools/height_editor.py. Retain the existing base
configuration and, especially, VanityUBEHeelAdapter.user.json with Glass=1.1.
Do not rebuild BodySlide, change OBody or replace your build13-compatible SVS.

Launch once with the new DLL. Point the editor --plugins to the actual generated
output folder's parent (usually MO2 overwrite/SKSE/Plugins). It displays the
physical user file selected by the running plugin. Do not maintain another copy.

1. Keep CPB+Glass visible. Save Heel=0.9, NoHeel=0, return focus to the game and
   let it run for a few seconds. The editor must show same-process acceptance;
   confirm the foot posture changes. Then restore the verified Heel=1.1.
   A paused/background-suspended game cannot execute main-thread tasks: pending
   is appropriate until gameplay resumes. No save/load or process restart.
2. With CPB still visible, remove the footwear so the known UBE naked feet are
   actually displayed. Expect confirmed-barefoot-flat and NoHeel=1 / Heel=0.
   Re-equip Glass; the retained manual Heel=1.1 must return. Unknown shoes must
   not be mistaken for barefoot.

If either fails, send the log, configuration-status.json, runtime-state.json and
height-events.json. No new raw geometry capture is needed. The receipt separates
wrong path, absent heartbeat, rejected input, queued update and accepted policy.

## Verification boundary

Tests run the real threaded configuration worker on ordinary files AND on an
explicit split virtual/backing-path fixture: creating an optional overlay after
startup, atomic same-size/same-mtime replacement, CWD changes, invalid ranges,
malformed/deleted files, acknowledgement before/after delivery, and recovery.
Native Windows handle path resolution is tested on Unicode paths. Editor tests
cover fresh/stale receipts, exact content identity, wrong-file refusal, and
acceptance only after restart. Barefoot policy is exhaustively tested with both
non-null and null worn-query metadata, including all other failed guards.

These tests are not usvfs injection or Skyrim gameplay. The actual MO2 mapping
and rendered barefoot result remain the two integrated acceptance items above.
The measurement algorithm/report schema is unchanged from 0.16.0; older
measurement-generator labels may remain in cached height/geometry evidence.
