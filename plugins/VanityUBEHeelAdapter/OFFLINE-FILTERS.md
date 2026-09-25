# 0.17.0-offline5 — filtered catalogs and explicit pair scopes

Python tools only. Keep the current 0.17.0 DLL, console ESP, user configuration,
working barefoot branch and the existing NoHeel / Heel range policy. No change
to geometry fitting, residual thresholds, NIF/TRI/ESP or MO2 load ordering.

## Why this update

The old GUI gave recommend() an empty stocking list when nothing usable was
selected, and recommend interpreted that as ALL measurable stockings. Shoes
were always all measurable shoes. A large catalog therefore exceeded the fixed
20,000 pair budget without a useful preflight. This update does not simply raise
the budget or silently split and partially apply a request.

## UI

Separate stocking and footwear tabs have independent filters and selections.
A third tab can inspect all candidate/diagnostic records. Defaults show only
`measurable`; this means the scanner found the geometry/capability it needs,
not that every shoe/stocking pair is valid or visually proven to fit.

Filters:
- Type: stockings, footwear, classified raised/flat foot pose, candidate or unknown.
  Raised/flat are existing measured foot-pose labels, not guesses from heel names.
- Status: measurable, all, unmeasurable, source errors, quarantine, exact statuses.
- Name / EDID, source plugins, model path, model provider / physical path, all fields.
- Different fields are AND. Within one text field, space-separated words are AND;
  `|`, comma or semicolon separate OR alternatives. Matching is case-insensitive,
  literal substring matching. Backslashes are preserved; ! is NOT a negation.
  Example: `heel | 高跟 | 丝袜 | cosplay`. Model query: `!UBE`.

Column headings sort. Horizontal scrolling exposes long paths; double-click or
Details opens a copyable complete row. Rendering is paged at 300 rows. Ctrl+A
selects the current page; the explicit Select All Filtered button selects every
eligible matching row across pages. Page changes preserve selection. A filter
that hides a previously selected row removes it from the calculation scope;
clearing the filter does not silently reselect it. Stockings and shoes remain
independent when switching tabs.

The footer shows exact stocking count x footwear count and total pairs BEFORE
starting. Compute is disabled for empty selection or more than 20,000 pairs.
The engine validates again before opening geometry. Above 1,000 pairs the UI asks
for confirmation. The reference shoe is independent of the target-shoe filter:
it still needs a unique, measurable catalog entry and explicit user confirmation.
A frozen list of exact armor::addon keys is passed to the worker and written into
`offline-candidates.json` under `selection`.

Save/Load Filters uses a separate, user-chosen `VHA.filters.json`. It does not
write the runtime user overlay or preserve hidden calculation selections.
Load Old Catalog is READ-ONLY PREVIEW: offline4 catalogs can be used immediately
to inspect/filter metadata, but a new scan in MO2 is required before calculation
and application. Cached metadata is not trusted as current source geometry.

## Three distinct kinds of source

Origin ARMO/ARMA plugins come from stable record IDs. Winning-record plugins are
reported separately in new scans as `armorWinnerPlugin` / `addonWinnerPlugin`.
The plugin filter searches both. None establishes which MO2 mod supplies a NIF.

For actual model providers, the reader asks GetFinalPathNameByHandleW for the
SAME opened NIF/BSA handle that was read. This is best effort. It does not walk
inactive mods or infer a provider from a filename. With a known mods directory,
a physical path under that directory identifies its immediate child mod folder.
A standard profile under `<instance>/profiles/` supplies a default
`<instance>/mods` root; custom locations must be entered in the UI or --mods-root.
Overwrite is identified only under that instance's known overwrite path.
NIF sources (including high/low weight models) determine modelProviderMods; an
unrelated TRI provider is not presented as the model provider. BSA provenance
identifies the archive provider, not an invented loose path for an archive member.
If the handle API is unavailable, returns only virtual Data, or cannot establish
an origin, provider state stays unknown/partial. The UI shows this limitation.
Old catalogs usually lack this field; rescan to collect it.

API reference inspected:
https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew
Widget behavior reference:
https://docs.python.org/3.12/library/tkinter.ttk.html

## Compatibility and safeguards

The offline2 open-first I/O, offline3 official-master rules and offline4 record
quarantine remain in place. Quarantine never becomes measurable through a filter
or manual label. Existing manual pairs are preserved on batch apply. The native
2048-entry user-overlay limit is unchanged; 20,000 is a computation budget, not
permission to save an arbitrary-sized runtime configuration.

The Python API keeps None as explicit all-mode for existing callers, but [] now
means none. recommend adds keyword-only shoe_keys. CLI uses --stocking and --shoe;
--all-measurable is required to deliberately include an unspecified side. Old
candidate reports must be recalculated by offline5 before application.

## Install and check

Merge the complete tools folder into the actual physical folder named in the
MO2 Python launch argument. Do not create tools/tools; ensure title says
0.17.0-offline5. Replacing only the entry script misses required new modules.
No new DLL, ESP, BodySlide rebuild or load-order change is needed.

1. Optional: load an existing offline-catalog.json to verify filters immediately.
2. Via MO2, scan once with the same Data/profile; set the mods root when custom.
3. Stocking tab: measurable, plugin `Cosplay Basics`, model `CPB_SB_1.nif`.
   Select one intended variant, or deliberately select all filtered variants.
4. Footwear tab: measurable, plugin `Converse | Glass high-heeled | Witchy Agata`,
   model `!UBE`. Select intended variants; inspect the footer count.
5. Keep the verified Converse anchor. Calculate. Candidate rows must contain
   only selected identities; rejected fits are still rejected, not auto-approved.
6. Apply a reviewed, not-already-manual pair only when desired. Existing Glass=1.1
   and other user settings stay protected. Existing runtime reload instructions
   apply; this update does not alter the game's console/reload implementation.

No game launch is required to verify the new filtering, selection or preflight.

## Test boundary

Pure filters and explicit-scope tests, synthetic full scan -> selected compute ->
apply tests, and real Tk interaction tests cover empty selection, oversized jobs,
reference independence, pagination, hidden-selection removal, literal Chinese/
English/path matching, filter save/load, preview restrictions and provenance.
The tests use synthetic game-format data, not bundled user plugins or meshes.
The user's complete catalog may be used locally for UI scale checking, but it is
not committed to the public repository. Windows CI tests the real handle API on
ordinary files; that is not a claim of validation inside the user's MO2 hooks.
