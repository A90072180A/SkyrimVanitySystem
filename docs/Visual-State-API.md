# Visual-State API 001

`src/api/SkyrimVanitySystemAPI.h` is the public, read-only native interface for
plugins that need to observe the visual armor replacements authored by Skyrim
Vanity System (SVS).

## Scope and guarantees

- Interface revision: `001`.
- Current actor scope: the player. Queries for other actors return `false`.
- A snapshot is built live from the player's worn ARMO/ARMA records and the SVS
  variants that were successfully registered with DAVE.
- Conditions use the same `TESCondition::IsTrue(actor, actor)` evaluation as
  DAVE.
- DAVE priority, slot matching, preview override order, race compatibility, and
  empty-replacement hiding are mirrored for SVS-owned variants.
- Each piece supplies source and replacement ARMO/ARMA pointers and FormIDs,
  trigger/source/visual slot masks, variant ID, and male/female/current-actor
  model paths.
- String pointers and the piece array are valid only during the callback.

Revision `001` does not claim to be a global DAVE resolver. DAVE's public `001`
API does not expose its final resolution cache, so a higher-priority variant
registered directly by another plugin cannot be observed by SVS. Snapshots set
`kVisualState_SvsContributionsOnly` to make that boundary machine-readable.

The snapshot is a live evaluation of what the SVS-owned variants select at
query time. DAVE performs its 3D update asynchronously and exposes no
refresh-completed callback, so it is not proof that the new geometry has
already finished attaching. `kVisualState_RefreshCompletionUnavailable` and
`kVisualState_LiveEvaluation` make this distinction explicit.

## Querying

Copy `src/api/SkyrimVanitySystemAPI.h` into the consumer project and fetch the
interface after SKSE post-load:

```cpp
auto *svs =
    SkyrimVanitySystemAPI::GetSkyrimVanitySystemInterface001();
if (svs && svs->IsReady()) {
  svs->VisitActorVisualState(
      RE::PlayerCharacter::GetSingleton(),
      [](const SkyrimVanitySystemAPI::VisualState001 *state, void *) {
        for (std::uint32_t index = 0; index < state->pieceCount; ++index) {
          const auto &piece = state->pieces[index];
          // Read or copy fields here. Do not retain string/array pointers.
        }
      },
      nullptr);
}
```

The message receiver name is `Skyrim Vanity System`, matching the SKSE plugin
declaration. It is intentionally not the `SkyrimVanitySystem.dll` filename.

## Notifications

`RegisterVisualStateChangedListener` reports successful SVS definition and
preview commits. The callback runs on the game thread from a later SKSE task,
queued after SVS asks DAVE to refresh or apply a preview override.

The listener is intentionally not a substitute for equipment or gameplay
condition events. Consumers that already observe those events should query the
live snapshot again when they fire. Always unregister the listener before the
consumer plugin unloads.

## Suggested UBE Heel Adapter flow

1. Fetch interface `001` after SKSE post-load.
2. Register one listener and also query on the adapter's equipment/condition
   events.
3. In each snapshot, group pieces by replacement ARMO/ARMA and inspect
   `actorModelPath` (or resolve the supplied replacement FormIDs).
4. Identify vanity stockings and shoes in the adapter, not in SVS.
5. Calculate the continuous UBE `NoHeel` value in the adapter.
6. Apply the morph only to the selected stocking replacement nodes/meshes.
7. Treat `kVisualPiece_Hidden` as removal, and prefer pieces marked
   `kVisualPiece_Preview` while preview is active.

This keeps UBE, heel-height detection, TRI morphing, and Automatic Stocking
compatibility outside the SVS core.
