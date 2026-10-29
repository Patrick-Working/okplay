/* vfwprobe - can this PC decode these AVIs the way AoE2 plays them?
 *
 * AoE2 plays its intro videos with MCIWnd (Video for Windows), which can only use the
 * VfW ("vidc.*") codecs installed on the PC. This asks VfW for a decompressor and then
 * actually decodes a frame from the middle of each file, reporting its brightness so a
 * black/garbage frame can't pass as success.
 *
 *   vfwprobe [options] file.avi [file.avi ...]   -> one RESULT line per file, exit 0 if all decode
 *
 *   --codec x.dll [--fcc IV50]  load a VfW codec DLL into this process only (ICINSTALL_FUNCTION):
 *                               tests a decoder with no registry change and no admin
 *   --bpp 24|32|16|8|best       output format to ask VfW for (best = what the display wants,
 *                               like playback does); default: VfW's choice
 *   --all                       decode every frame, report failures, black frames and speed
 *   --dump N out.bmp            save decoded frame N (compare against a reference decoder)
 *   --refuse BPP                with --codec: the codec turns down that output depth, to simulate a
 *                               player asking for a format we lack (e.g. 8-bit) - does it fall back?
 *   --mci SECS                  also PLAY the first SECS seconds through MCI (the MCIAVI player
 *                               AoE2 uses) in a hidden window; with --codec, counts the frames
 *                               MCIAVI actually handed to the codec
 */
#include <windows.h>
#include <vfw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LRESULT (CALLBACK *drvproc_t)(DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM);

static drvproc_t real_proc;
static volatile LONG spy_decodes, spy_begins, spy_fail;
static int spy_bpp, spy_topdown, spy_refuse;
static volatile LONG spy_refused;

/* sits between VfW and the codec DLL: counts what the player really asks for */
static LRESULT CALLBACK spy_proc(DWORD_PTR id, HDRVR drv, UINT msg, LPARAM p1, LPARAM p2)
{
    if (spy_refuse) {
        const BITMAPINFOHEADER *want = NULL;
        if ((msg == ICM_DECOMPRESS_QUERY || msg == ICM_DECOMPRESS_BEGIN) && p2) want = (const BITMAPINFOHEADER *)p2;
        if ((msg == ICM_DECOMPRESSEX_QUERY || msg == ICM_DECOMPRESSEX_BEGIN) && p1) want = ((ICDECOMPRESSEX *)p1)->lpbiDst;
        if (want && want->biBitCount == spy_refuse) { InterlockedIncrement(&spy_refused); return ICERR_BADFORMAT; }
    }
    LRESULT r = real_proc(id, drv, msg, p1, p2);
    if (msg == ICM_DECOMPRESS || msg == ICM_DECOMPRESSEX) {
        InterlockedIncrement(&spy_decodes);
        if (r != ICERR_OK) InterlockedIncrement(&spy_fail);
    } else if (msg == ICM_DECOMPRESS_BEGIN || msg == ICM_DECOMPRESSEX_BEGIN) {
        const BITMAPINFOHEADER *out = msg == ICM_DECOMPRESS_BEGIN ? (const BITMAPINFOHEADER *)p2
                                                                  : ((ICDECOMPRESSEX *)p1)->lpbiDst;
        InterlockedIncrement(&spy_begins);
        if (out) { spy_bpp = out->biBitCount; spy_topdown = out->biHeight < 0; }
    }
    return r;
}

static int opt_all, opt_dump = -1, opt_bpp, opt_mci;
static const char *opt_dump_path;

static double brightness(const BITMAPINFOHEADER *out)
{
    const BYTE *bits = (const BYTE *)out + out->biSize + out->biClrUsed * 4;
    long w = out->biWidth, h = labs(out->biHeight), bpp = out->biBitCount;
    long stride = ((w * bpp + 31) / 32) * 4;
    double sum = 0;
    long n = 0;
    if (bpp != 24 && bpp != 32) return -1;
    for (long y = 0; y < h; y += 4)
        for (long x = 0; x < w; x += 4) {
            const BYTE *p = bits + y * stride + x * (bpp / 8);
            sum += (p[0] + p[1] + p[2]) / 3.0;
            n++;
        }
    return n ? sum / n : -1;
}

static int dump_bmp(const BITMAPINFOHEADER *out, const char *path)
{
    long stride = ((out->biWidth * out->biBitCount + 31) / 32) * 4;
    DWORD pal = out->biClrUsed ? out->biClrUsed * 4 : (out->biBitCount <= 8 ? (1u << out->biBitCount) * 4 : 0);
    DWORD bits = stride * labs(out->biHeight);
    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + out->biSize + pal;
    fh.bfSize = fh.bfOffBits + bits;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(out, out->biSize + pal + bits, 1, f);
    fclose(f);
    return 1;
}

static void mci_play(const char *path, int secs)
{
    char cmd[MAX_PATH + 128], ret[128];
    MCIERROR e;
    LONG d0 = spy_decodes;
    snprintf(cmd, sizeof(cmd), "open \"%s\" type avivideo alias okv style popup", path);
    if ((e = mciSendStringA(cmd, ret, sizeof(ret), NULL)) != 0) {
        char msg[256];
        mciGetErrorStringA(e, msg, sizeof(msg));
        printf("  mci: open failed: %s\n", msg);
        return;
    }
    mciSendStringA("window okv state hide", NULL, 0, NULL);
    mciSendStringA("set okv time format ms", NULL, 0, NULL);
    snprintf(cmd, sizeof(cmd), "play okv from 0 to %d wait", secs * 1000);
    DWORD t0 = GetTickCount();
    e = mciSendStringA(cmd, NULL, 0, NULL);
    DWORD ms = GetTickCount() - t0;
    mciSendStringA("status okv frames skipped", ret, sizeof(ret), NULL);
    char skipped[128];
    snprintf(skipped, sizeof(skipped), "%s", ret);
    mciSendStringA("close okv", NULL, 0, NULL);
    if (e) {
        char msg[256];
        mciGetErrorStringA(e, msg, sizeof(msg));
        printf("  mci: play failed: %s\n", msg);
        return;
    }
    if (real_proc)
        printf("  mci: played %d s in %lu ms, codec decoded %ld frames (%ld failed) into %dbpp %s, skipped %s%s\n",
               secs, ms, spy_decodes - d0, spy_fail, spy_bpp, spy_topdown ? "top-down" : "bottom-up",
               skipped[0] ? skipped : "?", spy_refused ? " (after refusing the first choice)" : "");
    else
        printf("  mci: played %d s in %lu ms, skipped %s\n", secs, ms, skipped[0] ? skipped : "?");
}

static int probe(const char *path)
{
    PAVIFILE pf = NULL;
    PAVISTREAM ps = NULL;
    HRESULT hr = AVIFileOpenA(&pf, path, OF_READ | OF_SHARE_DENY_NONE, NULL);
    if (hr != 0) { printf("%s: cannot open (0x%08lx)\nRESULT FAIL %s\n", path, (unsigned long)hr, path); return 0; }
    hr = AVIFileGetStream(pf, &ps, streamtypeVIDEO, 0);
    if (hr != 0) { printf("%s: no video stream\nRESULT FAIL %s\n", path, path); AVIFileRelease(pf); return 0; }

    AVISTREAMINFOA si;
    AVIStreamInfoA(ps, &si, sizeof(si));
    LONG fmtsz = 0;
    AVIStreamReadFormat(ps, 0, NULL, &fmtsz);
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)calloc(1, fmtsz > (LONG)sizeof(BITMAPINFOHEADER) ? fmtsz : sizeof(BITMAPINFOHEADER));
    AVIStreamReadFormat(ps, 0, bih, &fmtsz);
    char cc[5] = {0};
    memcpy(cc, &bih->biCompression, 4);
    LONG frames = AVIStreamLength(ps);
    double fps = si.dwScale ? (double)si.dwRate / si.dwScale : 0;
    printf("%s: %ldx%ld %s, %ld frames @ %.2f fps (%.1f s)\n", path, bih->biWidth, bih->biHeight, cc, frames, fps,
           fps > 0 ? frames / fps : 0);

    HIC hic = ICLocate(ICTYPE_VIDEO, 0, bih, NULL, ICMODE_DECOMPRESS);
    if (hic) {
        ICINFO info;
        memset(&info, 0, sizeof(info));
        info.dwSize = sizeof(info);
        ICGetInfo(hic, &info, sizeof(info));
        char name[128];
        WideCharToMultiByte(CP_ACP, 0, info.szDescription, -1, name, sizeof(name), NULL, NULL);
        printf("  decompressor: %s\n", name[0] ? name : "(unnamed)");
        ICClose(hic);
    } else {
        printf("  decompressor: NONE installed for '%s'\n", cc);
    }

    BITMAPINFOHEADER want, *pwant = NULL;
    if (opt_bpp == -1) {
        pwant = (BITMAPINFOHEADER *)AVIGETFRAMEF_BESTDISPLAYFMT;
    } else if (opt_bpp) {
        memset(&want, 0, sizeof(want));
        want.biSize = sizeof(want);
        want.biWidth = bih->biWidth;
        want.biHeight = labs(bih->biHeight);
        want.biPlanes = 1;
        want.biBitCount = (WORD)opt_bpp;
        want.biCompression = BI_RGB;
        pwant = &want;
    }

    int ok = 0;
    PGETFRAME gf = AVIStreamGetFrameOpen(ps, pwant);
    if (!gf) {
        printf("  decode: AVIStreamGetFrameOpen failed - VfW cannot decode this video%s\n",
               opt_bpp ? " into that format" : "");
    } else {
        BITMAPINFOHEADER *out = (BITMAPINFOHEADER *)AVIStreamGetFrame(gf, frames / 2);
        if (!out) {
            printf("  decode: frame %ld failed\n", frames / 2);
        } else {
            double avg = brightness(out);
            printf("  decode: frame %ld OK, %ldx%ld %dbpp, average brightness %.1f/255\n", frames / 2, out->biWidth,
                   labs(out->biHeight), out->biBitCount, avg);
            ok = avg > 3.0 || out->biBitCount < 24;   /* a real picture, not an all-black frame */
        }
        if (ok && opt_all) {
            LONG bad = 0, black = 0;
            DWORD t0 = GetTickCount();
            for (LONG i = 0; i < frames; i++) {
                BITMAPINFOHEADER *f = (BITMAPINFOHEADER *)AVIStreamGetFrame(gf, i);
                if (!f) { if (bad++ < 3) printf("  all: frame %ld FAILED\n", i); continue; }
                if (brightness(f) >= 0 && brightness(f) < 1.0) black++;
            }
            DWORD ms = GetTickCount() - t0;
            printf("  all: %ld frames, %ld failed, %ld black, %lu ms (%.0f fps; the video needs %.2f)\n", frames, bad,
                   black, ms, ms ? frames * 1000.0 / ms : 0, fps);
            if (bad) ok = 0;
        }
        if (ok && opt_dump >= 0) {
            BITMAPINFOHEADER *f = (BITMAPINFOHEADER *)AVIStreamGetFrame(gf, opt_dump);
            if (f && dump_bmp(f, opt_dump_path)) printf("  dump: frame %d -> %s\n", opt_dump, opt_dump_path);
            else { printf("  dump: frame %d FAILED\n", opt_dump); ok = 0; }
        }
        AVIStreamGetFrameClose(gf);
    }
    free(bih);
    AVIStreamRelease(ps);
    AVIFileRelease(pf);
    if (opt_mci) mci_play(path, opt_mci);
    printf("RESULT %s %s\n", ok ? "DECODES" : "NO_PICTURE", path);
    return ok;
}

int main(int argc, char **argv)
{
    const char *codec = NULL, *fcc = "IV50";
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--codec") && i + 1 < argc) codec = argv[++i];
        else if (!strcmp(argv[i], "--fcc") && i + 1 < argc) fcc = argv[++i];
        else if (!strcmp(argv[i], "--all")) opt_all = 1;
        else if (!strcmp(argv[i], "--bpp") && i + 1 < argc) { i++; opt_bpp = !strcmp(argv[i], "best") ? -1 : atoi(argv[i]); }
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) { opt_dump = atoi(argv[i + 1]); opt_dump_path = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--mci") && i + 1 < argc) opt_mci = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--refuse") && i + 1 < argc) spy_refuse = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (codec) {
        HMODULE m = LoadLibraryA(codec);
        real_proc = m ? (drvproc_t)GetProcAddress(m, "DriverProc") : NULL;
        if (!real_proc) { printf("codec %s: cannot load DriverProc (error %lu)\n", codec, GetLastError()); return 2; }
        if (strlen(fcc) != 4 ||
            !ICInstall(ICTYPE_VIDEO, mmioFOURCC(fcc[0], fcc[1], fcc[2], fcc[3]), (LPARAM)spy_proc, NULL, ICINSTALL_FUNCTION)) {
            printf("codec %s: ICInstall failed\n", codec);
            return 2;
        }
        printf("codec %s installed for '%s' in this process only\n", codec, fcc);
    }
    AVIFileInit();
    int all = 1;
    for (; i < argc; i++) all &= probe(argv[i]);
    AVIFileExit();
    return all ? 0 : 1;
}
