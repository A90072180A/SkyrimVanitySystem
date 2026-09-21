# 0.11.0 - measured native morphs, live selection retries, stocking calibration donors

This is a data-collection release, not automatic shoe-posture fitting. The existing scoped NoHeel application, manual shoe values, SVS GUI and render sources are unchanged.

## Findings driving the change

The 0.10 test reported `morph-absent` for Feet/NoHeel in the bare-foot reference TRI, not an I/O failure. Its morph-name inventory instead contains HiHeelz_CBBE and HiHeelz_CBBE_to_UBE. Those names must not be assumed to mean NoHeel or its inverse. Bare-foot attachment was logged but not exported; its exact rejection cause was not captured by 0.10. The upstream BSVisit walk already includes its root, so this release does not invent a root-exclusion fix.

## Changes

- Retain the checked vertex/index decoder. Unknown layouts, nonidentity maps and multi-partition geometry still fail explicitly; no new raw layouts are guessed.
- After a player attachment request, retry at 150, 500 and 1500 ms. SKSE tasks do the game-object work. The timer holds only tickets and time. New requests invalidate older queued retries; the main thread never sleeps.
- Continue to require current scene or active biped membership. If an observed Feet pointer cannot be found, accept only one supported current Feet geometry with an exact normalized inherited BODYTRI match. No filename-only, name-only, nearest-node or buffered-part fallback. `identityIsCorrelated=true` distinguishes this weaker identity evidence from pointer identity. None of these diagnostic selections become runtime morph targets.
- Log live Feet RTTI, geometry support, BODYTRI availability, observed-root type and retry attempt on selection failure. A legacy or dynamic unsupported object remains unsupported rather than being cast into another layout.
- Export each requested native morph independently to `nativeMorphMeasurements` and `referenceMorphMeasurements`, including its own sparse delta, native name, source fingerprint and status. `mappingToNoHeel` remains null; `automaticApplicationAllowed` remains false.
- Optionally collect geometry from explicitly configured `stockings` in active biped parts, tagged `geometryRole=stocking`, and measure the actual geometry name's NoHeel record. This is the donor side of the eventual foot/stocking calibration; it does not assume the stocking and foot share a topology.
- Preserve `localMorphState=not-certified`: actor-wide morph keys do not tell us which scoped local pass last affected a mesh. Exported coordinates must not silently be treated as a NoHeel=0 endpoint.
- Persist all measurements through the existing background JSON writer. This is not the BodySlide disk-index cache, which remains unimplemented.

## Install and test

Keep the working Converse manual value 1.0, the same OBody preset, and the existing BodySlide outputs. Keep Automatic Stocking disabled. Use the bundled config or add:

```json
"exportFootGeometry": true,
"exportStockingCalibration": true,
"referenceBodyTri": "!UBE\\Feet\\femalefeet_tangent.tri",
"diagnosticFootMorphs": ["NoHeel", "HiHeelz_CBBE", "HiHeelz_CBBE_to_UBE"]
```

1. Show Converse AND the already configured CPB stocking through SVS; wait three seconds. Slot 32 may be used to keep the stocking visible independently of actual shoes.
2. Unequip the actual shoes and wait three seconds with bare feet visible, then restore the shoes.
3. Display Glass or Gala once and wait three seconds. Do not add a guessed manual posture value.
4. Send the full adapter log plus a ZIP of `Data/SKSE/Plugins/VanityUBEHeelAdapter/geometry`. New files have generatorVersion 0.11.0 and schema 3. Older files can remain. Do not paste coordinate arrays into chat.

MO2 may put these new files in Overwrite or a configured output mod. The small `generated.json` remains a separate metadata report; it is not an applied calibration cache.

Expected evidence: `[morph measurement] ... morph='HiHeelz_CBBE' status=present ... autoApply=false`, a stocking's native `NoHeel` record, and either a bare-foot snapshot or specific `[foot selection]` diagnostics. `present` confirms that the file contains a checked delta, not that its direction/scale corresponds to the desired stocking posture.

Only Skyrim AE 1.6.1170 is enabled for the raw exporter. Reading/parsing TRI resources is limited to MO2-visible loose files. Native sparse morphs may target different vertex domains; no cross-mesh application or automatic profile promotion occurs in this phase.

## Verification

The existing decoder and TRI/fit tests remain. FootCapturePolicyTests adds 22 checks for exact resource matching, ambiguous and unsupported geometry rejection, duplicate pointer deduplication, path validation, stale-ticket rejection and increasing retry delays. Tests exercise portable policy, not Skyrim's actual callback timing or scene membership. The latter needs the game test above.
