---
name: build-via-powershell-not-bash
description: "Run the MSBuild command through the PowerShell tool, never Bash — Bash's TEMP is unexpanded and causes a fake MSB3073"
metadata:
  type: project
---

Run the project's MSBuild command through the **PowerShell tool**, not the Bash tool.

**Why:** the Bash tool's shell has `TEMP`/`TMP` set to the literal unexpanded string `%TEMP%` (which
is also why a `%TEMP%/` directory appears in the repo root and is gitignored). That breaks MSBuild's
`PostBuildEvent` copy step with an `MSB3073` error that looks **identical** to "the game is running
and holding `OblivionReloaded.dll`". The same command run via PowerShell builds and copies cleanly.

**How to apply:** when a build fails with MSB3073, check which tool ran it before concluding the DLL is
locked. If it was Bash, re-run it through PowerShell rather than asking Adam to close the game.

Related: [[shader-deployment-workflow]] — the build copies only the DLL and PDB.
