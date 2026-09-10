---
name: grass-drawn-after-water
description: "Oblivion main-pass order: opaque -> water group A (WATER012) -> sky -> LOD terrain -> grass -> water group B (WATER000/001); grass draws BETWEEN the two water groups. Pre-water snapshot at first water bind excludes grass."
metadata: 
  node_type: memory
  type: reference
  originSessionId: e23d1f36-bdcb-4868-b0d1-9d0abb166a7e
  modified: 2026-07-26T16:12:58.040Z
---

**Engine fact (REFINED by [GrassOrderDbg] frame capture 2026-07-15, Lake Rumare/IC bridge):**
Oblivion's main `RenderObject(WorldSceneGraph)` pass order is:
opaque scene → **water group A** (`WATER012.pso`, 4 passes) → sky/sun/clouds →
LOD terrain (`SLS1017/1030` blocks) → **grass** (single batched pass) →
**water group B** (`WATER000/001.pso`, 9 passes).
CONFIRMED by position logging (2026-07-15): group A = distant LOD water planes
(90k+ units from camera), group B = NEAR cell water (cell-aligned positions in
loaded cells). So the near water surface renders AFTER grass — "Geo → Grass →
(inject here) → NearWater" needs NO engine reordering.

**Discriminating near vs LOD water binds — use the PS NUMBER, not alpha flags:**
group A always binds `WATER012+`, group B always `WATER000-011`. The alpha-blend
bit is NOT usable: normally near water = 00ED (blend on, zwrite off) and LOD =
00EC, but when the camera gets CLOSE to the water the engine flips the whole
frame's water to an opaque mode (all 00EC, zwrite on, near surface switches to
WATER007) and a blend-bit predicate silently stops matching (the mid-scene shadow
apply then fell back to post-water and painted shadows on the surface near the
player — ScreenShot4 bug). The earlier "grass after water" fact (2026-07-12) came
from the pre-water snapshot firing at the FIRST water bind (group A, LOD) — true
but incomplete. Grass DOES write depth (it lands in the main `DepthTexture`,
resolved at end of `RenderObject(WorldSceneGraph)`), which is why DoF/AO/god-rays
respect it. Also: raw game shaders that declare TESR_RenderedBuffer get the scene
StretchRect'd into it at their first SetCT per frame (ShaderRecord::SetCT,
`RenderedBufferFilled`) — this is how water refraction sees the (now shadowed)
pre-water scene.

**Dispatch call sites (from [GrassOrderDbg] stack traces):** pass-group scheduler
fn around `0x007ACxxx` (ret addrs 007ACD0F=grass, 007ACDE9/007ACED1=water) called
from `0x007AE6F2`; fixed group order. Generic pass call site ra=`0x007673FD`;
grass has a dedicated render routine, call site ra=`0x007F799F`. Grass
NiAlphaProperty flags=`0x12ED` (blend + alpha-test ref 64 + ALPHA_NOSORTER);
water surface flags=`0x00EC`.

**Consequence for pre-water depth:** `DepthTexturePreWater` is snapshotted at the
first numbered water-surface bind (`WATER000-012`, RenderHook.cpp ~366). Because
grass draws after that point, the pre-water snapshot has the submerged floor but
**no grass**. Oblivion has a water plane in nearly every cell (sea level even under
terrain), so the snapshot fires almost every frame — meaning a depth effect that
reads pre-water depth sees through grass *everywhere*, not just near visible water.

**Symptom this caused:** the image-space sun-shadow apply
(`ShadowsExteriors.fx.hlsl`) read only pre-water depth, so grass pixels
reconstructed the ground behind the blade and the ground's shadow was painted over
grass "as if it were invisible."

**Fix pattern (commit 6e96aef):** no single snapshot has both grass and
floor-without-water-surface, so `readDepth` samples BOTH depths and selects per
pixel by the waterline: main depth (grass + land) where the visible surface is
above `TESR_WaterSettings.x` (= `Tes->GetWaterHeight(Player)`), else pre-water depth
(submerged floor). Waterless cells report a large-negative sentinel height, so the
fallback never fires there. Sample both UNCONDITIONALLY then ternary-select — a
`tex2D` inside dynamic `if` fails to compile (`X3528`, gradients + flow control).
Underwater grass stays the floor shadow (accepted: water renders over it).

Diagnostic method that nailed it: false-color maps returned from the shader
(classify each pixel, e.g. RED=water/GREEN=land) — decisive when you can't attach a
debugger to the game.
