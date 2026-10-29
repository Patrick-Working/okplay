/* moviehost - stands in for a launcher that plays a game's intro movies itself (Steam's Galactic
 * Battlegrounds player.exe): a program that uses version.dll and MCI, and never loads dplayx.dll.
 * moviehelper_test.sh runs it without and with codec/moviehelper's version.dll next to it.
 *
 *   moviehost movie.avi [seconds]   -> RESULT PICTURE (exit 0) / NO_PICTURE (exit 1) / BROKEN_VERSION (exit 2)
 */
#include <windows.h>
#include <vfw.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    char sys[MAX_PATH], cmd[MAX_PATH + 64], err[200] = "";
    DWORD handle, size, secs = argc > 2 ? atoi(argv[2]) : 2;
    VS_FIXEDFILEINFO *ffi = NULL;
    UINT len = 0;
    BYTE *buf;
    HIC ic;
    MCIERROR e;

    if (argc < 2) { printf("usage: moviehost movie.avi [seconds]\n"); return 2; }
    /* version.dll must behave exactly as Windows' own: read kernel32's version through it */
    GetSystemDirectoryA(sys, sizeof(sys));
    strcat(sys, "\\kernel32.dll");
    size = GetFileVersionInfoSizeA(sys, &handle);
    buf = malloc(size + 1);
    if (!size || !GetFileVersionInfoA(sys, 0, size, buf) || !VerQueryValueA(buf, "\\", (void **)&ffi, &len) || !ffi ||
        !HIWORD(ffi->dwFileVersionMS)) {
        printf("version.dll calls FAILED (size %lu)\nRESULT BROKEN_VERSION\n", size);
        return 2;
    }
    printf("version.dll: kernel32 is %u.%u.%u.%u\n", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
           HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));

    Sleep(100);   /* a real player spends far longer creating its window before its first movie */
    ic = ICOpen(ICTYPE_VIDEO, mmioFOURCC('i', 'v', '5', '0'), ICMODE_DECOMPRESS);
    printf("Indeo 5 decoder: %s\n", ic ? "opens" : "NONE");
    if (ic) ICClose(ic);

    snprintf(cmd, sizeof(cmd), "open \"%s\" type avivideo alias m", argv[1]);
    if ((e = mciSendStringA(cmd, NULL, 0, NULL))) { mciGetErrorStringA(e, err, sizeof(err)); printf("mci open: %s\n", err); }
    else {
        mciSendStringA("window m state hide", NULL, 0, NULL);
        mciSendStringA("play m", NULL, 0, NULL);
        Sleep(secs * 1000);
        mciSendStringA("close m", NULL, 0, NULL);
        printf("mci: played %lu s\n", secs);
    }
    printf("RESULT %s\n", ic ? "PICTURE" : "NO_PICTURE");
    return ic ? 0 : 1;
}
