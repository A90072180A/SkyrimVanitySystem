# 0.17.0-offline3 — official primary plugins and Skyrim.ccc

Python tools only. Keep the existing 0.17.0 DLL, command ESP, user configuration,
Glass NoHeel=0 / Heel=1.1, and the already verified barefoot behavior unchanged.
No mesh changes, fit/range changes, or MO2 sorting operations are included.

## Diagnosis

The offline2 traceback reaches read_plugin and then dependency validation for
USSEP. It no longer stops at path.stat. A master not yet in `seen` used to be
reported as `missing/late active master`, conflating distinct failures.

The scanner only supplemented starred plugins.txt entries with five base ESMs.
MO2's Skyrim SE support instead appends its game-root Skyrim.ccc entries to
primaryPlugins; libloadorder uses the same manifest as an early-loading list.
A primary file can thus be present and loaded in-game without a starred entry.
This omission can explain the reported ccbgssse001-fish.esm error; the traceback
alone does not prove the user's actual CCC content, installation, or order.

Primary references inspected:
- ModOrganizer2/modorganizer-game_skyrimSE, src/gameskyrimse.cpp,
  primaryPlugins() and CCPlugins(), blob 75197cfcd2af1c5ed4697be48cfeb7bfd036e6d4.
- Ortham/libloadorder, src/game_settings.rs, ccc_file_paths() and
  early_loading_plugins(), commit 743e8c9fcc37ff27808f0d392ce49dac5d948843.

## Correction

Read `<game root>/Skyrim.ccc` through the same open-first I/O as offline2.
Effective primary order is installed base ESMs followed by installed manifest
entries in their declared order, case-insensitively deduplicated. This includes
any declared four free creations or _ResourcePack.esl, without hardcoding only
those names. The remaining ordinary plugins still require a star in plugins.txt
and retain the selected profile's order. No wildcard cc* activation or recursive
activation/reordering of ordinary dependencies is performed.

An absent or empty CCC file does not imply all installed creations are active.
Absent manifest plugins are recorded rather than fabricated. If an enabled
plugin requires one, dependency validation still fails. Permission errors and
malformed manifests also fail instead of being silently treated as absent.

The distinct dependency errors are now `late-active-master`, `master-not-active`
(the file opens but is not included), and `master-not-visible` (direct open
cannot find the file). These describe this scanner process, not proof about
what Skyrim has loaded.

New report, including on dependency failure:
`SKSE/Plugins/VanityUBEHeelAdapter/offline/offline-loadorder.json`
It records the CCC path/content status, starred entries, profile order, actual
resolved order, each item's inclusion source, skipped absent implicit plugins,
input hashes, and the exact failure. No load-order files are changed.

Candidates bind the exact plugins.txt/loadorder.txt/Skyrim.ccc input bytes and
also record absent optional inputs. Changing/adding/removing the manifest or
installing a previously absent manifest plugin requires a new scan before batch
application. Old offline1/offline2 candidate reports are not applied by offline3.
Existing manual configuration remains protected by the prior merge policy.

## Install / one check

Close scanner and editor. Merge this package's entire tools directory into the
actual directory used by the MO2 executable entry. For the reported traceback:
`D:\MO2\mods\VHA_OfflineScanner_0.17.0-offline2-hotfix\`
The directory may keep its old name: the window must show 0.17.0-offline3.
Do not create tools/tools, copy ESPs into Data, edit plugins.txt, or reinstall
USSEP as a workaround. Start the same script using MO2, choose the same correct
Data/profile and rescan. The first milestone is passing USSEP dependency checks
and completing the catalog; then select CPB and recalculate suggestions.
No game launch, DLL replacement or BodySlide rebuild is needed for this test.
If the check still fails, send the new traceback and offline-loadorder.json;
it contains no meshes. The manifest is in the game root, not its Data folder.

## Verification boundary

Synthetic regression fixtures cover the official masters omitted from starred
lists, conflicting later profile positions, duplicate case spellings, BOM and
comments, primary-only profiles, true absent/disabled/late dependencies,
permission/encoding/path errors, open-first VFS-stat simulation, read-only inputs,
and changed/present-after-absent manifests invalidating batch application.
These are not an actual MO2 injection or a scan of the user's current profile.
