/* winweb_names.h - private names for everything our DirectPlay library defines that a game may
 * define too: a browser game often has its own little Windows layer (rogan2 defines GetLastError,
 * GetProcAddress...) and its own DirectPlay entry points. Every source of the library sees these
 * names (winweb.h includes this first), so the symbols in the library are winweb_* / okweb_*. */
#ifndef WINWEB_NAMES_H
#define WINWEB_NAMES_H

/* the 64 Windows functions (winweb.c) */
#define CLSIDFromString winweb_CLSIDFromString
#define CloseHandle winweb_CloseHandle
#define CoCreateGuid winweb_CoCreateGuid
#define CreateEventA winweb_CreateEventA
#define CreateEventW winweb_CreateEventW
#define CreateFileMappingA winweb_CreateFileMappingA
#define CreateProcessA winweb_CreateProcessA
#define CreateSemaphoreA winweb_CreateSemaphoreA
#define CreateThread winweb_CreateThread
#define DebugBreak winweb_DebugBreak
#define DeleteCriticalSection winweb_DeleteCriticalSection
#define DisableThreadLibraryCalls winweb_DisableThreadLibraryCalls
#define DuplicateHandle winweb_DuplicateHandle
#define EnterCriticalSection winweb_EnterCriticalSection
#define FlushViewOfFile winweb_FlushViewOfFile
#define FreeLibrary winweb_FreeLibrary
#define GetCurrentProcess winweb_GetCurrentProcess
#define GetCurrentProcessId winweb_GetCurrentProcessId
#define GetCurrentThreadId winweb_GetCurrentThreadId
#define TlsAlloc winweb_TlsAlloc
#define TlsGetValue winweb_TlsGetValue
#define TlsSetValue winweb_TlsSetValue
#define GetEnvironmentVariableA winweb_GetEnvironmentVariableA
#define GetLastError winweb_GetLastError
#define GetLocalTime winweb_GetLocalTime
#define GetMessageW winweb_GetMessageW
#define GetModuleFileNameA winweb_GetModuleFileNameA
#define GetModuleHandleA winweb_GetModuleHandleA
#define GetProcAddress winweb_GetProcAddress
#define GetTickCount winweb_GetTickCount
#define GetVersion winweb_GetVersion
#define InitOnceExecuteOnce winweb_InitOnceExecuteOnce
#define InitializeCriticalSection winweb_InitializeCriticalSection
#define IsBadStringPtrA winweb_IsBadStringPtrA
#define IsBadStringPtrW winweb_IsBadStringPtrW
#define LeaveCriticalSection winweb_LeaveCriticalSection
#define LoadLibraryA winweb_LoadLibraryA
#define MapViewOfFile winweb_MapViewOfFile
#define MapViewOfFileEx winweb_MapViewOfFileEx
#define MultiByteToWideChar winweb_MultiByteToWideChar
#define RegCloseKey winweb_RegCloseKey
#define RegEnumKeyExA winweb_RegEnumKeyExA
#define RegGetValueA winweb_RegGetValueA
#define RegGetValueW winweb_RegGetValueW
#define RegOpenKeyExA winweb_RegOpenKeyExA
#define RegQueryValueExA winweb_RegQueryValueExA
#define ReleaseSemaphore winweb_ReleaseSemaphore
#define ResetEvent winweb_ResetEvent
#define ResumeThread winweb_ResumeThread
#define SetEvent winweb_SetEvent
#define SetLastError winweb_SetLastError
#define Sleep winweb_Sleep
#define SleepEx winweb_SleepEx
#define TerminateThread winweb_TerminateThread
#define UnmapViewOfFile winweb_UnmapViewOfFile
#define VirtualProtect winweb_VirtualProtect
#define VirtualQuery winweb_VirtualQuery
#define WaitForSingleObject winweb_WaitForSingleObject
#define WideCharToMultiByte winweb_WideCharToMultiByte
#define lstrcmpW winweb_lstrcmpW
#define lstrcpyW winweb_lstrcpyW
#define lstrcpynA winweb_lstrcpynA
#define lstrcpynW winweb_lstrcpynW
#define lstrlenA winweb_lstrlenA
#define lstrlenW winweb_lstrlenW
#define timeGetTime winweb_timeGetTime
#define wsprintfA winweb_wsprintfA

/* DirectPlay's own entry points (a game reaches them through okweb_dplay.h) */
#define DirectPlayCreate okweb_DirectPlayCreate
#define DirectPlayEnumerateA okweb_DirectPlayEnumerateA
#define DirectPlayEnumerateW okweb_DirectPlayEnumerateW
#define DirectPlayLobbyCreateA okweb_DirectPlayLobbyCreateA
#define DirectPlayLobbyCreateW okweb_DirectPlayLobbyCreateW
#define DllGetClassObject okweb_DllGetClassObject
#define DllMain okweb_DllMain

#endif
