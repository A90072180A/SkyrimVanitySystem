# VHA 0.19.1 CPB integrated acceptance build

Extends the current branch without replacing the established 0.18.0 height
policy, .vhi/.vhs library, browser, manual settings or SVS rendering. The public
native artifact contains no proprietary meshes. Install only with the matching
privately prepared CPB surface bundle; the old offline prototype is NOT a mod.

## Runtime

Exact CPB model/BODYTRI matching selects the smallest common subtree and requires
both named, fully skinned 23340-vertex sibling shapes with NoHeel and Heel.
Unrelated or incomplete geometry is rejected. The generated TRI is separate:
`!ube/caenarvon/cosplay/vha/cpb_sb_surface_v1.tri`.

Agata hiding is limited to the exact ARMO/ARMA/model in the handoff, female mesh,
source weight zero, NoHeel=0/Heel=.47, verified CPB and shoe assets, and no active
unvalidated foot-affecting RaceMenu/OBody slider. It follows a successful local
height submission. Existing explicit `preserve` or the global occlusion switch
veto it. Unknown/transparent shoes do not inherit the mask from a generic mark.

Glass preserves skin and stockings. VHA_GlassFootFit is a separate temporary
local morph, enabled only for the exact Glass identity at NoHeel=0/Heel=1.1 and
the verified weight-zero foot context. It never changes the manual height value.
Other owners' morph keys are not deleted. Switching shoes or barefoot rebuilds
the local baseline and removes this fit while retaining normal height behavior.

A visibility lease saves the original cull flag, restores only a still-owned
value, and yields to detected external writes. Initially hidden geometry is not
unhidden. Same-value writes cannot convey ownership through a Boolean flag.
NiPointers retain object storage; current graph membership is checked before
restoration. No live Skyrim objects are used by file workers. Disabled/ignored
visuals, unavailable SVS state, reload, unequip and graph rebuild clear leases.

## Private assets and existing library

The private bundle retains full vertex domains in both siblings (extra memory),
38736 + 5656 faces, unchanged native vertex attributes, UVs, weights, shader
references and all 222 original position morphs. Both shapes receive identical
fit deltas, preserving their shared boundary. Full visibility with fit zero
reconstructs the original surface. This is not the compact offline prototype.

A compiled fingerprint locks the reviewed profile, three exact old-to-new
resource/fingerprint equivalences and the blocked-slider lists. The worker checks
new source bytes independently. A library entry migrates only when BOTH original
resource and fingerprint match; replacement bytes still must match. Aliases are
part of query cache identity. The existing index, shards and user JSON are NEVER
rewritten. Rollback simply restores the old source overlay and adapter.

## Evidence boundaries

CI tests policy, transactional restoration, file guards, migration, old library
behavior and Windows SKSE compilation. Private model tests verify source hashes,
face union, all native morphs and additive-fit packing. None proves gameplay
rendering. The Glass correction was measured only at source weight zero/H1.1.
Nearest-triangle diagnostics are not complete collision tests: six potential
negative-normal samples remain. Arbitrary body presets, animations, normals,
shoe-shell collisions and transparency sorting are not claimed solved. Agata's
own stocking meshes and skin are not modified.

`runtime-state.json` separates prepared-target detection, source validity,
fit/hide eligibility, actual cull state and interface submission. A successful
submission is not visual validation. Reload remains `set VHA_Reload to 1`;
close the console afterwards. `2` reports version/status.

## Install / rollback

Use a separate MO2 overlay after the old adapter and CPB/BodySlide outputs. It
contains only the matching 0.19.1 DLL, two CPB NIFs, versioned TRI, prepared profile
and documentation. Do NOT replace SkyrimVanitySystem.dll, user JSON, library or
existing tool executable paths. Fully restart Skyrim when changing model/TRI
resources. Disable the overlay with the game closed to return to prior files.

One acceptance pass: Agata -> Glass -> barefoot -> unknown shoe; check no shoe
opening hole, Glass skin and stockings visible at Heel=1.1, complete flat-foot
barefoot restoration, then equip rebuild, reload and save/load. Keep a pre-test
save. This is an integrated acceptance build, not an already gameplay-verified fix.
