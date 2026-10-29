/* okindeo - Indeo Video 5 ("IV50") decoder for Video for Windows, built on FFmpeg's indeo5 decoder.
 *
 * Windows dropped its Indeo codecs around 2009, so games that play Indeo AVIs through Windows'
 * own player (MCIWnd/MCIAVI - e.g. Age of Empires II's intro videos) play the sound but no picture.
 * Registering this DLL as the "vidc.IV50" codec brings the picture back without touching the game's
 * files:  HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Drivers32  vidc.IV50 = <path>
 *
 * Decode only. Output: uncompressed 24- or 32-bit RGB, bottom-up or top-down.
 * FFmpeg is LGPL 2.1+; this DLL links a minimal static libavcodec (indeo5 only).
 */
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#define FCC_IV50 mmioFOURCC('I', 'V', '5', '0')
#define FCC_iv50 mmioFOURCC('i', 'v', '5', '0')

typedef struct {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    uint8_t *buf;          /* input copy with FFmpeg's required zero padding */
    int bufcap;
    int w, h;
    int have_frame;
} okind;

static int is_iv50(const BITMAPINFOHEADER *bi)
{
    return bi && (bi->biCompression == FCC_IV50 || bi->biCompression == FCC_iv50);
}

static LRESULT query(const BITMAPINFOHEADER *in, const BITMAPINFOHEADER *out)
{
    if (!is_iv50(in) || in->biWidth <= 0 || in->biHeight == 0) return ICERR_BADFORMAT;
    if (!out) return ICERR_OK;
    if (out->biCompression != BI_RGB) return ICERR_BADFORMAT;
    if (out->biBitCount != 24 && out->biBitCount != 32) return ICERR_BADFORMAT;
    if (out->biWidth != in->biWidth || labs(out->biHeight) != labs(in->biHeight)) return ICERR_BADFORMAT;
    return ICERR_OK;
}

static LRESULT get_format(const BITMAPINFOHEADER *in, BITMAPINFOHEADER *out)
{
    if (!out) return sizeof(BITMAPINFOHEADER);
    if (query(in, NULL) != ICERR_OK) return ICERR_BADFORMAT;
    memset(out, 0, sizeof(*out));
    out->biSize = sizeof(*out);
    out->biWidth = in->biWidth;
    out->biHeight = labs(in->biHeight);
    out->biPlanes = 1;
    out->biBitCount = 24;
    out->biCompression = BI_RGB;
    out->biSizeImage = ((out->biWidth * 3 + 3) & ~3) * out->biHeight;
    return ICERR_OK;
}

static void end(okind *o)
{
    if (o->ctx) avcodec_free_context(&o->ctx);
    if (o->frame) av_frame_free(&o->frame);
    if (o->pkt) av_packet_free(&o->pkt);
    o->have_frame = 0;
}

static LRESULT begin(okind *o, const BITMAPINFOHEADER *in, const BITMAPINFOHEADER *out)
{
    if (query(in, out) != ICERR_OK) return ICERR_BADFORMAT;
    end(o);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_INDEO5);
    if (!codec) return ICERR_ERROR;
    o->ctx = avcodec_alloc_context3(codec);
    o->frame = av_frame_alloc();
    o->pkt = av_packet_alloc();
    if (!o->ctx || !o->frame || !o->pkt) { end(o); return ICERR_MEMORY; }
    o->ctx->width = in->biWidth;
    o->ctx->height = labs(in->biHeight);
    if (avcodec_open2(o->ctx, codec, NULL) < 0) { end(o); return ICERR_ERROR; }
    o->w = in->biWidth;
    o->h = labs(in->biHeight);
    return ICERR_OK;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YUV 4:1:0 (or any planar YUV the decoder hands back) -> BGR(A) DIB, BT.601 limited range */
static void to_rgb(okind *o, uint8_t *dst, int bpp, int topdown)
{
    AVFrame *f = o->frame;
    int xs = 2, ys = 2;   /* chroma subsampling shifts: 4:1:0 */
    if (f->format == AV_PIX_FMT_YUV420P) xs = ys = 1;
    else if (f->format == AV_PIX_FMT_YUV422P) { xs = 1; ys = 0; }
    else if (f->format == AV_PIX_FMT_YUV444P) xs = ys = 0;
    int w = o->w < f->width ? o->w : f->width, h = o->h < f->height ? o->h : f->height;
    int px = bpp / 8, stride = ((o->w * px) + 3) & ~3;
    for (int y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)(topdown ? y : (o->h - 1 - y)) * stride;
        const uint8_t *Y = f->data[0] + (size_t)y * f->linesize[0];
        const uint8_t *U = f->data[1] + (size_t)(y >> ys) * f->linesize[1];
        const uint8_t *V = f->data[2] + (size_t)(y >> ys) * f->linesize[2];
        for (int x = 0; x < w; x++) {
            int c = 298 * (Y[x] - 16), u = U[x >> xs] - 128, v = V[x >> xs] - 128;
            uint8_t *p = row + x * px;
            p[0] = clamp8((c + 516 * u + 128) >> 8);
            p[1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
            p[2] = clamp8((c + 409 * v + 128) >> 8);
            if (px == 4) p[3] = 255;
        }
    }
}

static LRESULT decode(okind *o, const BITMAPINFOHEADER *in, const void *data, int size,
                      const BITMAPINFOHEADER *out, void *dst, int hurry)
{
    if (!o->ctx) return ICERR_ERROR;
    if (size > 0 && data) {   /* zero-size = a dropped frame: repeat the last picture */
        if (size + AV_INPUT_BUFFER_PADDING_SIZE > o->bufcap) {
            free(o->buf);
            o->bufcap = size + AV_INPUT_BUFFER_PADDING_SIZE + 4096;
            o->buf = (uint8_t *)malloc(o->bufcap);
            if (!o->buf) { o->bufcap = 0; return ICERR_MEMORY; }
        }
        memcpy(o->buf, data, size);
        memset(o->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        o->pkt->data = o->buf;
        o->pkt->size = size;
        if (avcodec_send_packet(o->ctx, o->pkt) == 0 && avcodec_receive_frame(o->ctx, o->frame) == 0)
            o->have_frame = 1;
        av_packet_unref(o->pkt);
    }
    if (hurry) return ICERR_OK;
    if (!o->have_frame) {
        memset(dst, 0, (size_t)(((o->w * (out->biBitCount / 8)) + 3) & ~3) * o->h);
        return ICERR_OK;
    }
    to_rgb(o, (uint8_t *)dst, out->biBitCount, out->biHeight < 0);
    return ICERR_OK;
}

__declspec(dllexport) LRESULT WINAPI DriverProc(DWORD_PTR id, HDRVR drv, UINT msg, LPARAM p1, LPARAM p2)
{
    okind *o = (okind *)id;
    switch (msg) {
    case DRV_LOAD: case DRV_FREE: case DRV_ENABLE: case DRV_DISABLE: case DRV_INSTALL: case DRV_REMOVE:
        return DRV_OK;
    case DRV_OPEN: {
        ICOPEN *io = (ICOPEN *)p2;
        if (io && io->fccType != ICTYPE_VIDEO && io->fccType != 0) return 0;
        okind *n = (okind *)calloc(1, sizeof(okind));
        if (!n) return 0;
        if (io) io->dwError = ICERR_OK;
        return (LRESULT)n;
    }
    case DRV_CLOSE:
        if (o) { end(o); free(o->buf); free(o); }
        return DRV_OK;
    case DRV_QUERYCONFIGURE:
        return 0;
    case ICM_GETINFO: {
        ICINFO *ii = (ICINFO *)p1;
        if (!ii || (DWORD)p2 < sizeof(ICINFO)) return 0;
        memset(ii, 0, sizeof(*ii));
        ii->dwSize = sizeof(ICINFO);
        ii->fccType = ICTYPE_VIDEO;
        ii->fccHandler = FCC_IV50;
        ii->dwVersion = 1;
        ii->dwVersionICM = ICVERSION;
        lstrcpyW(ii->szName, L"okIndeo5");
        lstrcpyW(ii->szDescription, L"Indeo Video 5 decoder (FFmpeg) for Classic Netplay for AoE2");
        return sizeof(ICINFO);
    }
    case ICM_DECOMPRESS_QUERY:
        return query((const BITMAPINFOHEADER *)p1, (const BITMAPINFOHEADER *)p2);
    case ICM_DECOMPRESS_GET_FORMAT:
        return get_format((const BITMAPINFOHEADER *)p1, (BITMAPINFOHEADER *)p2);
    case ICM_DECOMPRESS_BEGIN:
        return o ? begin(o, (const BITMAPINFOHEADER *)p1, (const BITMAPINFOHEADER *)p2) : ICERR_ERROR;
    case ICM_DECOMPRESS: {
        ICDECOMPRESS *d = (ICDECOMPRESS *)p1;
        if (!o || !d) return ICERR_ERROR;
        return decode(o, d->lpbiInput, d->lpInput, (int)d->lpbiInput->biSizeImage, d->lpbiOutput, d->lpOutput,
                      (d->dwFlags & ICDECOMPRESS_HURRYUP) != 0);
    }
    case ICM_DECOMPRESS_END:
        if (o) end(o);
        return ICERR_OK;
    /* the Ex variants: accepted only for whole-frame, unstretched decoding */
    case ICM_DECOMPRESSEX_QUERY:
    case ICM_DECOMPRESSEX_BEGIN: {
        ICDECOMPRESSEX *x = (ICDECOMPRESSEX *)p1;
        if (!x) return ICERR_BADPARAM;
        if (x->lpbiDst && (x->dxDst != x->dxSrc || x->dyDst != x->dySrc)) return ICERR_BADFORMAT;
        if (msg == ICM_DECOMPRESSEX_QUERY) return query(x->lpbiSrc, x->lpbiDst);
        return o ? begin(o, x->lpbiSrc, x->lpbiDst) : ICERR_ERROR;
    }
    case ICM_DECOMPRESSEX: {
        ICDECOMPRESSEX *x = (ICDECOMPRESSEX *)p1;
        if (!o || !x) return ICERR_ERROR;
        return decode(o, x->lpbiSrc, x->lpSrc, (int)x->lpbiSrc->biSizeImage, x->lpbiDst, x->lpDst,
                      (x->dwFlags & ICDECOMPRESS_HURRYUP) != 0);
    }
    case ICM_DECOMPRESSEX_END:
        if (o) end(o);
        return ICERR_OK;
    }
    if (msg < DRV_USER) return DefDriverProc(id, drv, msg, p1, p2);
    return ICERR_UNSUPPORTED;
}
