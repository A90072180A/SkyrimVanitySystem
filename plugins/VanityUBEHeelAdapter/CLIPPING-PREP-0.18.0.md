# Clipping preparation: separate skin, fabric and transparent footwear

This is an **experimental source-preparation core**, not an enabled runtime fix.
The installed adapter DLL, height values, library format, user overrides, barefoot
handling, scanner and browser behavior are unchanged. No user game assets are
committed. No new in-game test is requested for this preparation milestone.

## Evidence and scope

The supplied closed Agata sample already has its `Feet` shape marked Hidden.
The supplied transparent Glass sample keeps Feet visible. CPB is a single
continuous fabric shape; it is not a skin-foot object. Therefore hiding another
skin object is not a sufficient cure for stocking-through-shoe clipping.
Presence/absence/Hidden flags of a source Feet shape are diagnostic evidence,
not a universal shoe-coverage detector and not proof of current runtime culling.

Treat a user's transparent/open-shoe designation as a preserve-visibility rule.
Unknown shoes and barefoot must preserve fabric. Even for approved opaque shoes,
the collar boundary needs coverage validation; a scalar height coordinate alone
cannot guarantee invisibility during animation or a changed body preset.

## Source-preserving preparation core

`tools/offline/mesh_partition_prep.py` exposes a bytes-in/bytes-out `split` helper.
Callers supply an explicit nonempty proper face subset and a **new** BODYTRI
resource path. It performs no disk writes, installation, scan or runtime change.
It accepts one bounded SSE BSTriShape, one identity-domain skin partition and a
unique NiNode parent; unsupported or ambiguous inputs are rejected.

The original shape keeps the complement faces. A separate foot shape has exactly
the selected faces. Each keeps the complete original vertex domain. UV, normals,
packed skin streams, skin transforms and original morph records are preserved;
TRI records for the original shape are duplicated for the foot shape without
requantizing their deltas. Both shapes start with the original visibility flags.
The new TRI path prevents accidentally using a file lacking the new shape's
morphs. The old vertex/morph index correspondence is never guessed or renumbered.

Round-trip verification checks decoded positions, local/skin transforms,
complementary face ownership, original and duplicated TRI responses and unchanged
unrelated blocks. Full visibility reconstructs the same **geometric surface**.
This is not a pixel-equivalence claim: alpha ordering, bone-driven effects,
cloning, render bounds and materials still need runtime validation.

The complete vertex buffer is deliberately duplicated in this initial method;
it is larger than a fully compacted mesh. Compaction is deferred until it can
remap every skin and TRI reference with equally strong verification.

## Not yet implemented

- No conditional runtime culling of the prepared foot shape.
- No automatic opaque-shoe classification or automatic region approval.
- No tightening or corrective morph applied to game models.
- No migration/rebinding of an existing height library to changed NIF/TRI bytes.
- No BodySlide source-project update or guarantee against later Batch Build.

**Do not install prepared development NIF/TRI as a drop-in patch.** Their hashes
change, and the old height library will correctly fail source validation. The
scanner's treatment of multiple morph-capable shapes and the local RaceMenu
application/restoration path must be integrated before any installable release.
The future controller must target only current SVS visual identities, preserve
all parts for transparent/open/barefoot/unknown states, restore on rebuild and
never override another mod's ownership of a hide flag.

## Fit investigations remain separate

A bounded inward-only shrink can reduce average gap yet leave or worsen existing
skin intersections. A bidirectional, local clearance proposal is more appropriate
for transparent shoes, but vertex clearance alone is insufficient: triangles can
still intersect skin or the shoe shell. Report such residual contacts explicitly.
Source-only tests do not model current OBody, animation, textures, alpha sorting
or alternative material records. Fitted prototypes must not be presented as
final game patches or as a reason to change a previously verified heel control.

## Verification

`tests/test_mesh_partition_prep.py` contains synthetic fixtures only: exact
round-trip, face partition, new TRI path, morph equivalence through both heel
directions, no input mutation, preservation of original skin/subtrees and
rejection of unsafe paths, empty/full/invalid faces, repeat preparation, missing
morph shapes and truncated input. Existing Python suites remain unchanged.
