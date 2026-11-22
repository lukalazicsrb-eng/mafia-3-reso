# Ultra reflection investigation — findings and scope

## User's repro

The user deliberately returned to **High** in Modern Tweaks because **Ultra** did not work. `[Visuals] Reflections=1` is therefore the user's fallback choice, **not a detected cause**. This branch does not modify `ModernTweaks.dat`, `render_config.xml`, the existing F8 quality mode, or any reflection GPU resources.

The supplied `MafiaReflections.log` contained only:

- `Verified executable D3D11 import connected.`
- No `CRC32=` candidate records, no shader SHA-256 records, and no reflection target descriptors.

The supplied `MafiaReflections-loader.log` confirmed that the original 2.5.1 DLL and the F8 diagnostic dropdown loaded. This **does not prove** that the game's D3D11 context, shader creation and draw callbacks were captured.

SHA-256 of the supplied original `ModernTweaks_reflections_original.dll`:
`529b16daec59e2ce5c735e5c8e7974662b647591da2847c53a16f165a479e900`.
The previously installed diagnostic proxy has SHA-256
`9e3875d13e85052ee5ba79030ba8ac02dd75732b01af4c307f99ce9ea534c84a`.

## An actual source defect fixed here

`native_d3d11_hooks.cpp` originally enabled all draw tracking only when
`CreatePixelShader`, `Draw`, and `DrawIndexed` hooking succeeded in one
short-circuited `&&` expression. If Modern Tweaks or another component had
already intercepted the pixel shader factory, drawing observation would never
even be attempted. The game could still run, the IAT connection could still
print "installed", yet no DX11 draw/reflection resource would ever be logged.

This branch attempts Draw/DrawIndexed hooks independently of the shader
factory; adds actual device/shader/draw/dispatch/present counters; records
time-delayed loader health to distinguish "IAT changed" from "render thread
seen"; and captures a strictly bounded sample of **unmatched shader hashes**
without treating reference CRC values as guaranteed matches.

An unmatched hash is not proof of an SSR pass and never authorizes replacing
the renderer. Sampling is bounded to avoid per-frame disk writes and is
disabled by the existing F8 diagnostic switch.

## Important distinction: what Ultra actually means

The distributed original Modern Tweaks README describes:
"Reflections: High (full resolution, the game's best) or Ultra (plus a cleaner
filter, fewer sparkling pixels)." That is **not a promise that Ultra doubles
reflection target width/height**, and not evidence of 2×/4× MSAA in the game.
The existing `ReflectionMsaaTarget` is only an isolated test raster target,
not a vehicle or SSR integration.

## What would prove an actual Ultra rendering fix

Capture the same reflection shader and render target while High is active and
while Ultra is active. Check SHA-256 shader identities, full resource/view
formats, dimensions, history, viewport and actual filter output. Only then
change the verified Ultra shader and ensure game stability, shadows, vehicles,
clouds, motion and the F8 menu remain unchanged elsewhere.

**Status:** this branch fixes the broken instrumentation/visibility and does
not yet implement or prove a working Ultra reflection shader replacement.
Do not label its ZIP "Ultra fixed" before an in-game A/B shows the filter
executing correctly.

## Verification

The Windows CI workflow runs `reflection_settings_test` (including the new
health sample) and the isolated MSAA WARP test. These do not run Mafia III and
therefore cannot verify any Ultra in-game rendering.
