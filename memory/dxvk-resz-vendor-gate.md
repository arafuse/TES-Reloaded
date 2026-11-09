---
name: dxvk-resz-vendor-gate
description: "CONFIRMED in-game: DXVK after 2.5.3 only honours RESZ when reporting an AMD vendor; on Nvidia OR's depth copy silently goes empty (water vanishes). Fix is d3d9.hideNvidiaGpu = True in dxvk.conf; a StretchRect depth copy is NOT a viable code fix"
metadata:
  node_type: memory
  type: project
  originSessionId: 021854aa-fcf7-4d1b-8c67-7ee9a679ad40
  modified: 2026-11-09T02:23:13.621Z
---

OR copies the game's depth buffer into its INTZ textures (`RenderManager::ResolveDepthInto`) with the
AMD RESZ hack, falling back to `NvAPI_D3D9_StretchRectEx` when `CheckDeviceFormat(RESZ)` fails. Every
depth consumer goes through it: water, the pre-water copy, the near-shell mask.

- **DXVK 2.5.3** reported RESZ and ran `ResolveZ()` for any vendor.
- **DXVK 2.7.1** gates both on the *reported* vendor being AMD (`d3d9_adapter.cpp` CheckDeviceFormat,
  `d3d9_device.cpp` `D3DRS_POINTSIZE` handler). On the RTX 5070 Ti OR then takes the NVAPI path, which
  is a silent no-op on a DXVK device: the depth textures stay empty and near water draws invisibly.
  Symptom in `OblivionReloaded.log`: "NVIDIA detected: NVAPI supported." instead of
  "AMD/Intel detected: RESZ supported."
- **Fix (confirmed 2026-11-08):** `d3d9.hideNvidiaGpu = True` in the game's `dxvk.conf` (reports an
  AMD RX 6700 XT). `dxgi.hideNvidiaGpu` does NOT affect d3d9. OR uses no other vendor hack (ATOC,
  NVDB, FETCH4, INST), so only RESZ changes.
- **DXVK 3.1.1 (2026-11-08):** same `dxvk.conf`, and the log shows "AMD/Intel detected: RESZ
  supported." Not tested whether 3.1.1 still gates RESZ on vendor, so keep the spoof.
- **Not a code fix:** DXVK's `StretchRect` does accept depth→depth copies (INTZ and D24S8 are both
  `VK_FORMAT_D24_UNORM_S8_UINT`), but returns `D3DERR_INVALIDCALL` while `InScene`, and OR resolves
  mid-scene. Native D3D9 refuses INTZ texture targets anyway.

**Why:** a DXVK upgrade can silently break every depth-based effect with no DXVK log warning.
**How to apply:** after any DXVK swap, check the RESZ line in `OblivionReloaded.log` first. Keep
`d3d9.hideNvidiaGpu = True` when replacing `dxvk.conf`. See [[game-runs-on-dxvk]].
