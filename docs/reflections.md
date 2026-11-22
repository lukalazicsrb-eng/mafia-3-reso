# Evidence and implementation boundaries

## Modern Tweaks and Mafia III

The available Modern Tweaks 2.5.1 log shows `reflections ultra`, a `[Visuals] Reflections=1` change, and four renderer offsets `0x6ad21`, `0x6ac87`, `0x6ac85`, `0x6ad44`. It does not contain frame texture descriptors or identify the meaning and size of each field. The original DLL is not part of this repository. Modern Tweaks has no public F8 extension interface in the available release. Its own options and `ModernTweaks.dat` settings are used for the comparison. [Modern Tweaks release](https://www.nexusmods.com/mafia3definitiveedition/mods/78)

The shipped game combines screen-space and cube-map array tracing. Its reflection algorithm uses importance sampling and screen-space filtering. The later full-resolution spatio-temporal algorithm in the engine presentation is a separate improvement. [2K renderer presentation](https://www.gdcvault.com/play/1025467/)

| Candidate DXBC CRC-32 | Role to check in the capture |
| --- | --- |
| `685D5D10`, `7E83AA58` | Screen-space reflection passes. Identify size, viewport, resource producers and history usage. |
| `A2C3D0F2` | Possible screen-space reflection pass. |
| `3117C877` | Probe blend. `t3` is a cube array and `t4` a sky cubemap in the reference. The other inputs and output targets need capture validation. |

The candidate IDs come from an independent game-specific shader reference. The observer also records SHA-256 of the bytecode received from D3D11. CRC-32 is a discovery filter. It supplies no permission to replace a pass or resize a texture. [Shader reference](https://github.com/Filoppi/Luma-Framework/tree/30c4045a47edc1a175646bd8a0e18b89778f91e5/Shaders/Mafia%20III) · [Pass labels](https://github.com/Filoppi/Luma-Framework/blob/30c4045a47edc1a175646bd8a0e18b89778f91e5/Source/Games/Mafia%20III/main.cpp#L1946-L1948)

## Where MSAA applies

`ReflectionMsaaTarget` supports a dedicated 2D raster pass with its own color and depth. It validates color/depth format support and sample quality, pairs attachments with identical dimensions and sample counts, and resolves to a single-sample shader resource. The caller supplies the geometry and draw states. Configuration reuses targets until the description changes. [Direct3D 11 target constraints](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargets) · [ResolveSubresource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-resolvesubresource)

A fullscreen SSR shader can cover every sample in a pixel while computing a single ray. Raster MSAA therefore cannot by itself improve the sampling or history of SSR. A compute UAV or mipmapped probe array requires its own sampling strategy. GTA IV FusionFix implements multisampled render targets for known geometry/reflection passes. The same approach would require a verified Mafia III geometry pass, matching depth, and a resolved resource consumed by the intended vehicle material. [FusionFix implementation](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix/blob/7c021163a0081e64c65d063efea58e133f43ec80/source/reflectionmsaa.ixx)

## What the captures should prove

Inspect equivalent passes off/Ultra at the same 1440p output, including viewport, UAV/RTV dimension, mips, samples, subsequent inputs and producers. Capture again after loading or resize if the initial A/B reveals a dimension change. Check vehicle paint separately from the rear-view mirror. Before a resolution change, verify that the complete pass accepts the new extent, including depth, dispatch coverage, sampling coordinates, reprojection and dependent consumers. Temporal filtering requires motion/depth and history validation across frames. No gameplay or performance findings are claimed without those measurements.
