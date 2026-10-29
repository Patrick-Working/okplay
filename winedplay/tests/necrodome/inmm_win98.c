/* _inmm.dll for the GOG Necrodome on Windows 95/98 (test VMs, old PCs): GOG renamed the exe's WINMM.dll
 * import to its own _inmm.dll (CD music from ogg files), which needs SSE and libvorbis - neither on a
 * Pentium MMX. This one hands every function the exe imports straight to Windows' winmm.dll, so the exe
 * stays byte for byte GOG's (the drop-in's game fix checks that) and CD music is simply off.
 * Build: gcc -O1 -shared -o _inmm.dll inmm_win98.c inmm_win98.def   (mingw32; check9x it)
 * LGPL 2.1+, like the rest of the drop-in. */
#include <windows.h>
void *p_auxGetDevCapsA;
__asm__(".globl _t_auxGetDevCapsA; _t_auxGetDevCapsA: jmp *_p_auxGetDevCapsA");
void *p_auxGetNumDevs;
__asm__(".globl _t_auxGetNumDevs; _t_auxGetNumDevs: jmp *_p_auxGetNumDevs");
void *p_auxGetVolume;
__asm__(".globl _t_auxGetVolume; _t_auxGetVolume: jmp *_p_auxGetVolume");
void *p_auxSetVolume;
__asm__(".globl _t_auxSetVolume; _t_auxSetVolume: jmp *_p_auxSetVolume");
void *p_joyGetDevCapsA;
__asm__(".globl _t_joyGetDevCapsA; _t_joyGetDevCapsA: jmp *_p_joyGetDevCapsA");
void *p_joyGetPosEx;
__asm__(".globl _t_joyGetPosEx; _t_joyGetPosEx: jmp *_p_joyGetPosEx");
void *p_mciGetErrorStringA;
__asm__(".globl _t_mciGetErrorStringA; _t_mciGetErrorStringA: jmp *_p_mciGetErrorStringA");
void *p_mciSendCommandA;
__asm__(".globl _t_mciSendCommandA; _t_mciSendCommandA: jmp *_p_mciSendCommandA");
void *p_midiOutGetVolume;
__asm__(".globl _t_midiOutGetVolume; _t_midiOutGetVolume: jmp *_p_midiOutGetVolume");
void *p_midiOutPrepareHeader;
__asm__(".globl _t_midiOutPrepareHeader; _t_midiOutPrepareHeader: jmp *_p_midiOutPrepareHeader");
void *p_midiOutReset;
__asm__(".globl _t_midiOutReset; _t_midiOutReset: jmp *_p_midiOutReset");
void *p_midiOutSetVolume;
__asm__(".globl _t_midiOutSetVolume; _t_midiOutSetVolume: jmp *_p_midiOutSetVolume");
void *p_midiOutUnprepareHeader;
__asm__(".globl _t_midiOutUnprepareHeader; _t_midiOutUnprepareHeader: jmp *_p_midiOutUnprepareHeader");
void *p_midiStreamClose;
__asm__(".globl _t_midiStreamClose; _t_midiStreamClose: jmp *_p_midiStreamClose");
void *p_midiStreamOpen;
__asm__(".globl _t_midiStreamOpen; _t_midiStreamOpen: jmp *_p_midiStreamOpen");
void *p_midiStreamOut;
__asm__(".globl _t_midiStreamOut; _t_midiStreamOut: jmp *_p_midiStreamOut");
void *p_midiStreamPause;
__asm__(".globl _t_midiStreamPause; _t_midiStreamPause: jmp *_p_midiStreamPause");
void *p_midiStreamProperty;
__asm__(".globl _t_midiStreamProperty; _t_midiStreamProperty: jmp *_p_midiStreamProperty");
void *p_midiStreamRestart;
__asm__(".globl _t_midiStreamRestart; _t_midiStreamRestart: jmp *_p_midiStreamRestart");
void *p_midiStreamStop;
__asm__(".globl _t_midiStreamStop; _t_midiStreamStop: jmp *_p_midiStreamStop");
void *p_timeBeginPeriod;
__asm__(".globl _t_timeBeginPeriod; _t_timeBeginPeriod: jmp *_p_timeBeginPeriod");
void *p_timeEndPeriod;
__asm__(".globl _t_timeEndPeriod; _t_timeEndPeriod: jmp *_p_timeEndPeriod");
void *p_timeGetTime;
__asm__(".globl _t_timeGetTime; _t_timeGetTime: jmp *_p_timeGetTime");
void *p_waveOutGetVolume;
__asm__(".globl _t_waveOutGetVolume; _t_waveOutGetVolume: jmp *_p_waveOutGetVolume");
void *p_waveOutSetVolume;
__asm__(".globl _t_waveOutSetVolume; _t_waveOutSetVolume: jmp *_p_waveOutSetVolume");

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
    if (why == DLL_PROCESS_ATTACH) {
        HMODULE w = LoadLibraryA("winmm.dll");
        if (!w) return FALSE;
        p_auxGetDevCapsA = (void *)GetProcAddress(w, "auxGetDevCapsA");
        p_auxGetNumDevs = (void *)GetProcAddress(w, "auxGetNumDevs");
        p_auxGetVolume = (void *)GetProcAddress(w, "auxGetVolume");
        p_auxSetVolume = (void *)GetProcAddress(w, "auxSetVolume");
        p_joyGetDevCapsA = (void *)GetProcAddress(w, "joyGetDevCapsA");
        p_joyGetPosEx = (void *)GetProcAddress(w, "joyGetPosEx");
        p_mciGetErrorStringA = (void *)GetProcAddress(w, "mciGetErrorStringA");
        p_mciSendCommandA = (void *)GetProcAddress(w, "mciSendCommandA");
        p_midiOutGetVolume = (void *)GetProcAddress(w, "midiOutGetVolume");
        p_midiOutPrepareHeader = (void *)GetProcAddress(w, "midiOutPrepareHeader");
        p_midiOutReset = (void *)GetProcAddress(w, "midiOutReset");
        p_midiOutSetVolume = (void *)GetProcAddress(w, "midiOutSetVolume");
        p_midiOutUnprepareHeader = (void *)GetProcAddress(w, "midiOutUnprepareHeader");
        p_midiStreamClose = (void *)GetProcAddress(w, "midiStreamClose");
        p_midiStreamOpen = (void *)GetProcAddress(w, "midiStreamOpen");
        p_midiStreamOut = (void *)GetProcAddress(w, "midiStreamOut");
        p_midiStreamPause = (void *)GetProcAddress(w, "midiStreamPause");
        p_midiStreamProperty = (void *)GetProcAddress(w, "midiStreamProperty");
        p_midiStreamRestart = (void *)GetProcAddress(w, "midiStreamRestart");
        p_midiStreamStop = (void *)GetProcAddress(w, "midiStreamStop");
        p_timeBeginPeriod = (void *)GetProcAddress(w, "timeBeginPeriod");
        p_timeEndPeriod = (void *)GetProcAddress(w, "timeEndPeriod");
        p_timeGetTime = (void *)GetProcAddress(w, "timeGetTime");
        p_waveOutGetVolume = (void *)GetProcAddress(w, "waveOutGetVolume");
        p_waveOutSetVolume = (void *)GetProcAddress(w, "waveOutSetVolume");
    }
    return TRUE;
}
