# 0.17.0 source update / offline tool 1

## Delivery and build verification

This source update is based on `eced8f03eb95ea64f1d82c7fe90c529c19e0756d`
and is integrated on `vanity-ube-heel-adapter-poc`. The build workflow tests the
C++ cores, actual configuration worker, Python editor and offline scanner on Linux
and Windows, then compiles the SKSE DLL and generates the command ESP. Consult
that commit's GitHub Actions result and the package's BUILD.json for the actual
build status; source publication alone does not establish a successful build.

The Python offline tool independently supports the existing 0.16.1 user-overlay
format. It requires Python 3.10+ and tkinter for the GUI. Skyrim does not need to
be running to scan, calculate recommendations or explicitly apply reviewed pairs.
For the console commands below, install both the newly built 0.17.0 DLL and the
command ESP. Keep existing base and user configuration, especially Glass=1.1.

## Explicit console reload (requires new DLL + generated command ESP)

The original ESL-flagged `VanityUBEHeelAdapter.Commands.esp` contains just one
float GLOB, `VHA_Reload` (local ID 0x800), and a Skyrim.esm master reference.
It does not contain armor, scripts, quests, textures or meshes.

```
set VHA_Reload to 1
```

Close the console and let gameplay run. The next main-thread poll consumes and
zeros the mailbox; the file worker rereads the pinned actual configuration files.
Two stable valid readings are required. The main thread accepts the configuration
UNCONDITIONALLY, even if its effective values are equal, resets the controlled
scope bookkeeping, and makes three bounded, spaced local reapply passes. Both
NoHeel and Heel are submitted transactionally; unrelated OBody keys remain.
The mailbox is cleared on game load so a saved trigger is not replayed.

```
set VHA_Reload to 2
```

Prints the native version/configuration revision. Other values are consumed and
ignored with help text. No ConsoleUtil / ConsoleNG / Papyrus extension is required.
`configuration-status.json.manualReload` distinguishes request, validated/pending,
accepted-reapply-requested, and rejection. Invalid JSON keeps the prior controls.
A submission is NOT evidence that the visible geometry changed. Automatic watching
remains; the command is a deterministic fallback, not a claim that the old log
failed to read: the supplied 0.16.1 log actually accepted revision 3 and submitted
Heel=0.9 during the same process.

The barefoot policy, SVS DLL/render fixes, native height limits and .15 automatic
residual limit are unchanged. No tightening or global model rebuild was added.

## Offline scanner — run through MO2, no Skyrim process

Extract the tools to a normal directory. Add an MO2 executable whose Binary is
64-bit Python's `python.exe`, not a terminal launched outside MO2. Arguments:

```
"D:\MO2\tools\VHAOffline\tools\vha_offline.py" gui
```

Start In: the Skyrim Special Edition installation directory. Use your actual
Python/tool locations. In the GUI select the game's virtual Data directory and
the exact active MO2 profile directory (contains plugins.txt and loadorder.txt).
Do not point it at the whole `mods` directory. Running outside MO2 sees physical
Data, not the profile's virtual winning files; missing resources then stay missing.

The scanner:
- reads the profile's starred enabled plugins plus official implicit masters;
- respects that order and requires every master to be active and earlier;
- resolves on-disk FormIDs through each plugin's own MAST list, retains stable
  origin-plugin/local IDs, and applies winning ARMO/ARMA overrides/deletions;
- follows female third-person ARMA MOD3 models, not inventory/world models;
- separates UBE and non-UBE alternatives and refuses ambiguous UBE pairs;
- validates packed BODYTRI shape names and real NoHeel/Heel sparse offsets;
- reads source SSE 20.2.0.7/user12/BS100 skinned single-partition BSTriShape data;
- reports missing/unsupported inline/multi-partition/ambiguous geometry rather
  than manufacturing a flat-foot value;
- handles exact topology and unique ordered deletion of small whole components,
  with the existing 10% vertex-loss/search limits (including the Agata nail case).

Coverage is an inventory of eligible active armor records, NOT a promise that
every Skyrim mesh format or every semantic stocking can be identified correctly.
Footwear is found through slot 37. Stocking candidates combine name/slot evidence
with real NoHeel capability; merely marking an item cannot create a missing morph.
NIF-only installations without active armor records are not guessed into FormIDs.
An optional `--race plugin.esp|XXXXXXXX` filters direct/additional race membership;
implicit race inheritance and engine-specific runtime selection are not emulated.

Loose virtual Data wins over archives. Plugin-associated `<plugin>.bsa` and
`<plugin> - Textures.bsa` are considered in plugin order. Named BSA versions
104 (zlib) and 105 (LZ4) are supported. Base/INI-loaded additional archives require
`--archive-list <text-file>` listing their Data filenames from low to high priority.
That list must reflect the active INI configuration; the tool never indiscriminately
loads all BSAs from inactive mods. Constructed compressed/uncompressed BSA fixtures
are tested; no claim is made of running against this user's actual archive set.

## Height model and review

Default flat reference: `[AFxII] Converse AS.esp|0000080A`. Select its exact
`armor::addon` key when needed. It must be a user-confirmed flat reference, not a
filename guess. The donor flat endpoint is NoHeel=1. This is an explicit convention
which can fail for unusual authors' sliders; candidates always need visual review.

Use already built NIFs. Select actor weight 0..100 (default 0). Weight-enabled
models require compatible _0/_1 files and the same BODYTRI; their positions are
interpolated. Optional command-line `--preset <XML> --preset-name <name>` adds that
preset's morph values using each mesh's own TRI. Do NOT add a preset twice to meshes
already built with it. A body preset containing nonzero height sliders is refused.

An offline process cannot read current actor/OBody/animation state. Its values are
source-model/preset recommendations, not live-body calibration. Native foot-pose
classification, when the reference feet basis is available, distinguishes flat-like
and raised foot poses, not a shoe's physical heel/platform height in centimetres.

Only a single nonzero branch is selected. NoHeel remains [0,1], Heel [0,heelMax]
(default 2, maximum 10). No universal Heel/NoHeel conversion ratio is assumed.
A nearest-triangle correspondence preserves the reference stocking-to-foot gap;
there is no width repair. Out-of-range and high-residual cases remain explicit.

GUI sequence: Scan -> optionally select stocking rows -> Calculate -> select
recommendations -> Apply selected. 'Select within limits' does not select rejected
candidates. You may deliberately allow reviewed high-residual candidates, or edit
one recommendation numerically; an edited row's displayed residual belongs to the
original mathematical suggestion, not an unperformed refit. Ambiguous or unparsed
pairs cannot be approved as scan results. Manual classification uses the same user
configuration and never edits an ESP.

Batch Apply is an explicit manual instruction, NOT an auto-approval forged into the
runtime measurement cache. It validates source hashes/providers and active-load-order
inputs, checks ranges, merges only absent pairs, preserves existing manual/ignored
pairs and legacy reference values, and saves the overlay atomically with a backup.
Existing fixed values (such as CPB+Glass Heel=1.1) are NOT overwritten by scanning.
Use the regular height_editor.py to edit an existing manual override.

After a later body/mesh/preset change, rescan/review fixed manual values; they are
not automatically invalidated like source-bound runtime approvals. All applied
values are traceable through the offline apply receipt and user pair notes.

## Files

By default, in virtual `Data/SKSE/Plugins/VanityUBEHeelAdapter/offline/`:
- offline-catalog.json: active equipment, selected models, capability/status.
- offline-candidates.json: values, residuals, reference assumptions, sources/hash.
- offline-apply-receipt.json: applied vs preserved indices and destination file.

User choices remain in `VanityUBEHeelAdapter.user.json`, never mixed into generated
height-profiles.json. The last actual path receipt is used where available. Do not
maintain multiple competing copies. Change mod priority / move source mods only
with the game closed. No source asset is written, no network access is performed.

## One combined test, after native build

1. Keep known CPB+Glass manual NoHeel=0/Heel=1.1. Save Heel=.9, enter
   `set VHA_Reload to 1`, close console, run 3–5 seconds. Check accepted revision,
   a fresh [height apply], and visual change; then restore 1.1 using the command.
   If accepted/submitted but visually unchanged, report that distinction.
2. Remove shoes with CPB still visible: retain the already fixed flat barefoot.
   Put Glass back: retain its manual value. No body rebuilding/new save required.
3. With Skyrim CLOSED, launch the scanner through the same MO2 profile. Scan and
   calculate CPB against Converse/Glass/Agata. Review rejected cases; never silently
   relax the .15 acceptance threshold. Apply a chosen new pair; an existing Glass
   manual entry must be reported preserved. Start the game and check that pair.
4. Negative check: NoHeel=1.1 or malformed JSON must not replace the last good
   native state. Source changes after scanning must cause Apply to request rescan.

For problems send the log, configuration-status.json, runtime-state.json,
height-events.json, and the three small offline reports. Raw geometry not needed.

## Verification and limitations

Portable native cores, the real threaded configuration worker, Python binary
readers/fixtures, GUI actions, and archived-snapshot numerical replay can be tested
locally. Those are not a Windows DLL build, usvfs injection, or Skyrim visual tests.
The supplied source includes CI staging and tests for the command ESP and scanner.
Keep this distinction when releasing/building the patched repository.

Format references: TES5Edit/TES5Edit Core/wbDefinitionsTES5.pas (ARMO, ARMA, GLOB),
this repository's NifSourceCore/TriMorphCore/SurfacePostureCore/TopologySubsetCore,
and the established BSA named directory/file format. Original fixtures contain no
user meshes or Bethesda game assets. Project license: GPL-3.0.
