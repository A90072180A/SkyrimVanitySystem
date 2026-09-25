# 0.17.0-offline6: lossless report storage and shoe-group review

Python-only update. Runtime DLL/ESP, barefoot, SVS rendering, geometry fit,
NoHeel [0,1], configured Heel maximum, and existing manual pairs are unchanged.

## Defect

Offline5 allowed 20,000 calculations but serialized every result with repeated
item names, model paths, source file/provider metadata and a two-space indentation.
A valid 115 x 168 = 19,320 job could therefore finish calculating and fail the
separate 64 MiB UTF-8 output limit. The exception was not a missing game resource,
load-order error, free-disk-space diagnosis, or a failure of the pair-budget UI.

## Lossless storage (implemented)

`offline-candidates.json` now uses schema 2 / `vha-candidates-interned-v1`:

- metadata/reference/context/plugin validation is stored once;
- stocking and footwear identity dictionaries replace repeated full item records;
- source records, ordered source sets and measured stocking responses are shared;
- identical complete analysis results are shared only when their canonical JSON
  is exactly equal (including raw values, rejection reason and edited history);
- each pair retains its original index and five table references. The decoder
  restores a logical schema-1 per-pair view for existing UI and apply validation.

No numeric rounding, statistical inference, confidence pooling or automatic
acceptance occurs in this storage transformation. Both morph values, raw values,
residual, membership, sources and rejection status survive. Distinct results
remain distinct even when they happen to be close. Two rows decoded from a shared
entry do not share mutable dictionaries: editing one cannot modify the other.

The file remains ordinary UTF-8 JSON, but it is compact rather than pretty-printed.
A bounded streaming writer avoids allocating one giant encoded string. It keeps
the 64 MiB disk limit, flushes a unique temporary file and only publishes after
successful completion. Old output remains intact on error, temporary files are
removed. Bad references, invalid/non-finite JSON, oversized tables/row counts and
excessive expanded data are rejected during read. Read/write limits agree.

The calculation is retained in memory on publication failure; the UI displays it
as UNSAVED, blocks applying an older disk report, and offers Retry Save without
re-running geometry. Closing the process still loses that unsaved memory. This
is not resumable partial-calculation/checkpoint support.

Single-row manual editing uses the same compact writer and publishes an edited
copy before changing the UI. Batch apply additionally compares the currently
shown report's file hash. A different report cannot be applied using stale UI
indexes. Existing source/provider hashes, load-order checks, record quarantine,
range rules and protection of pre-existing user values are retained.

## Shoe-group analysis (implemented, review-only)

Use `按鞋共享值分析` after saving a calculation. It operates on selected candidate
rows, or the whole current report when no result rows are selected. It writes
`offline-shoe-groups.json`, a small, separate review report bound to the candidate
file hash. It does NOT write native runtime defaults or alter actual values.

Default: zero tolerance and same measured stocking response. The response family
hash includes the weighted source baseline positions, ordered triangles, local /
skin transforms and the actual NoHeel/Heel deltas, not merely matching slider
names. Thus differently named texture variants may qualify, but unrelated morph
bases do not automatically become one family. This is a source-model measurement,
not a runtime OBody assertion.

Optional coefficient tolerance 0..0.1 (e.g. 0.02) proposes a median representative,
rounded to two decimal places only when EVERY member is within tolerance and the
representative respects the control bounds. Greedy clustering uses the entire
candidate group's bound, not transitive nearest-neighbor chaining. Groups split
by the exact shoe/ARMA/model, active morph direction and original result status.
Range-saturated, unsupported, ambiguous or invalid rows are excluded, never
promoted by group membership. NoHeel and Heel remain separate directions.

An optional Cross Response Families switch allows broader numerical exploration,
but reports family count and makes clear that numerical similarity is NOT shared
morph semantics. Original values, min/max, largest proposed coefficient change
and a morph-based estimate of added vertex displacement are retained/referenced.
The estimate is in model units and is NOT a new fit residual or visual guarantee;
residual at the proposed representative is explicitly unknown.

Members are explicit candidate indexes under the source report hash, not wildcards
or rules that automatically enroll newly installed stockings. The member dialog
shows stable armor/addon IDs and original values. An old group report is only a
historical proposal once its source candidate file changes.

## Native user configuration boundary

This update does NOT add a grouped schema to the 0.17 DLL. That loader still reads
explicit user `pairs`, at most 2048 entries; the offline tool also checks its
encoded configuration budget before writing. Computation size, report size and
runtime configuration size are three different limits. Batch application of too
many NEW pairs is rejected before checking all resource files, with no partial
user file written. It does not claim 20,000 calculated results can all be applied.

A future native shared-value rule should use a shoe-specific group plus an explicit
stocking/member list and source context; exact manual-pair overrides / ignore
rules must win. Changing a common value would affect all members, unlike changing
one pair. Changing a mesh, TRI, race or build context would require validation.
Near-value grouping is lossy unless per-member residual offsets are retained.
Do not put the review report into `VanityUBEHeelAdapter.user.json`.

## Verification and installation

Synthetic 19,320-pair and all-distinct 20,000-pair tests exercise write/read,
identity/control/source equality, not actual visual fitting of the user's catalog.
Additional tests cover source invalidation, rejection preservation, malformed
references, failed publication/retry, existing configuration safeguards, grouping
bounds and real Tk interactions. No user load order or game assets are committed.

Merge the entire `tools` folder into the actual physical tool directory named by
the MO2 Python launch argument. Window title must say `0.17.0-offline6`. No DLL,
ESP, BodySlide rebuild or user settings replacement is required. Rescan once, then
retry the desired <=20,000 pair selection; the former 19,320 selection is valid.
Check saved byte count and report, then optionally preview exact/0.02 groups.
Do not batch-apply thousands of suggestions merely because storage succeeded.

Implementation references:
- https://docs.python.org/3.12/library/json.html (iterencode, compact separators)
- https://docs.python.org/3.12/library/os.html (fsync, replace)
