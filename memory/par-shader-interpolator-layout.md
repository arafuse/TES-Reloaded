---
name: par-shader-interpolator-layout
description: PAR (parallax) VSO overrides MUST keep stock output registers — un-overridden stock PAR PSOs share those VSOs; PAR multipass uses ZFUNC EQUAL, so POM shadow relief goes through a COLOR1 side channel; how to extract/disassemble stock PAR shaders
metadata:
  type: project
---

Overriding a `POM/PAR20xx.vso` changes its output for EVERY pixel shader it pairs with, including the
stock PSOs we don't override (e.g. VS2002 also feeds stock PS2003, VS2022 feeds PS2018/2019/2021). All
overrides compile to SM3, and in D3D9 the linkage goes by TEXCOORDn, so moving an output silently feeds
garbage to the stock siblings. Every overridden VSO emits the exact stock layout (camera is t6 for
most, t7 for VS2022/2032).

Stock parallax is a single tap: `uv += (baseMap.a * 0.04 - 0.02) * normalize(cameraTS).xy` (shared as
`ParallaxUV` in `POM/Includes/PAR.hlsl`), scaled by `POM.ini HeightMapScale` (default 0.04 = stock)
via `TESR_ParallaxData` c8: x = scale, y = -0.5·scale precomputed on CPU so it stays one mad. PAR2024
is odd: normal map on s0, base/height on s1.

Fork convention: the overridden diffuse PAR PSOs drop the vanilla ShadowMap/ShadowMaskMap term (OR's
shadow pass replaces it); PAR2024 (specular-only) keeps it like stock.

**PAR multipass (measured, interior):** pass 1 has ZWrite on, LESSEQUAL (interior: stock VS2020/PS2016
lighting-only; single-pass props: PS2000/2004). Every follow-up has ZWrite off, **ZFUNC EQUAL**:
VS2024/PS2020 additive, VS2028/PS2022 base multiply (DESTCOLOR·ZERO; losing it = grayscale),
VS2030/PS2023 + VS2034/PS2025 additive per-light. Most interior PAR draws are stock shaders.

**Never write POM depth into the main depth buffer.** Writing oDepth in all passes breaks the EQUAL
follow-ups (grayscale; even at scale 0 our z/w isn't bit-identical to rasterized depth → fuzz);
first-pass-only + relaxing follow-ups to LESSEQUAL bleeds the follow-up light passes onto any
neighbour inside the offset band (white outlines at every junction — level meshes overlap at
corners). Even bit-exact all-pass depth would make neighbours poke through walls.

**Shadow side channel instead:** the first-pass PSOs (`POMShadowPixelShaders` in ShaderIOHook.cpp:
2000/2002/2004/2006/2010/2016/2018/2026) output `COLOR1` = (geometric view depth, relief view depth);
VSOs emit `DepthData : TEXCOORD8` = (clip.w, eye distance). `ShaderManager::BindPOMDepth` puts the
G32R32F `TESR_POMDepthBuffer` on RT1 only under those draws in the main scene (ZWrite on, no blend) and
it is cleared per main scene. ShadowsExteriors/ShadowsPoint (`reliefDepth`) use the relief depth for
the receiver POSITION only where the stored geometric depth matches the pixel's depth within 0.1%;
normals stay geometric. Relief LIFTS toward the eye (height 1 = `POM.ini ShadowReliefScale` units):
pushing into the wall would let the flat wall in the shadow map occlude every recess.

**How to apply:** before changing a PAR VSO, check which stock PSOs read its registers. Ground truth:
extract from `Data/Shaders/shaderpackage019.sdp` (header `<III` ver,count,size; entries = 256-byte
name + `<I` size + bytecode) and run `fxc /dumpbin /Fc out.asm FILE`. Re-capture pass order with the
`Develop.LogShaders` key (`[GrassOrderDbg]` log in RenderHook). See [[shader-pipeline-facts]],
[[fxc-verify-shader-edits]].
