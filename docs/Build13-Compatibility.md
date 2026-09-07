# Skyrim Upscaler Build 13 compatibility

This branch contains the compatibility changes used for Skyrim Upscaler All-In-One Build 13.

## Symptom

Skyrim Vanity System continued to load and initialize Dear ImGui, but the menu was invisible after updating to Upscaler Build 13.

## Root cause and compatibility approach

The working fix avoids relying on the legacy SVS presentation timing for the Build 13 UI path:

- SVS registers an SKSE Menu Framework render event bridge when `SKSEMenuFramework.dll` is available.
- SVS draws during the framework's `kAfterRender` lifecycle, using the D3D11 UI render target already selected by that path instead of forcing the swapchain backbuffer.
- Skyrim input events are consumed synchronously inside the input-dispatch hook rather than storing raw `RE::InputEvent*` pointers for later processing.
- The original present-hook rendering remains as a fallback if SKSE Menu Framework is unavailable.

## Verified setup

The fix was verified in-game on Skyrim SE/AE 1.6.1170 with Skyrim Upscaler All-In-One Build 13 and SKSE Menu Framework 3.13. Menu opening, closing, input handling, and rendering were confirmed working.

## Build

The GitHub Actions workflow produces an MO2-ready package containing:

`SKSE/Plugins/SkyrimVanitySystem.dll`

The compatibility build string is `1.4.10-build13-compatible`.
