---
name: or-screenshot-dds-diagnostic
description: "The mod's F11 screenshot is a full-precision FP16 DDS of the PRE-HDR effect-chain output - the best tool for diagnosing post-processing artifacts."
metadata: 
  node_type: memory
  type: reference
  originSessionId: cfa30f06-1814-48fa-a89b-ffa3daa3825c
  modified: 2026-08-20T22:06:51.656Z
---

`ScreenshotKey = 87` (F11) in `OblivionReloaded.ini` saves via `D3DXSaveSurfaceToFileA` at the end
of `ShaderManager::RenderEffects`. Two things make it a debugging instrument, not just a picture:

1. **It captures the effect chain's render target**, before the game's HDR pass. With
   `RenderEffectsBeforeHdr = 1` that is `EffectSurface`. So "is the artifact in this file?" splits
   the pipeline cleanly in half: in it = the post chain; not in it = HDR/present.
2. **`ScreenshotType = 4` is `D3DXIFF_DDS`, not JPG** - the code appends `.jpg` for any non-zero
   type, so the file is named `C:\Screenshots\Oblivion<date>.jpg` but is a DDS holding the
   **raw A16B16G16R16F** surface. Full float precision, no tonemap, no compression.
   (`ScreenshotPath = \Screenshots` resolves against the drive root, not the game folder.)

Decode it with plain Python - no numpy/PIL needed (neither is installed):

```python
import struct
d = open(path,'rb').read()
H = struct.unpack_from('<I', d, 12)[0]   # 0x0C
W = struct.unpack_from('<I', d, 16)[0]   # 0x10  (fourCC at 84 == 113 == A16B16G16R16F)
def px(x, y): return struct.unpack_from('<4e', d, 128 + (y*W+x)*8)  # '<4e' = 4 half floats
```

`array.array` has no `'e'` typecode - use `struct` with `'e'`. To *look* at it, write a PNG with
`zlib` + `struct` (gamma 1/2.2) and open it with the Read tool; scale a crop up 6x to inspect a
1px artifact.

This is what identified the [[volumetric-light-halfres-edge-leak]]: exact per-channel deltas at the
edge (+0.22 R, +0.00 G, variable B) named the leaking buffer and the 25% bilinear weight outright,
where a JPEG or a visual description could not have.
