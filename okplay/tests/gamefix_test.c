/* gamefix_test: loads the drop-in's dplayx.dll from its own folder and exits, so the game-fix
 * check (winedplay/shim/gamefix.c) runs against this exe. gamefix_test.sh runs it under other
 * names and sizes - named like a fixed game but not that exact file, it must be left alone. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    char path[MAX_PATH], *slash;
    HMODULE m;
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash) lstrcpyA(slash + 1, "dplayx.dll");
    m = LoadLibraryA(path);
    printf("%s %s\n", path, m ? "loaded" : "NOT loaded");
    if (m) FreeLibrary(m);
    return m ? 0 : 1;
}
