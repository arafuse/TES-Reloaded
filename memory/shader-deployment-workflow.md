---
name: shader-deployment-workflow
description: Shaders deploy via a directory symlink into the repo, so game-folder INI edits ARE repo edits; only DLL+PDB are copied by the build
metadata:
  node_type: memory
  type: feedback
  originSessionId: f67b9041-017f-4383-84d6-b0dc1184859b
  modified: 2026-07-28T01:32:31.366Z
---

`C:\Games\Steam\steamapps\common\Oblivion\Data\Shaders\OblivionReloaded` is a **directory symlink** to
`<repo>\OblivionReloaded\Shaders` (verified 2026-07-27 with `ls -la`; `CLAUDE.md` states the same).
So shader source and `Shadows.ini` are deployed the moment they are edited in the repo — there is no
copy step. The MSBuild post-build event copies only `OblivionReloaded.dll` + `.pdb` to
`Data\OBSE\Plugins\`.

**This supersedes an earlier version of this memory** which claimed Adam copies the `Shaders` directory
by hand before each test. That was wrong; the symlink predates it.

**How to apply:**

- Don't write into the game folder directly — edit the repo copy, which is the source of truth.
- When Adam tunes a value in "his game folder's `Shadows.ini`", he is editing the **tracked repo file**.
  Expect it to show up in `git status`, and check its current value rather than assuming it matches the
  last commit. During the far-plane-shadows work it sat at a diagnostic value for several sessions.
- After changing a `.fx.hlsl`, say plainly that `[Develop] CompileShaders = 1` is required (default 0),
  or the stale compiled `.fx` is used and an in-game test silently exercises the old shader. A code
  review once caught exactly that: a verification would have "passed" against a 3-day-old shader.

Related: [[shader-pipeline-facts]], [[build-via-powershell-not-bash]].
