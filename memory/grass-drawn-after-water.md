---
name: grass-drawn-after-water
description: "Oblivion main-pass order: opaque -> LOD water (WATER012+) -> sky -> LOD terrain -> grass -> NEAR water (WATER000-011); discriminate near water by PS number, not alpha flags"
metadata:
  type: reference
---

**Engine fact (measured with the `[GrassOrderDbg]` per-pass log + position logging):**
Oblivion's main `RenderObject(WorldSceneGraph)` pass order is:
opaque scene → **LOD water** (`WATER012+`, distant planes 90k+ units away) → sky/sun/clouds →
LOD terrain (`SLS1017/1030` blocks) → **grass** (single batched pass) →
**near water** (`WATER000-011`, cell water in loaded cells).
So the near water surface renders AFTER grass — "geometry → grass → (inject here) → near water" needs
no engine reordering.

**Discriminate near vs LOD water by the PS NUMBER, not alpha flags.** Normally near water = 00ED
(blend on, zwrite off) and LOD = 00EC, but when the camera gets close to the water the engine flips
the whole frame's water to an opaque mode (all 00EC, zwrite on, near surface switches to WATER007),
so a blend-bit predicate silently stops matching. The code uses `PixelShader->isNearWater`
(set in ShaderIOHook.cpp from the shader name; excludes the `WATERHMAP*` height-map pre-pass).

**What this drives in the code:** the mid-scene sun-shadow apply (RenderHook.cpp, gated on
`InMainScenePass && !PreWaterDepthBufferFilled && isNearWater`) resolves `DepthTexturePreWater` and
renders the darkening quad at the first NEAR water draw. At that instant the depth holds every
shadow receiver — land, grass, submerged floor — with no near-water surface, so
`ShadowsExteriors.fx.hlsl::readDepth` samples that single snapshot. See
[[water-reflection-pass-detection]] for why `InMainScenePass` is required.

Grass DOES write depth (it lands in the main depth buffer), which is why DoF/AO/god-rays respect it.
Raw game shaders that declare `TESR_RenderedBuffer` get the scene StretchRect'd into it at their first
SetCT per frame (`ShaderRecord::SetCT`, `RenderedBufferFilled`) — this is how water refraction sees the
pre-water scene.

**Dispatch call sites (from `[GrassOrderDbg]` stack traces):** pass-group scheduler around
`0x007ACxxx` (ret addrs 007ACD0F=grass, 007ACDE9/007ACED1=water) called from `0x007AE6F2`; fixed group
order. Generic pass call site ra=`0x007673FD`; grass has a dedicated render routine, call site
ra=`0x007F799F`. Grass NiAlphaProperty flags=`0x12ED` (blend + alpha-test ref 64 + ALPHA_NOSORTER);
water surface flags=`0x00EC`.

Re-capture the pass order: the `Develop.LogShaders` key arms the `[GrassOrderDbg]` log in RenderHook.
False-color output from the shader (classify each pixel, e.g. RED=water/GREEN=land) is the fastest way
to debug which buffer a pixel came from when no debugger can attach.
