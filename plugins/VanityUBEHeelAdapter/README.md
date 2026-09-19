# Vanity UBE Heel Adapter — visual-state POC

This directory contains the first proof-of-concept consumer for the
Skyrim Vanity System visual-state API 001.

## Scope

This POC deliberately does **not**:

- identify stockings automatically;
- estimate heel height;
- parse or apply BODYTRI morphs;
- write `NoHeel` through RaceMenu;
- modify UBE feet, shoes, stockings, or OBody state.

It only verifies the SVS → consumer data path.

The plugin:

1. obtains `ISkyrimVanitySystemInterface001` after SKSE `kPostPostLoad`;
2. registers one visual-state listener;
3. queues one additional SKSE task after a visual-state notification;
4. calls `VisitActorVisualState(player)`;
5. logs every `VisualPiece001`.

Logged fields include:

- variant ID and flags;
- source ARMO / ARMA FormIDs;
- replacement ARMO / ARMA FormIDs;
- trigger, source, and visual slot masks;
- variant priority;
- current actor model path.

## Build

From the repository root, configure the normal CommonLibSSE-NG build and build
only the adapter target:

```powershell
$env:SVS_BUILD_VERSION = '1.4.10'
$env:SVS_BUILD_VERSION_STRING = '1.4.10-build13-compatible'
xmake f -y --builddir=build/runtime/flat --skyrim_se=y --skyrim_ae=y --skyrim_vr=n -m releasedbg
xmake build -y VanityUBEHeelAdapter
```

Expected output:

```text
build/runtime/flat/windows/x64/releasedbg/VanityUBEHeelAdapter.dll
```

## Runtime requirements for this POC

- SKSE / Address Library appropriate for the game runtime;
- Skyrim Vanity System built from the visual-state API branch ancestry
  (commit `4a4dd0e9376181957ed9752bc58530c12294086c` or later);
- Dynamic Armor Variants Extended as required by SVS.

The adapter should load after SKSE normally; it does not require an ESP.

## Game test

Install `VanityUBEHeelAdapter.dll` under:

```text
Data/SKSE/Plugins/
```

Then exercise these SVS operations:

1. load a save with a normal vanity outfit;
2. switch to another vanity outfit;
3. change one slot override;
4. preview an outfit / kit;
5. cancel the preview;
6. test a kit where a stocking is visually injected through slot 32;
7. test a shoe replacement through slot 37.

Inspect the SKSE log for `VanityUBEHeelAdapter`. A successful change should
produce an `[SVS snapshot]` line followed by one or more `[SVS piece]` lines.

For each visible replacement, compare the log with what the actor actually
shows in game. In particular record whether:

- `replacementARMO` / `replacementARMA` identify the expected item;
- `triggerSlots` reports the source vanity trigger (for example slot 32 or 37);
- `visualSlots` matches the replacement ARMA;
- `model` points at the expected female mesh;
- preview pieces carry the preview flag;
- hidden pieces carry the hidden flag.

## Success criterion

Do not proceed to TRI morphing until the logged visual pieces reliably match
the visible SVS/DAVE result for the test cases above.

The next phase will add a hand-authored stocking + footwear mapping and still
remain logging-only before any mesh mutation is attempted.
