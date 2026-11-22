# Experimental native Ultra spatial denoiser — verified GPU core, game pass pending

**Do not call this a working Mafia III Ultra reflection fix yet.**

The original Modern Tweaks v2.5.1 README states that **High is already full-resolution reflections**; **Ultra = High plus cleaner filtering**. The user's setting `Reflections=1` is an intentional High fallback because Ultra did not work, **not the cause of the problem**.

## Implemented source

- `src/ultra_spatial_filter.cpp/.h`: genuine native D3D11 fullscreen-triangle shader for a **3x3 edge-aware spatial denoiser** of an already-rendered, separately identified reflection output. No temporal reprojection or per-frame jitter: the filter does not introduce new history wobble.
- Creates a *copy* of the original reflection render target for read-only sampling; writes the filtered result to the original RTV and restores the complete D3D11 graphics state via `SwapDeviceContextState`. Preserves HDR format, dimensions, alpha, existing scene resolution and original Modern Tweaks F8.
- Restricts input to single RTV / single-sampled 2D texture / one mip / known HDR or RGBA8 formats, no DSV or MRT, to avoid damaging unrelated game passes, geometry, clouds or HUD.
- `native_d3d11_hooks.cpp` now allows an **afterDraw** callback following the game's original Draw, with the existing recursion guard.
- `reflection_observer.cpp` includes integration that runs **only if ALL conditions** hold:
  1. Existing reflection diagnostics are enabled.
  2. `[UltraSpatialFilter] Enable=1` explicitly set in `MafiaReflections.ini`.
  3. `[UltraSpatialFilter] VerifiedShaderSHA256=<64 hex chars>` is populated with a *separately verified Mafia III original SSR shader fingerprint*; no guessed CRC is accepted.
  4. **Original Modern Tweaks F8 Ultra** currently selected (`[Visuals] Reflections=2`).
  5. Current game pixel shader exact SHA-256 matches the verified fingerprint.
  6. Bound render target meets every strict isolated-pass signature above.

If any check fails, **original game rendering runs unchanged**. `High` is never altered. Settings persist in the existing F8/ModernTweaks menu; this feature does not introduce another overlay or depend on ReShade/Luma.

## Proof actually obtained

Windows GitHub Actions on MSVC/Windows x64 compiled all targets and passed:
- `ultra_spatial_filter_warp`: actual **D3D11 WARP shader output changed a noisy pixel 128 -> 124**, preserved alpha=200 and RTV binding; rejected mipped resource and invalid null targets.
- `reflection_msaa_warp`: older isolated D3D11 MSAA test.
- `reflection_settings`: F8 diagnostic settings and hook health integration.

These are **GPU unit tests** on synthetic texture content, **not Mafia III rendering tests**.

## What is still blocking real Ultra reflection behavior

Neither the original v0.1.2 diagnostic log nor the supplied screenshots and ModernTweaks.log contain the **actual pixel shader bytecode SHA-256 and RTV at the game's Ultra SSR denoising stage**. They show only the loader and Original Modern Tweaks engine field offsets. The earlier candidate CRC hashes from a separate game-specific project have not been validated as this project's standard DXBC CRC32 and must not be used to trigger output changes.

Blindly applying the denoiser to an arbitrary Draw pass could turn the entire scene blurry or introduce visual artifacts. Therefore **the new filter remains disarmed by default**. A matched High/Ultra GPU capture (e.g. with RenderDoc) must establish the correct shader, output texture and pass order. Then the signature can be locked into a verified build and the filter enabled selectively for F8 Ultra.

Do **not** tell the user to change `Reflections=1` to `2` as a repair. The user chooses High because Ultra currently fails. A proof of correct integration requires High vs Ultra A/B images, confirmed GPU-filter dispatch on the selected reflection render target and visible improvement without temporal wobble.

No original Modern Tweaks binary is redistributed or overwritten by this source. Installer only wraps the user's existing copy, hash-checks it, and includes a full restore script.
