---
name: or-screenshot-dds-diagnostic
description: "The mod's screenshot key saves the effect chain's render target (a DDS named .jpg when ScreenshotType=4) - with RenderEffectsBeforeHdr=1 it is a raw FP16 pre-HDR capture, the best tool for diagnosing post-processing artifacts"
metadata:
  type: reference
---

`[Main] ScreenshotKey` (87 = F11) saves via `D3DXSaveSurfaceToFileA` at the end of
`ShaderManager::RenderEffects`, from the effect chain's `RenderTarget`. Two things make it a
debugging instrument, not just a picture:

1. **It captures the effect chain's output.** Which side of the game's HDR pass that is depends on
   `[Main] RenderEffectsBeforeHdr`: at 1 the chain runs pre-HDR (`RenderEffectsPreHdr`, FP16 target),
   at 0 (code default) post-HDR (`RenderEffectsPostHdr`). In pre-HDR mode "is the artifact in this
   file?" splits the pipeline in half: in it = the post chain; not in it = HDR/present.
2. **`ScreenshotType` is a `D3DXIMAGE_FILEFORMAT`; 4 = DDS.** The code appends `.jpg` for any
   non-zero type, so the file is `<ScreenshotPath>Oblivion<date>.jpg` but holds a raw DDS of the
   surface (`ScreenshotPath = \Screenshots` resolves against the drive root, not the game folder).
   In pre-HDR mode that is **A16B16G16R16F**: full float precision, no tonemap, no compression.
   Check the fourCC (offset 84; 113 = A16B16G16R16F) before decoding.

Decode with plain Python - no numpy/PIL needed (neither is installed):

```python
import struct
d = open(path,'rb').read()
H = struct.unpack_from('<I', d, 12)[0]   # 0x0C
W = struct.unpack_from('<I', d, 16)[0]   # 0x10
def px(x, y): return struct.unpack_from('<4e', d, 128 + (y*W+x)*8)  # '<4e' = 4 half floats
```

`array.array` has no `'e'` typecode - use `struct`. To *look* at it, write a PNG with `zlib` +
`struct` (gamma 1/2.2) and open it with the Read tool; scale a crop up 6x to inspect a 1px artifact.
Exact per-channel deltas at an edge can name a leaking buffer and its bilinear weight outright, where
a JPEG or a visual description cannot.
