# 0.18.0-library1: browse published shoe libraries

Python-only update; runtime DLL remains 0.18.0. The binary index/shard format,
height fitting, residual-warning policy, manual precedence and barefoot are unchanged.
No game assets or user configuration are included in this commit.

## Open an existing library

The offline window adds **浏览已保存高度库**. It works without scanning or loading
candidate results. If a shoe is already selected on the shoe page, that exact
ARMO/ARMA/model combination is selected in the browser when it exists in the index.
The browser can also be started by MO2 as a separate executable:

```text
python.exe "D:\MO2\mods\VHA 0.18.0 Update\tools\vha_library.py" gui
```

Choose the actual `height-library` folder containing `index.vhi`. The existing
configuration-status/user-overlay path resolver is used when a plugins directory
is available; the user may explicitly choose a different library for inspection.

Search shoes by name, plugin, stable ID or model. Names are optional metadata from
an already loaded catalog; **载入名称清单（可选）** reads an existing catalog without
rescanning. If names are unavailable, stable IDs and paths remain searchable.
Select one exact shoe and click **读取所选鞋**, or double-click it.

Only the active index and the selected shoe file are read. Unreferenced old `.vhs`
files and historical receipts are not treated as current rules. File hashes,
identity, membership and sizes are checked; if the index changes, refresh first.
This verifies stored library integrity, not the current game assets merely by viewing.

Groups show stored control-table entries and explicit member counts. They are not
original user-named group identifiers: equal values may have been pooled by the
exporter. Members show stored and original NoHeel/Heel, original fit residual,
warning/approximation/manual flags and exact identities. Double-click a member
for full precision and source details. Approximate/edited controls have no newly
computed residual. Stored values are not necessarily the final runtime values:
manual/ignore rules, source validation, weight and actor applicability still matter.

Group selection filters members; **显示全部组成员** clears that filter. Member search
supports literal case-insensitive terms with spaces for AND, pipes/commas for OR.
At most 2000 matching members are rendered at once; the interface explicitly says
when it is capped. Narrow the filter or export JSON for all members.

**导出这双鞋的 JSON** writes all members to a new destination outside the library.
It does not rewrite the binary library, candidate report or user configuration.
This release does not add library editing/deleting controls.

## Clipping investigation

**打包所选配对模型（仅本地诊断）** copies the source-bound loose NIF/TRI for 1–16
selected members to a new ZIP. Start through MO2 and select the actual game Data.
Each source must match the library fingerprint. The bundle is private, is never
automatically uploaded, and must not be installed as a patch. No textures,
BodySlide OSP/OSD sources or whole mod directories are collected. An existing
manual pair omitted from the library cannot be collected through this action;
provide its current NIF/TRI directly or select another published member sharing
the intended stocking mesh.

No automatic foot hiding or CPB tightening is enabled here. A shoe's Feet-node
presence does not establish whether skin is visible. Opaque closed coverage,
open/transparent surfaces, skin under stockings and stocking-through-shoe clipping
must be distinguished. Hiding an entire continuous stocking shape would also
hide the leg/body fabric. A future scoped solution must preserve current height
morphs, UV/skin data, restoration on barefoot/open shoes, SVS visual identity and
source-bound exclusions. Ordinary BodySlide zaps delete geometry at build time,
not as reversible runtime vertex morphs.

## Verification

The tests use synthetic binary libraries: selected-shoe-only reads, shared/original
values, ignored orphan shards, independent copies, stale-index rejection, malformed
content, local diagnostic bundling and real Tk actions. They do not prove in-game
clipping is fixed. Existing scanner/exporter tests continue to run. See CI for
Windows/Linux results; local Linux results do not imply MO2 injection was tested.
