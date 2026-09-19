---
name: distantlod-file-format
description: "RE'd Data\\DistantLOD\\<world>_<x>_<y>.lod format — LOD object instance positions are rounded to whole units because the engine steals the fractional bits for a packed normal"
metadata:
  type: reference
---

Files live in `distantlod\` inside the BSAs (e.g. `MergedLOD - LODs.bsa`, `Oblivion - Meshes.bsa`).
Layout (all little-endian, 4-byte fields):
```
uint32 recordCount
repeat recordCount times:
    uint32 formID          // base static (e.g. 0x89B)
    uint32 instanceCount N
    float  pos[N][3]       // ABSOLUTE worldspace X, Y, Z
    float  rot[N][3]       // radians, usually all 0
    float  radius[N]       // bounding radius; scale = 0.01 * radius in the shader
```

**The important part:** the low bits of every one of those floats are not position data.
`frac(X)` and `frac(Y)` are always exactly 0.5, `frac(Z)` always 0.970703125, `frac(radius)`
always ~0.97. The distant-LOD vertex shaders (`Shaders/ExtraShaders/DISTLOD2002.vso.hlsl`,
`DISTLOD2003.vso.hlsl`) show why:

```hlsl
r1.xyzw = frac(InstanceData[i]);          // fractional bits ARE the payload
q0.xyz  = (r1.xyz - 0.5) / 0.5;           // -> packed vertex normal
... DiffuseColor * (r1.w * dot(DiffuseDir, q0)) + AmbientColor
r0.xyz  = IN.position.xyz * (0.01 * InstanceData[i].w) + InstanceData[i];  // full value used as world pos
```

So the engine packs a normal (xyz) + a light multiplier (w) into the fractional part, and the
*whole* value (integer + stolen fraction) is used as the instance's world position. Distant LOD
object/tree instances are therefore **snapped to a 1-unit grid** (invisible at LOD range), and scale
is quantized to 0.01.
