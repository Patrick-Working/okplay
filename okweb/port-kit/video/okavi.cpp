// okavi - a game's AVI movies in the browser: Indeo Video 5 (IV50, which no browser decodes) through
// FFmpeg's indeo5 decoder (LGPL; build_ffmpeg_indeo5_wasm.sh), PCM sound (8/16-bit, mono/stereo) through
// WebAudio, drawn on a canvas over the game's. Generic: a port puts its own window layer on top (rogan2
// emulates Windows' MCIWnd with it: see ../README.md). Frames are decoded in okavi_tick(): call it once
// per game frame.
//
//   if (okavi_open("avi/intro.avi")) { okavi_place(x, y, w, h, screen_w, screen_h); okavi_play(); }
//   each frame: okavi_tick();   when okavi_mode() == OKAVI_STOPPED: okavi_close();
//
// The canvas goes into Module.okaviParent (else #game-container, else the canvas's parent), positioned
// in percent of the game's screen. Build with -I<ffmpeg>/include, link libavcodec.a libavutil.a.
#ifdef __EMSCRIPTEN__
#include "okavi.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

struct Movie
{
	bool open;
	FILE *f;
	int w, h;
	double fps;
	std::vector<std::pair<long, uint32_t>> frames;   // video chunks: file offset, size
	std::vector<float> sound;        // the whole soundtrack, -1..1, channels interleaved
	int sound_rate, sound_channels, sound_bits;
	enum { STOPPED, PLAYING, PAUSED } mode;
	double started_ms, paused_ms;    // the movie clock: now - started (less the pauses)
	int next_frame;                  // the next frame to decode
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	std::vector<uint8_t> chunk;      // a frame's data with FFmpeg's zero padding
	std::vector<uint8_t> rgba;
};

static Movie g_movie;

// ---- the page side: a canvas over the game, and the sound
EM_JS(void, okavi_js_show, (int x, int y, int w, int h, int screen_w, int screen_h, int vw, int vh), {
	const box = (Module.okaviParent || document.getElementById('game-container') || (Module.canvas && Module.canvas.parentNode) || document.body);
	let c = document.getElementById('okavi-video');
	if (!c) {
		c = document.createElement('canvas');
		c.id = 'okavi-video';
		c.style.cssText = 'position:absolute;z-index:5;pointer-events:none;background:#000;image-rendering:auto';
		box.appendChild(c);
	}
	c.width = vw; c.height = vh;
	c.style.left = (100 * x / screen_w) + '%';
	c.style.top = (100 * y / screen_h) + '%';
	c.style.width = (100 * w / screen_w) + '%';
	c.style.height = (100 * h / screen_h) + '%';
	c.style.display = 'block';
});

EM_JS(void, okavi_js_frame, (const uint8_t *rgba, int w, int h), {
	const c = document.getElementById('okavi-video');
	if (!c) return;
	const img = new ImageData(new Uint8ClampedArray(HEAPU8.buffer, rgba, w * h * 4).slice(), w, h);
	c.getContext('2d').putImageData(img, 0, 0);
});

EM_JS(void, okavi_js_sound, (const float *pcm, int count, int channels, int rate, double offset_s), {
	try {
		if (Module.okaviSound) { Module.okaviSound.stop(); Module.okaviSound = null; }
		if (!count) return;
		const ctx = Module.okaviCtx || (Module.okaviCtx = new (window.AudioContext || window.webkitAudioContext)());
		if (ctx.state !== 'running') ctx.resume().catch(() => {});
		const all = new Float32Array(HEAPU8.buffer, pcm, count);
		const frames = Math.floor(count / channels);
		const buf = ctx.createBuffer(channels, frames, rate);
		for (let c = 0; c < channels; c++) {
			const ch = new Float32Array(frames);
			for (let i = 0; i < frames; i++) ch[i] = all[i * channels + c];
			buf.copyToChannel(ch, c);
		}
		const src = ctx.createBufferSource();
		src.buffer = buf;
		src.connect(ctx.destination);
		src.start(0, Math.max(0, offset_s));
		Module.okaviSound = src;
	} catch (e) { console.warn('[okavi] sound:', e); }
});

EM_JS(void, okavi_js_silence, (void), {
	if (Module.okaviSound) { try { Module.okaviSound.stop(); } catch (e) {} Module.okaviSound = null; }
});

EM_JS(void, okavi_js_hide, (void), {
	okavi_js_silence();
	const c = document.getElementById('okavi-video');
	if (c) c.remove();
});

EM_JS(double, okavi_js_now, (void), { return performance.now(); });

// ---- the AVI
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t fcc(const char *s) { return (uint8_t)s[0] | ((uint8_t)s[1] << 8) | ((uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24); }

// the chunks of a LIST 'movi' (or 'rec ' inside it): stream 'vid' video, 'aud' sound
static void scan_movi(Movie *m, long pos, long end, int vid, int aud)
{
	uint8_t hd[12];
	while (pos + 8 <= end) {
		fseek(m->f, pos, SEEK_SET);
		if (fread(hd, 1, 12, m->f) < 8) return;
		uint32_t id = rd32(hd), size = rd32(hd + 4);
		if (id == fcc("LIST")) {
			scan_movi(m, pos + 12, pos + 8 + (long)size, vid, aud);
		} else if (id != fcc("JUNK")) {
			int stream = (hd[0] - '0') * 10 + (hd[1] - '0');
			bool data = (hd[2] == 'd' && (hd[3] == 'c' || hd[3] == 'b'));
			if (stream == vid && data) {
				m->frames.push_back(std::make_pair(pos + 8, size));
			} else if (stream == aud && hd[2] == 'w' && hd[3] == 'b' && size) {
				std::vector<uint8_t> s(size);
				fseek(m->f, pos + 8, SEEK_SET);
				size_t n = fread(s.data(), 1, size, m->f);
				if (m->sound_bits == 16)   // 16-bit signed PCM
					for (size_t i = 0; i + 1 < n; i += 2) m->sound.push_back((int16_t)(s[i] | (s[i + 1] << 8)) / 32768.0f);
				else                       // 8-bit unsigned PCM
					for (size_t i = 0; i < n; i++) m->sound.push_back((s[i] - 128) / 128.0f);
			}
		}
		pos += 8 + (long)((size + 1) & ~1u);
	}
}

static bool open_avi(Movie *m, const char *file)
{
	m->f = fopen(file, "rb");
	if (!m->f) { printf("[okavi] %s not found\n", file); return false; }
	uint8_t hd[12];
	if (fread(hd, 1, 12, m->f) != 12 || rd32(hd) != fcc("RIFF") || rd32(hd + 8) != fcc("AVI ")) return false;
	long end = 8 + (long)rd32(hd + 4);
	int stream = 0, vid = -1, aud = -1;
	long pos = 12;
	uint32_t vscale = 1, vrate = 15;
	while (pos + 8 <= end) {
		fseek(m->f, pos, SEEK_SET);
		if (fread(hd, 1, 12, m->f) < 8) break;
		uint32_t id = rd32(hd), size = rd32(hd + 4);
		if (id == fcc("LIST") && rd32(hd + 8) == fcc("hdrl")) {
			// the streams: strh (type, rate) and strf (the format) per LIST 'strl'
			std::vector<uint8_t> h(size - 4);
			fseek(m->f, pos + 12, SEEK_SET);
			if (fread(h.data(), 1, h.size(), m->f) != h.size()) return false;
			for (size_t p = 0; p + 8 <= h.size();) {
				uint32_t cid = rd32(&h[p]), csz = rd32(&h[p + 4]);
				if (cid == fcc("LIST") && p + 12 <= h.size() && rd32(&h[p + 8]) == fcc("strl")) {
					for (size_t q = p + 12; q + 8 <= p + 8 + csz && q + 8 <= h.size();) {
						uint32_t sid = rd32(&h[q]), ssz = rd32(&h[q + 4]);
						const uint8_t *b = &h[q + 8];
						if (sid == fcc("strh") && ssz >= 32 && q + 8 + 32 <= h.size()) {
							if (rd32(b) == fcc("vids")) { vid = stream; vscale = rd32(b + 20); vrate = rd32(b + 24); }
							else if (rd32(b) == fcc("auds")) aud = stream;
						} else if (sid == fcc("strf") && q + 8 + 16 <= h.size()) {
							if (stream == vid && ssz >= 16) { m->w = (int)rd32(b + 4); m->h = abs((int)rd32(b + 8)); }
							else if (stream == aud && ssz >= 16 && (b[0] | (b[1] << 8)) == 1) {   // WAVE_FORMAT_PCM
								m->sound_channels = b[2] | (b[3] << 8);
								m->sound_rate = (int)rd32(b + 4);
								m->sound_bits = b[14] | (b[15] << 8);
								if (m->sound_channels < 1 || m->sound_channels > 2 || (m->sound_bits != 8 && m->sound_bits != 16))
									aud = -1;   // (not a format we play: the movie stays silent)
							}
						}
						q += 8 + ((ssz + 1) & ~1u);
					}
					stream++;
				}
				p += 8 + ((csz + 1) & ~1u);
			}
		} else if (id == fcc("LIST") && rd32(hd + 8) == fcc("movi")) {
			scan_movi(m, pos + 12, pos + 8 + (long)size, vid, aud);
		}
		pos += 8 + (long)((size + 1) & ~1u);
	}
	m->fps = vscale ? (double)vrate / vscale : 15.0;
	if (m->fps <= 0 || m->fps > 120) m->fps = 15.0;
	if (vid < 0 || m->w <= 0 || m->h <= 0 || m->frames.empty()) { printf("[okavi] no video in %s\n", file); return false; }
	printf("[okavi] %s %dx%d %.1f fps, %d frames, sound %d Hz %d-bit %s %.1f s\n", file, m->w, m->h, m->fps,
	       (int)m->frames.size(), m->sound_rate, m->sound_bits, m->sound_channels == 2 ? "stereo" : "mono",
	       m->sound_rate ? (double)m->sound.size() / m->sound_channels / m->sound_rate : 0.0);
	return true;
}

static void close_movie(Movie *m)
{
	okavi_js_hide();
	if (m->ctx) avcodec_free_context(&m->ctx);
	if (m->frame) av_frame_free(&m->frame);
	if (m->pkt) av_packet_free(&m->pkt);
	if (m->f) fclose(m->f);
	*m = Movie();
}

static double clock_ms(Movie *m)
{
	if (m->mode == Movie::PAUSED) return m->paused_ms - m->started_ms;
	return okavi_js_now() - m->started_ms;
}

// decode the next frame; false = the movie is over (or broken)
static bool decode_next(Movie *m, bool show)
{
	if (m->next_frame >= (int)m->frames.size()) return false;
	std::pair<long, uint32_t> fr = m->frames[m->next_frame++];
	if (!fr.second) return true;   // (a dropped frame: the last one stays)
	m->chunk.assign(fr.second + AV_INPUT_BUFFER_PADDING_SIZE, 0);
	fseek(m->f, fr.first, SEEK_SET);
	if (fread(m->chunk.data(), 1, fr.second, m->f) != fr.second) return false;
	m->pkt->data = m->chunk.data();
	m->pkt->size = (int)fr.second;
	if (avcodec_send_packet(m->ctx, m->pkt) < 0) return true;
	if (avcodec_receive_frame(m->ctx, m->frame) < 0 || !show) return true;
	// YUV (Indeo 5: 4:1:0, a chroma sample per 4x4 pixels) to RGBA
	AVFrame *f = m->frame;
	int cw = f->width > 0 ? f->width : m->w, ch = f->height > 0 ? f->height : m->h;
	int sx = 1 << (cw > 0 && f->linesize[1] * 4 <= f->linesize[0] + 32 ? 2 : 1), sy = sx;   // (4 for yuv410p)
	m->rgba.resize((size_t)cw * ch * 4);
	for (int y = 0; y < ch; y++) {
		const uint8_t *Y = f->data[0] + y * f->linesize[0];
		const uint8_t *U = f->data[1] + (y / sy) * f->linesize[1];
		const uint8_t *V = f->data[2] + (y / sy) * f->linesize[2];
		uint8_t *o = &m->rgba[(size_t)y * cw * 4];
		for (int x = 0; x < cw; x++) {
			int c = (Y[x] - 16) * 298, d = U[x / sx] - 128, e = V[x / sx] - 128;
			int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
			o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
			o[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
			o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
			o[3] = 255;
			o += 4;
		}
	}
	okavi_js_frame(m->rgba.data(), cw, ch);
	return true;
}

// call once per game frame: decodes and shows the frames that are due, ends the movie at its end
extern "C" void okavi_tick()
{
	Movie *m = &g_movie;
	if (!m->open || m->mode != Movie::PLAYING) return;
	int due = (int)(clock_ms(m) * m->fps / 1000.0) + 1;
	if (due > (int)m->frames.size()) due = (int)m->frames.size();
	while (m->next_frame < due)
		if (!decode_next(m, m->next_frame == due - 1)) break;
	double sound_ms = m->sound_rate ? 1000.0 * m->sound.size() / m->sound_channels / m->sound_rate : 0;
	double frames_ms = 1000.0 * m->frames.size() / m->fps;
	if (m->next_frame >= (int)m->frames.size() && clock_ms(m) >= (sound_ms > frames_ms ? sound_ms : frames_ms)) {
		m->mode = Movie::STOPPED;   // the game asks every second, then closes the window
		okavi_js_silence();
	}
}

static void play(Movie *m, double from_ms)
{
	m->mode = Movie::PLAYING;
	m->started_ms = okavi_js_now() - from_ms;
	if (!m->sound.empty() && m->sound_rate > 0)
		okavi_js_sound(m->sound.data(), (int)m->sound.size(), m->sound_channels, m->sound_rate, from_ms / 1000.0);
}

// ---- the API (okavi.h)
extern "C" int okavi_open(const char *file)
{
	close_movie(&g_movie);
	Movie *m = &g_movie;
	if (!file || !open_avi(m, file)) { close_movie(m); return 0; }
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_INDEO5);
	m->ctx = codec ? avcodec_alloc_context3(codec) : NULL;
	if (!m->ctx) { close_movie(m); return 0; }
	m->ctx->width = m->w;
	m->ctx->height = m->h;
	m->ctx->thread_count = 1;
	m->frame = av_frame_alloc();
	m->pkt = av_packet_alloc();
	if (!m->frame || !m->pkt || avcodec_open2(m->ctx, codec, NULL) < 0) { close_movie(m); return 0; }
	m->mode = Movie::STOPPED;
	m->open = true;
	return 1;
}

extern "C" void okavi_place(int x, int y, int w, int h, int screen_w, int screen_h)
{
	if (g_movie.open) okavi_js_show(x, y, w, h, screen_w, screen_h, g_movie.w, g_movie.h);
}

extern "C" void okavi_size(int *w, int *h) { *w = g_movie.w; *h = g_movie.h; }
extern "C" void okavi_play() { if (g_movie.open) play(&g_movie, 0); }
extern "C" void okavi_stop() { g_movie.mode = Movie::STOPPED; okavi_js_silence(); }
extern "C" void okavi_pause()
{
	Movie *m = &g_movie;
	if (m->mode == Movie::PLAYING) { m->mode = Movie::PAUSED; m->paused_ms = okavi_js_now(); okavi_js_silence(); }
}
extern "C" void okavi_resume()
{
	Movie *m = &g_movie;
	if (m->mode == Movie::PAUSED) play(m, m->paused_ms - m->started_ms);
}
extern "C" int okavi_mode() { return g_movie.mode == Movie::PLAYING ? OKAVI_PLAYING : g_movie.mode == Movie::PAUSED ? OKAVI_PAUSED : OKAVI_STOPPED; }
extern "C" void okavi_close() { close_movie(&g_movie); }
extern "C" int okavi_available() { return avcodec_find_decoder(AV_CODEC_ID_INDEO5) != NULL; }

#endif
