#pragma once

#if defined(NEWVEGAS)
class PluginInterface {
public:
	UInt32	Version;
	UInt32	GameVersion;
	UInt32	EditorVersion;
	UInt32	IsEditor;

	bool	(*RegisterCommand)(CommandInfo* info);
	void	(*SetOpcodeBase)(UInt32 opcode);
	void*	(*QueryInterface)(UInt32 id);
};

class PluginInfo {
public:
	enum {
		kInfoVersion = 1
	};

	UInt32		InfoVersion;
	const char* Name;
	UInt32		Version;
};
#elif defined(OBLIVION)
typedef UInt32 PluginHandle;

class PluginInterface {
public:
	enum InterfaceType {
		kInterface_Console = 0,
		kInterface_Serialization,
	};

	UInt32	Version;
	UInt32	GameVersion;
	UInt32	EditorVersion;
	UInt32	IsEditor;

	bool	(*RegisterCommand)(CommandInfo* info);
	void	(*SetOpcodeBase)(UInt32 opcode);
	void*	(*QueryInterface)(UInt32 id);
	PluginHandle (*GetPluginHandle)();	///< OBSE v15+; only valid during Query or Load
};

/// OBSE's co-save (the .obse file written beside each .ess). Records are keyed by the plugin's opcode
/// base, so SetOpcodeBase must have been called before any data is written.
class SerializationInterface {
public:
	enum {
		kVersion = 1
	};

	typedef void (*EventCallback)(void* Reserved);

	UInt32	Version;
	void	(*SetSaveCallback)(PluginHandle Plugin, EventCallback Callback);
	void	(*SetLoadCallback)(PluginHandle Plugin, EventCallback Callback);
	void	(*SetNewGameCallback)(PluginHandle Plugin, EventCallback Callback);

	bool	(*WriteRecord)(UInt32 Type, UInt32 Version, const void* Buffer, UInt32 Length);
	bool	(*OpenRecord)(UInt32 Type, UInt32 Version);
	bool	(*WriteRecordData)(const void* Buffer, UInt32 Length);

	bool	(*GetNextRecordInfo)(UInt32* Type, UInt32* Version, UInt32* Length);
	UInt32	(*ReadRecordData)(void* Buffer, UInt32 Length);

	/// Remaps a refID stored in the save to the current load order; false if its plugin is no longer loaded.
	bool	(*ResolveRefID)(UInt32 RefID, UInt32* OutRefID);
};

class PluginInfo {
public:
	enum {
		kInfoVersion = 2
	};

	UInt32		InfoVersion;
	const char* Name;
	UInt32		Version;
};
#elif defined(SKYRIM)
class PluginInterface {
public:
	enum InterfaceType {
		kInterface_Invalid = 0,
		kInterface_Scaleform,
		kInterface_Papyrus,
		kInterface_Serialization,
		kInterface_Task,
		kInterface_Messaging,
		kInterface_Max,
	};

	UInt32	Version;
	UInt32	GameVersion;
	UInt32	EditorVersion;
	UInt32	IsEditor;

	void*	(*QueryInterface)(UInt32 id);
};

class PluginInfo {
public:
	enum {
		kInfoVersion = 1
	};

	UInt32		InfoVersion;
	const char* Name;
	UInt32		Version;
};

class PapyrusInterface {
public:
	typedef bool (*RegisterFunctions)(VMClassRegistry* registry);

	enum {
		kInterfaceVersion = 1
	};

	UInt32	interfaceVersion;
	bool	(*Register)(RegisterFunctions callback);
};
#endif