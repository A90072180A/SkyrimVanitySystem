# Vanity UBE Heel Adapter — phase 2 POC

This branch now verifies the next layer of the SVS → adapter pipeline:
logical-item aggregation plus **manual** stocking / footwear classification.

No mesh mutation is performed yet.

## Current behavior

The plugin:

1. connects to Skyrim Vanity System Visual-State API 001;
2. queries the player after SVS notifications;
3. logs the raw `VisualPiece001` records;
4. aggregates duplicate ARMA/source combinations into logical visual items keyed
   by replacement ARMO;
5. loads a small manual config;
6. reports visible configured stockings and configured footwear;
7. resolves the requested continuous `NoHeel` value in the log only.

It still does **not**:

- auto-detect stockings;
- auto-estimate heel height;
- parse BODYTRI;
- apply `NoHeel`;
- touch OBody, UBE feet, shoes, or actor-wide RaceMenu morph state.

## Config

Installed path:

```text
Data/SKSE/Plugins/VanityUBEHeelAdapter.json
```

Current POC schema:

```json
{
  "stockings": [
    "runtime:FE429D1A"
  ],
  "heels": {
    "runtime:FE4AB80A": 1.0
  }
}
```

The values are **runtime FormIDs** for this POC only.

The included defaults correspond to the latest test log:

- `FE429D1A` — the visual CPB stocking item;
- `FE4AB80A` — the visual Converse footwear item;
- Converse is configured as `NoHeel = 1.0` (flat foot).

Runtime FormIDs can change with load order. The adapter therefore also logs a
stable `Plugin.esp|localFormID` identifier for every logical item and ARMA.
A later phase will move the config to those stable identifiers.

## Expected log

When both configured items are visible:

```text
[manual stocking] ARMO=FE429D1A ...
[manual footwear] ARMO=FE4AB80A ... requestedNoHeel=1.000
[heel plan] stockingARMO=FE429D1A footwearARMO=FE4AB80A requestedNoHeel=1.000 morphApplied=false
```

For the stocking, every distinct replacement ARMA is also logged as a
`stocking geometry candidate`. This is intentional: a logical stocking ARMO
may contain the actual stocking mesh plus unrelated helper ARMA such as
`FemaleHands`. Phase 3 will select the geometry that actually carries the
`NoHeel` BODYTRI morph rather than morphing the whole ARMO.

## Test cases

Repeat the successful phase-1 scenarios:

1. slot 32 injects the stocking;
2. slot 37 injects the stocking;
3. slot 37 displays the configured Converse;
4. preview / cancel preview;
5. full outfit switching.

For each case verify that:

- the logical stocking ARMO is detected once even if raw pieces are duplicated;
- the stocking lists all candidate geometries;
- the Converse is detected as footwear only when visible;
- the final `heel plan` appears only when both a configured stocking and one
  configured footwear item are present;
- the line always ends with `morphApplied=false`.

Do not proceed to TRI mutation until these logs match the visible game state.
