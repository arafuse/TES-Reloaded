---
name: shader-deployment-workflow
description: Shaders deploy via a directory symlink into the repo, so game-folder shader INI edits ARE repo edits; only DLL+PDB are copied by the build; .fx edits need CompileShaders=1
metadata:
  type: feedback
---

`C:\Games\Steam\steamapps\common\Oblivion\Data\Shaders\OblivionReloaded` is a **directory symlink** to
`<repo>\OblivionReloaded\Shaders`. So shader source and the per-effect INIs (`Shadows.ini`, `POM.ini`,
…) are deployed the moment they are edited in the repo — there is no copy step. The MSBuild post-build
event copies only `OblivionReloaded.dll` + `.pdb` to `Data\OBSE\Plugins\`. The main
`OblivionReloaded.ini` lives in that Plugins folder and is NOT in the repo.

**How to apply:**

- Don't write into the game folder's shader directory directly — edit the repo copy, which is the
  source of truth.
- When Adam tunes a value in "his game folder's `Shadows.ini`", he is editing the **tracked repo file**.
  Expect it to show up in `git status`, and check its current value rather than assuming it matches the
  last commit.
- After changing a `.hlsl`, say plainly that `[Develop] CompileShaders = 1` is required (code default
  0), or the stale compiled shader is used and an in-game test silently exercises the old code.

Related: [[shader-pipeline-facts]], [[build-via-powershell-not-bash]].
