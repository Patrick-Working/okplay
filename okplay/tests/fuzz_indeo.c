/* fuzz_indeo - feed okindeo.dll corrupted Indeo 5 frames the way Video for Windows would.
 *
 *   fuzz_indeo okindeo.dll sample.avi [iterations] [seed]
 *   fuzz_indeo okindeo.dll sample.avi 100 --control     tells the codec the frame is 4 KB longer than
 *                                                       it is - must be CAUGHT (proves the trap works)
 *
 * Real frames from the AVI are mutated (bit flips, byte runs, truncation, splices) and decoded
 * through DriverProc. Each input ends exactly at a PAGE_NOACCESS page, so reading past the frame
 * faults; any crash is reported with the input saved to out/.
 */
#include <windows.h>
#include <vfw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LRESULT (CALLBACK *drvproc_t)(DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM);

static unsigned long long rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 16); }

static BYTE *cur_input;
static int cur_size;
static long iter;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    printf("CRASH at iteration %ld: exception 0x%08lx at %p (address %p), input %d bytes\n", iter, code,
           e->ExceptionRecord->ExceptionAddress,
           e->ExceptionRecord->NumberParameters > 1 ? (void *)e->ExceptionRecord->ExceptionInformation[1] : NULL, cur_size);
    CreateDirectoryA("out", NULL);
    FILE *f = fopen("out/fuzz-indeo-crash.bin", "wb");
    if (f) { fwrite(cur_input, 1, cur_size, f); fclose(f); printf("saved out/fuzz-indeo-crash.bin\n"); }
    printf("RESULT FAIL\n");
    fflush(stdout);
    ExitProcess(1);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: fuzz_indeo okindeo.dll sample.avi [iterations] [seed|--control]\n"); return 2; }
    long iters = argc > 3 ? atol(argv[3]) : 20000;
    int control = argc > 4 && !strcmp(argv[4], "--control");
    rng = argc > 4 && !control ? strtoull(argv[4], NULL, 0) : GetTickCount64() * 2654435761ull;
    if (!rng) rng = 1;
    printf("seed 0x%llx, %ld iterations%s\n", rng, iters, control ? " (CONTROL: lies about the frame size)" : "");

    HMODULE m = LoadLibraryA(argv[1]);
    drvproc_t dp = m ? (drvproc_t)GetProcAddress(m, "DriverProc") : NULL;
    if (!dp) { printf("cannot load DriverProc from %s\n", argv[1]); return 2; }

    /* every frame of the sample, as stored in the file */
    AVIFileInit();
    PAVIFILE pf;
    PAVISTREAM ps;
    if (AVIFileOpenA(&pf, argv[2], OF_READ | OF_SHARE_DENY_NONE, NULL) || AVIFileGetStream(pf, &ps, streamtypeVIDEO, 0)) {
        printf("cannot open %s\n", argv[2]);
        return 2;
    }
    LONG fmtsz = 0;
    AVIStreamReadFormat(ps, 0, NULL, &fmtsz);
    BITMAPINFOHEADER *in = (BITMAPINFOHEADER *)calloc(1, fmtsz + 64);
    AVIStreamReadFormat(ps, 0, in, &fmtsz);
    LONG nframes = AVIStreamLength(ps);
    BYTE **frames = (BYTE **)calloc(nframes, sizeof(BYTE *));
    LONG *sizes = (LONG *)calloc(nframes, sizeof(LONG));
    int maxsz = 0;
    for (LONG i = 0; i < nframes; i++) {
        LONG sz = 0;
        AVIStreamRead(ps, i, 1, NULL, 0, &sz, NULL);
        frames[i] = (BYTE *)malloc(sz + 1);
        AVIStreamRead(ps, i, 1, frames[i], sz, &sizes[i], NULL);
        if (sizes[i] > maxsz) maxsz = sizes[i];
    }
    printf("%ld frames, largest %d bytes\n", nframes, maxsz);

    /* guard-page input buffer: data is placed so its last byte is the last readable byte */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t cap = ((size_t)maxsz * 2 + 65536 + si.dwPageSize - 1) & ~(size_t)(si.dwPageSize - 1);
    BYTE *region = (BYTE *)VirtualAlloc(NULL, cap + si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(region + cap, si.dwPageSize, PAGE_NOACCESS, &old);
    BYTE *work = (BYTE *)malloc(cap);

    BITMAPINFOHEADER out;
    memset(&out, 0, sizeof(out));
    out.biSize = sizeof(out);
    out.biWidth = in->biWidth;
    out.biHeight = labs(in->biHeight);
    out.biPlanes = 1;
    out.biBitCount = 24;
    out.biSizeImage = ((out.biWidth * 3 + 3) & ~3) * out.biHeight;
    BYTE *pixels = (BYTE *)malloc(out.biSizeImage);

    AddVectoredExceptionHandler(1, on_fault);
    ICOPEN io;
    memset(&io, 0, sizeof(io));
    io.dwSize = sizeof(io);
    io.fccType = ICTYPE_VIDEO;
    io.dwFlags = ICMODE_DECOMPRESS;
    DWORD_PTR h = (DWORD_PTR)dp(0, NULL, DRV_OPEN, 0, (LPARAM)&io);
    if (!h || dp(h, NULL, ICM_DECOMPRESS_BEGIN, (LPARAM)in, (LPARAM)&out) != ICERR_OK) { printf("codec refused the format\n"); return 2; }

    long decoded_ok = 0, restarts = 0;
    DWORD t0 = GetTickCount();
    for (iter = 0; iter < iters; iter++) {
        LONG fi = rnd() % nframes;
        int n = sizes[fi];
        memcpy(work, frames[fi], n);
        switch (control ? 9 : rnd() % 6) {
        case 0:   /* flip a few bits */
            for (int k = 1 + rnd() % 8; k-- && n;) work[rnd() % n] ^= (BYTE)(1 << (rnd() % 8));
            break;
        case 1:   /* overwrite a run with one value */
            if (n) { int at = rnd() % n, len = 1 + rnd() % 64; memset(work + at, (BYTE)rnd(), at + len > n ? n - at : len); }
            break;
        case 2:   /* truncate */
            n = n ? rnd() % n : 0;
            break;
        case 3:   /* random header bytes (the picture/band headers live up front) */
            for (int k = 0; k < 24 && k < n; k++) if (rnd() % 3 == 0) work[k] = (BYTE)rnd();
            break;
        case 4: { /* splice the tail of another frame */
            LONG fj = rnd() % nframes;
            int at = n ? rnd() % n : 0, take = sizes[fj] ? rnd() % sizes[fj] : 0;
            if (at + take > (int)cap) take = (int)cap - at;
            memcpy(work + at, frames[fj], take);
            n = at + take;
            break; }
        case 5:   /* pure noise */
            n = rnd() % 4096;
            for (int k = 0; k < n; k++) work[k] = (BYTE)rnd();
            break;
        }
        cur_input = region + cap - n;
        cur_size = n;
        memcpy(cur_input, work, n);
        in->biSizeImage = control ? n + 4096 : n;
        ICDECOMPRESS d;
        memset(&d, 0, sizeof(d));
        d.lpbiInput = in;
        d.lpInput = cur_input;
        d.lpbiOutput = &out;
        d.lpOutput = pixels;
        if (dp(h, NULL, ICM_DECOMPRESS, (LPARAM)&d, sizeof(d)) == ICERR_OK) decoded_ok++;
        if (rnd() % 500 == 0) {   /* the player restarts the stream now and then */
            dp(h, NULL, ICM_DECOMPRESS_END, 0, 0);
            dp(h, NULL, ICM_DECOMPRESS_BEGIN, (LPARAM)in, (LPARAM)&out);
            restarts++;
        }
    }
    dp(h, NULL, ICM_DECOMPRESS_END, 0, 0);
    dp(h, NULL, DRV_CLOSE, 0, 0);
    printf("%ld corrupted frames, %ld returned OK, %ld restarts, no faults (%lu ms)\nRESULT ok\n", iters, decoded_ok,
           restarts, GetTickCount() - t0);
    return 0;
}
