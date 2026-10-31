#pragma once

/// Persists plugin state in OBSE's co-save (the .obse file beside each .ess). OBSE allows one save,
/// load and new-game callback per plugin, so every record is written and read here. Records:
/// - 'WIND' v1: WindState (Wind.h).
/// Loading a save without a record, or starting a new game, resets that state. Needs OBSE v15+ and
/// the opcode base set in CommandManager::AddCommands; without them nothing is saved and this logs why.
void CreateCoSave(const PluginInterface* Interface);
