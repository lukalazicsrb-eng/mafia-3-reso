# Ultra reflections: confirmed zero-callback failure and native direct-Draw bridge

## Evidence supplied by the user (22 Nov 2026)

`MafiaReflections-loader(2).log`:
```
Reflection observer installed (import attached; waiting for real renderer callbacks).
Forwarding to verified Modern Tweaks 2.5.1.
Reflection Diagnostics added to Modern Tweaks F8 / Visuals.
Hook health (12 seconds): factories=1 taggedDevices=0 taggedShaders=0 draws=0 dispatches=0 presents=0
WARNING: DX11 factory import was patched, but real game draw capture is NOT confirmed.
```

`MafiaReflections(2).log` contains only the header and `Verified executable D3D11 import connected`. No actual draw events, no shader identities, no reflection resource descriptions, and no Ultra filter output. This directly proves the earlier diagnostic installed *only* an IAT hook without reaching the renderer. Therefore the previous shader filter had **no opportunity to change Mafia III reflection pixels**.

The user also supplied a JPEG XR screenshot of the game near a black car. A single static screenshot is insufficient to establish the exact original SSR/Ultra filter pass or temporal shimmering, which requires a same-scene A/B or motion capture.

## Root cause and correction

The previous `src/native_d3d11_hooks.cpp` attached `D3D11CreateDevice` to the game's EXE import, then tried to replace several D3D11 COM vtable methods. Modern Tweaks has *its own* native DirectX11 hooks. The vtable approach refused previously hooked/out-of-module addresses and never tagged a game device, so no draw/shader/Present observers were active.

The original author's 61 MB `dinput8.dll`, SHA256
`529b16daec59e2ce5c735e5c8e7974662b647591da2847c53a16f165a479e900`,
was provided by the user and statically inspected. Verified against the exact PE bytes:

- Original Modern Tweaks Direct Draw forwarding wrapper RVA `0x79A40`.
- Indirect original D3D11 Draw callsite RVA `0x79A71`.
- Original bytes `FF 15 D1 80 9C 03` (`call qword ptr [rip+0x039C80D1]`).
- Indirect function pointer slot RVA `0x3A41B48`, computed as
  `0x79A71 + 6 + 0x039C80D1 = 0x3A41B48`.

This codepath was **previously reached in real gameplay** by the unrelated v9 F8 test; it is a better basis for observing original rendering than assuming a clean, unmodified D3D11 vtable.

The new `installVerifiedModernTweaksDrawBridge` verifies the exact module filename, PE layout, call bytes and target RVA, then **atomically replaces only the 8-byte function pointer slot in process memory** using `InterlockedCompareExchangePointer`. No executable instruction byte patch is needed, and no original Modern Tweaks file is modified on disk. The original draw method is always forwarded. A worker checks for initialization without blocking Steam startup/DirectInput and logs actual callbacks.

Now `HookHealth` exposes `modernTweaksDrawCalls` and `modernTweaksSlotInstalled`. The verified context/device can be tagged when received. If shader bytecode identity was not available because it was created before hooking, the observer writes up to 48 carefully marked **UNVERIFIED_RENDER_SAMPLE** records with shader pointer and currently bound SRV/RTV/DSV formats, dimensions and original reflection setting, without falsely treating them as SSR or triggering Ultra.

## What this fixes vs what it does not

**Implemented:** correction for the proven zero-callback failure in the old DLL, with version-specific checked loader and actual game render-resource capture path. Preserves Modern Tweaks F8 and existing High settings; no ReShade, no Luma, no new menu. Windows x64 compilation and original WARP denoiser tests can verify build quality but **not whether the new bridge is executed in Mafia III**.

**Not yet proven:** the game's true Ultra SSR shader/RTV and how the original author sets its extra filter. The native spatial filter in `src/ultra_spatial_filter.cpp` exists and passed GPU unit tests, but stays **disabled** until a verified original reflection shader identity and resource signature exist. This package must not be called a working Ultra fix.

An eventual completed Ultra fix requires a confirmed real-game High vs Ultra render-pass capture (with reproducible shader bytecode fingerprints and actual pre/post SSR texture output), then selective activation of the existing spatial filter or a corrected original game filter method. Simply replacing `Reflections=1` with `2` is not a repair; the user intentionally selected High because Ultra failed.

## Safety/rollback

Distribute **only our compiled 64-bit `dinput8.dll` plus `INSTALLATION.txt` and `ULTRA_STATUS.md`** in the manual-test ZIP; no `.cmd` or `.ps1`. The user retains their verified original as `ModernTweaks_reflections_original.dll`. The new DLL must be restored by copying back the saved previous `dinput8.dll` if the game does not launch.

Game test evidence to check *once*: `MafiaReflections-loader.log` should report `Verified original Modern Tweaks Draw slot connected atomically.` and nonzero `modernTweaksDraws`. `MafiaReflections.log` should contain actual `UNVERIFIED_RENDER_SAMPLE` or authenticated shader data. If those are absent, do not claim that Ultra works or expose a filter toggle.
