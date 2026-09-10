---
name: build-via-powershell-not-bash
description: "Run the MSBuild command through the PowerShell tool, never Bash — Bash's TEMP is unexpanded and causes a fake MSB3073"
metadata: 
  node_type: memory
  type: project
  originSessionId: 632d7927-cb5d-4cb3-aee3-433d81d5e723
  modified: 2026-07-27T20:11:27.892Z
---

Run the project's MSBuild command through the **PowerShell tool**, not the Bash tool.

**Why:** the Bash tool's shell has `TEMP`/`TMP` set to the literal unexpanded string `%TEMP%`. That breaks
MSBuild's `PostBuildEvent` copy step with an `MSB3073` error that looks **identical** to "the game is
running and holding `OblivionReloaded.dll`". The same command run via the PowerShell tool builds and
copies cleanly.

**How to apply:** when a build fails with MSB3073, check which tool ran it before concluding the DLL is
locked. If it was Bash, re-run it through PowerShell rather than asking Adam to close the game. This cost
a subagent a false diagnosis on 2026-07-27.

Related: [[shader-deployment-workflow]] — the build copies only the DLL and PDB.
