#pragma once

class Logger {
public:
	static void CreateLog(const char* FileName);
	static void Log(const char* Message, ...);
	
	static char			MessageBuffer[4096];
	static FILE*		LogFile;

};