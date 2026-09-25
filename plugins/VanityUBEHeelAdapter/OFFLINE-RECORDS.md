# 0.17.0-offline4 — contextual record diagnostics and safe quarantine

Python tools only. Keep the 0.17.0 DLL, command ESP, existing manual pairs,
NoHeel <= 1, extended Heel range, and working barefoot behavior. No meshes,
plugins, load-order files, or game runtime code are changed by this update.

## What is known / what remains unknown

The new offline3 traceback fails while decoding ARMO / MODL as a file FormID.
The previous load-order report records only `form-master-index-out-of-range`,
without the source plugin, record ID or offending bytes. It therefore cannot
identify a defective plugin or prove any particular missing format feature.

Skyrim ARMO's armature list uses MODL ArmorAddon FormLinks (see Mutagen's
`Mutagen.Bethesda.Skyrim/Records/Major Records/Armor.xml`). Field lengths alone
are not used to guess an alternative model-string layout. A file FormID is
resolved against that file's MAST table plus itself, not the full runtime
load order. In particular, high bytes FE/FF or FFFFFFFF are not automatically
translated into light-plugin or null references.

## Scanner policy

The public low-level parser remains strict by default. The scanner explicitly
enables a narrow recovery path for an out-of-range master index in a referenced
field, only after the containing record's own identity and binary framing are
successfully decoded. The entire affected ARMO/ARMA is quarantined, not just the
failing MODL entry. This is not an ESP repair or a claim that the engine itself
cannot use the item.

All enabled plugins are still opened, read and hashed. Missing/late/inactive
masters, invalid record header IDs, malformed framing, unsupported reference
sizes, decompression errors and I/O errors remain fatal. No mandatory plugin is
skipped and no load order is changed. The offline2 open/fstat and offline3
Skyrim.ccc handling are retained.

A quarantined winning override remains in the winning-record table as a blocker.
It cannot resurrect an older valid-looking record. Armor referring to a blocked
ARMA or template chain is blocked as a whole, including otherwise good alternate
addons. A later, genuinely valid override can supersede an earlier diagnostic;
the report distinguishes historical issues from winning issues. Deleted winners
remain tombstones and do not parse stale model references.

Only unblocked measured records can enter height recommendations. Batch apply
also checks the recorded block list for the reference, shoe, stocking and addons.
Changing source plugin bytes invalidates prior recommendations. Existing user
manual settings are not modified by scanning, quarantining or regeneration.

## Reports

`offline-records.json` is saved alongside `offline-loadorder.json`, including on
fatal record parsing errors. Each reference issue records the plugin/path,
record type, raw and stable record ID, editor ID, record offset, record/form
flags, compressed state, field/occurrence, offending raw FormID and bytes,
master count, origin-table count, and decoded master index. Occurrences are
zero-based; offsets refer to record headers in the plugin file, not an invented
subrecord offset inside compressed data.

The report also records processed/total plugin counts, the current plugin,
winning quarantine count and blocked stable IDs. The catalog visibly lists
quarantined armor and dependencies as ineligible. Bad unreferenced ARMA records
remain visible in the record report, not fabricated as standalone inventory
armor. A bounded diagnostic limit prevents unbounded report accumulation.

A completed scan with quarantine is partial coverage, not a clean-plugin bill
of health. The GUI says so and clears stale scan/candidate state before a rescan.
Old offline1/2/3 recommendation reports must be regenerated with offline4.

## Installation and next check

Close scanner/editor; merge the complete tools directory into the physical
folder used by the MO2 executable entry. The old directory name may remain;
the window must show `0.17.0-offline4`. Do not create tools/tools. Start via
MO2 with the same Data/profile; do not sort plugins or rebuild BodySlide.

First run the catalog scan without batch applying. Return offline-records.json
and offline-loadorder.json. The new report can identify the actual offending
record without another full geometry export. If the reference error is
quarantined, valid unrelated pairs remain available; do not treat a quarantine
as a successful repair of that item.

## Verification scope

Synthetic regressions cover exact context, FE/FF non-remapping, normal ESL
file-local references, compressed records, deleted/invalid/later-valid winners,
whole-record isolation, transitive template/ARMA blockers, explicit apply guards,
source invalidation, missing dependencies, VFS stat false negatives, preservation
of manual values, and fatal structural corruption. They are not a reproduction
of the user's offending plugin bytes or actual MO2 injection.
