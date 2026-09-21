// visionOS audio proof 2: the FFmpeg decode chain (libavformat/libavcodec/libavutil from the vcpkg ffmpeg the engine build
// links) runs on xrsimulator and decodes exactly the way FFmpegFile.cpp + OpenALAudioCache.cpp::decodeFFmpeg do:
//   custom AVIO read callback (the game's File abstraction) -> avformat_open_input(NULL url) -> avformat_find_stream_info
//   -> avcodec_find_decoder / avcodec_alloc_context3 / avcodec_parameters_to_context / avcodec_open2
//   -> av_read_frame / avcodec_send_packet / avcodec_receive_frame (NO flush at EOF, same as FFmpegFile::decodePacket)
//   -> planar -> interleaved copy, counting file->m_totalSamples and the byte size like decodeFFmpeg.
// Test media is generated in memory through libavformat's own muxers/encoders (no game data, no copyrighted content):
//   WAV PCM s16 stereo, WAV PCM u8 mono, WAV IMA ADPCM (the format the retail SFX use), OGG Vorbis, FLAC and a synthetic
//   MP3 (valid frame headers around zeroed frames; the vcpkg build has an MP3 demuxer/decoder but no encoder).
//
// scripts/qa/vision-audio-ffmpeg-test.sh builds and runs it in a simulator device.
#import <Foundation/Foundation.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                                        \
	do {                                                                                        \
		if (cond) { ++g_pass; printf("  PASS  "); }                                             \
		else { ++g_fail; printf("  FAIL  "); }                                                  \
		printf(__VA_ARGS__);                                                                    \
		printf("\n");                                                                           \
	} while (0)

static std::string avErr(int e) { char b[256]; av_strerror(e, b, sizeof b); return b; }

// ---- generate a file in memory (write side) ---------------------------------------------------------------
struct MemSink { std::vector<uint8_t> data; int64_t pos = 0; };
static int sinkWrite(void *opaque, const uint8_t *buf, int size) {
	auto *m = (MemSink *)opaque;
	if (m->pos + size > (int64_t)m->data.size()) m->data.resize(m->pos + size);
	memcpy(m->data.data() + m->pos, buf, size);
	m->pos += size;
	return size;
}
static int64_t sinkSeek(void *opaque, int64_t off, int whence) {
	auto *m = (MemSink *)opaque;
	if (whence == AVSEEK_SIZE) return (int64_t)m->data.size();
	whence &= ~AVSEEK_FORCE;
	if (whence == SEEK_SET) m->pos = off; else if (whence == SEEK_CUR) m->pos += off; else if (whence == SEEK_END) m->pos = (int64_t)m->data.size() + off;
	return m->pos;
}

// Encodes `seconds` of a two-tone signal. Returns the number of input samples per channel.
static bool encodeToMemory(const char *muxer, AVCodecID codecId, int rate, int channels, AVSampleFormat wantFmt, float seconds, MemSink *sink, long *samplesIn, std::vector<float> *refMono) {
	const AVCodec *enc = avcodec_find_encoder(codecId);
	if (!enc) { printf("  (no encoder for %s in this FFmpeg build)\n", avcodec_get_name(codecId)); return false; }
	AVFormatContext *oc = nullptr;
	int r = avformat_alloc_output_context2(&oc, nullptr, muxer, nullptr);
	if (r < 0 || !oc) { printf("  (no muxer %s: %s)\n", muxer, avErr(r).c_str()); return false; }
	uint8_t *iobuf = (uint8_t *)av_malloc(1 << 16);
	oc->pb = avio_alloc_context(iobuf, 1 << 16, 1, sink, nullptr, sinkWrite, sinkSeek);
	oc->flags |= AVFMT_FLAG_CUSTOM_IO;
	AVStream *st = avformat_new_stream(oc, nullptr);
	AVCodecContext *cc = avcodec_alloc_context3(enc);
	cc->sample_rate = rate;
	av_channel_layout_default(&cc->ch_layout, channels);
	AVSampleFormat fmt = wantFmt;
	if (enc->sample_fmts) {
		bool ok = false;
		for (const AVSampleFormat *f = enc->sample_fmts; *f != AV_SAMPLE_FMT_NONE; ++f) if (*f == fmt) ok = true;
		if (!ok) fmt = enc->sample_fmts[0];
	}
	cc->sample_fmt = fmt;
	cc->time_base = AVRational{1, rate};
	if (codecId == AV_CODEC_ID_VORBIS) cc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
	if (oc->oformat->flags & AVFMT_GLOBALHEADER) cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	r = avcodec_open2(cc, enc, nullptr);
	if (r < 0) { printf("  encoder open failed: %s\n", avErr(r).c_str()); return false; }
	avcodec_parameters_from_context(st->codecpar, cc);
	st->time_base = cc->time_base;
	r = avformat_write_header(oc, nullptr);
	if (r < 0) { printf("  write_header failed: %s\n", avErr(r).c_str()); return false; }

	const long total = (long)(seconds * rate);
	const int frameSize = cc->frame_size > 0 ? cc->frame_size : 1024;
	AVFrame *fr = av_frame_alloc();
	fr->format = cc->sample_fmt;
	av_channel_layout_copy(&fr->ch_layout, &cc->ch_layout);
	fr->sample_rate = rate;
	fr->nb_samples = frameSize;
	av_frame_get_buffer(fr, 0);
	AVPacket *pkt = av_packet_alloc();
	long done = 0;
	auto drain = [&]() {
		for (;;) {
			int rr = avcodec_receive_packet(cc, pkt);
			if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
			if (rr < 0) break;
			av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
			pkt->stream_index = st->index;
			av_interleaved_write_frame(oc, pkt);
			av_packet_unref(pkt);
		}
	};
	while (done < total) {
		int n = (int)std::min<long>(frameSize, total - done);
		av_frame_make_writable(fr);
		fr->nb_samples = n;
		for (int i = 0; i < n; ++i) {
			const double t = (double)(done + i) / rate;
			const float v = 0.35f * (float)sin(2 * M_PI * 440.0 * t) + 0.2f * (float)sin(2 * M_PI * 1250.0 * t);
			for (int c = 0; c < channels; ++c) {
				const float s = c == 0 ? v : v * 0.5f;
				switch (fmt) {
				case AV_SAMPLE_FMT_S16: ((int16_t *)fr->data[0])[i * channels + c] = (int16_t)lrintf(s * 32767); break;
				case AV_SAMPLE_FMT_U8: ((uint8_t *)fr->data[0])[i * channels + c] = (uint8_t)(128 + lrintf(s * 127)); break;
				case AV_SAMPLE_FMT_S16P: ((int16_t *)fr->data[c])[i] = (int16_t)lrintf(s * 32767); break;
				case AV_SAMPLE_FMT_FLTP: ((float *)fr->data[c])[i] = s; break;
				case AV_SAMPLE_FMT_FLT: ((float *)fr->data[0])[i * channels + c] = s; break;
				default: break;
				}
			}
			if (refMono) refMono->push_back(v);
		}
		fr->pts = done;
		avcodec_send_frame(cc, fr);
		drain();
		done += n;
	}
	avcodec_send_frame(cc, nullptr);
	drain();
	av_write_trailer(oc);
	*samplesIn = total;
	av_frame_free(&fr);
	av_packet_free(&pkt);
	avcodec_free_context(&cc);
	av_freep(&oc->pb->buffer);
	avio_context_free(&oc->pb);
	avformat_free_context(oc);
	return true;
}

// ---- decode side: a line-for-line mirror of FFmpegFile::open + OpenALAudioFileCache::decodeFFmpeg -----------------
struct MemFile { const uint8_t *p; size_t n, pos; };
static int readPacket(void *opaque, uint8_t *buf, int buf_size) {   // FFmpegFile::readPacket with File::read swapped for memory
	auto *f = (MemFile *)opaque;
	size_t left = f->n - f->pos;
	int read = (int)std::min<size_t>(left, (size_t)buf_size);
	if (read <= 0) return AVERROR_EOF;
	memcpy(buf, f->p + f->pos, read);
	f->pos += read;
	return read;
}

struct Decoded {
	bool ok = false;
	std::string codec, format;
	int channels = 0, rate = 0, bytesPerSample = 0;
	long totalSamples = 0;    // file->m_totalSamples
	long byteSize = 0;        // file->m_fileSize
	std::vector<uint8_t> pcm; // audioData
	double durationMs = 0;    // file->m_duration
	bool planar = false;
	AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
};

static Decoded decodeLikeEngine(const std::vector<uint8_t> &bytes, bool flushAtEof) {
	Decoded d;
	MemFile mf{bytes.data(), bytes.size(), 0};
	AVFormatContext *fmtCtx = avformat_alloc_context();
	constexpr size_t avio_ctx_buffer_size = 0x10000;
	uint8_t *buffer = static_cast<uint8_t *>(av_malloc(avio_ctx_buffer_size));
	AVIOContext *avioCtx = avio_alloc_context(buffer, avio_ctx_buffer_size, 0, &mf, &readPacket, nullptr, nullptr);
	fmtCtx->pb = avioCtx;
	fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
	int result = avformat_open_input(&fmtCtx, nullptr, nullptr, nullptr);
	if (result < 0) { printf("  avformat_open_input: %s\n", avErr(result).c_str()); av_freep(&avioCtx->buffer); avio_context_free(&avioCtx); return d; }
	result = avformat_find_stream_info(fmtCtx, NULL);
	if (result < 0) { printf("  find_stream_info: %s\n", avErr(result).c_str()); return d; }
	d.format = fmtCtx->iformat->name;
	AVStream *av_stream = fmtCtx->streams[0];
	const AVCodec *input_codec = avcodec_find_decoder(av_stream->codecpar->codec_id);
	if (!input_codec) { printf("  Codec not supported: '%s'\n", avcodec_get_name(av_stream->codecpar->codec_id)); return d; }
	d.codec = input_codec->name;
	AVCodecContext *codec_ctx = avcodec_alloc_context3(input_codec);
	avcodec_parameters_to_context(codec_ctx, av_stream->codecpar);
	result = avcodec_open2(codec_ctx, input_codec, NULL);
	if (result < 0) { printf("  avcodec_open2: %s\n", avErr(result).c_str()); return d; }
	AVFrame *frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	d.channels = codec_ctx->ch_layout.nb_channels;
	d.rate = codec_ctx->sample_rate;
	d.bytesPerSample = av_get_bytes_per_sample(codec_ctx->sample_fmt);
	d.fmt = codec_ctx->sample_fmt;
	d.planar = av_sample_fmt_is_planar(codec_ctx->sample_fmt);

	auto on_frame = [&](AVFrame *fr) {   // the lambda of decodeFFmpeg
		const int frame_data_size = av_samples_get_buffer_size(NULL, codec_ctx->ch_layout.nb_channels, fr->nb_samples, codec_ctx->sample_fmt, 1);
		d.pcm.reserve(d.pcm.size() + frame_data_size);
		if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(fr->format))) {
			for (int sample = 0; sample < fr->nb_samples; ++sample)
				for (int channel = 0; channel < d.channels; ++channel) {
					const uint8_t *src = fr->data[channel] + sample * d.bytesPerSample;
					d.pcm.insert(d.pcm.end(), src, src + d.bytesPerSample);
				}
		} else {
			d.pcm.insert(d.pcm.end(), fr->data[0], fr->data[0] + frame_data_size);
		}
		d.byteSize += frame_data_size;
		d.totalSamples += fr->nb_samples;
	};

	// FFmpegFile::decodePacket, repeated until it returns false (EOF)
	for (;;) {
		result = av_read_frame(fmtCtx, packet);
		if (result == AVERROR_EOF) break;
		if (result < 0) { printf("  av_read_frame: %s\n", avErr(result).c_str()); break; }
		int drainGuard = 64;
		result = avcodec_send_packet(codec_ctx, packet);
		while (result == AVERROR(EAGAIN) && drainGuard-- > 0) {
			int recv = avcodec_receive_frame(codec_ctx, frame);
			if (recv == AVERROR(EAGAIN)) break;
			if (recv < 0) break;
			on_frame(frame);
			result = avcodec_send_packet(codec_ctx, packet);
		}
		av_packet_unref(packet);
		if (result < 0 && result != AVERROR(EAGAIN)) { printf("  avcodec_send_packet: %s\n", avErr(result).c_str()); break; }
		while (result >= 0) {
			result = avcodec_receive_frame(codec_ctx, frame);
			if (result == AVERROR(EAGAIN)) break;
			if (result < 0) break;
			on_frame(frame);
		}
	}
	if (flushAtEof) {   // NOT done by the engine; measured to show what it leaves on the table
		avcodec_send_packet(codec_ctx, nullptr);
		while (avcodec_receive_frame(codec_ctx, frame) >= 0) on_frame(frame);
	}
	d.durationMs = (d.totalSamples / (double)d.rate) * 1000.0;   // decodeFFmpeg's m_duration
	d.ok = d.totalSamples > 0;
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&codec_ctx);
	avformat_close_input(&fmtCtx);
	av_freep(&avioCtx->buffer);
	avio_context_free(&avioCtx);
	return d;
}

// Interleaved decoded stream -> channel-0 float samples
static std::vector<float> channel0(const Decoded &d) {
	std::vector<float> out;
	const size_t stride = (size_t)d.channels * d.bytesPerSample;
	for (size_t off = 0; off + stride <= d.pcm.size(); off += stride) {
		const uint8_t *p = d.pcm.data() + off;
		switch (d.fmt) {
		case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: out.push_back(*(const int16_t *)p / 32768.0f); break;
		case AV_SAMPLE_FMT_U8: case AV_SAMPLE_FMT_U8P: out.push_back((*p - 128) / 128.0f); break;
		case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: out.push_back(*(const float *)p); break;
		default: out.push_back(0); break;
		}
	}
	return out;
}
// Best normalised correlation of decoded ch0 against the reference over lags 0..maxLag (codecs add delay).
static double bestCorrelation(const std::vector<float> &a, const std::vector<float> &ref, int maxLag, int *lagOut) {
	double best = -2; int bestLag = 0;
	const size_t n = std::min(a.size(), ref.size());
	if (n < 4000) return 0;
	for (int lag = 0; lag <= maxLag; lag += 1) {
		double sab = 0, saa = 0, sbb = 0;
		const size_t start = 2000;
		for (size_t i = start; i + lag < a.size() && i < ref.size() - 1 && i < start + 6000; ++i) {
			const double x = a[i + lag], y = ref[i];
			sab += x * y; saa += x * x; sbb += y * y;
		}
		const double c = sab / sqrt(saa * sbb + 1e-30);
		if (c > best) { best = c; bestLag = lag; }
	}
	*lagOut = bestLag;
	return best;
}

struct Case { const char *name, *muxer; AVCodecID codec; int rate, channels; AVSampleFormat fmt; float seconds; bool exactCount; long tolerance; double minCorr; };

int main() {
	@autoreleasepool {
		setvbuf(stdout, nullptr, _IOLBF, 0);
		printf("vision-audio-ffmpeg-test\n");
		printf("libavformat %s libavcodec %s libavutil %s\n", AV_STRINGIFY(LIBAVFORMAT_VERSION), AV_STRINGIFY(LIBAVCODEC_VERSION), AV_STRINGIFY(LIBAVUTIL_VERSION));
		printf("avcodec_configuration: %s\n", avcodec_configuration());
		CHECK(avcodec_find_decoder(AV_CODEC_ID_MP3) != nullptr, "MP3 decoder linked (retail music is MP3)");
		CHECK(avcodec_find_decoder(AV_CODEC_ID_ADPCM_IMA_WAV) != nullptr, "IMA ADPCM WAV decoder linked (retail SFX/voices)");
		CHECK(avcodec_find_decoder(AV_CODEC_ID_VORBIS) != nullptr, "Vorbis decoder linked");
		CHECK(av_find_input_format("mp3") != nullptr && av_find_input_format("wav") != nullptr && av_find_input_format("ogg") != nullptr, "mp3, wav and ogg demuxers linked");

		Case cases[] = {
		    {"WAV PCM s16 stereo 22050", "wav", AV_CODEC_ID_PCM_S16LE, 22050, 2, AV_SAMPLE_FMT_S16, 1.5f, true, 0, 0.99},
		    {"WAV PCM u8 mono 11025", "wav", AV_CODEC_ID_PCM_U8, 11025, 1, AV_SAMPLE_FMT_U8, 1.0f, true, 0, 0.98},
		    {"WAV IMA ADPCM mono 22050", "wav", AV_CODEC_ID_ADPCM_IMA_WAV, 22050, 1, AV_SAMPLE_FMT_S16P, 1.5f, false, 2048, 0.97},
		    {"OGG Vorbis stereo 44100", "ogg", AV_CODEC_ID_VORBIS, 44100, 2, AV_SAMPLE_FMT_FLTP, 1.5f, false, 4096, 0.95},
		    {"FLAC mono 48000", "flac", AV_CODEC_ID_FLAC, 48000, 1, AV_SAMPLE_FMT_S16, 1.0f, true, 0, 0.99},
		};
		for (auto &c : cases) {
			printf("== %s ==\n", c.name);
			MemSink sink; long samplesIn = 0; std::vector<float> ref;
			if (!encodeToMemory(c.muxer, c.codec, c.rate, c.channels, c.fmt, c.seconds, &sink, &samplesIn, &ref)) {
				CHECK(false, "generate %s", c.name);
				continue;
			}
			printf("  generated %zu bytes, %ld samples/channel in\n", sink.data.size(), samplesIn);
			Decoded d = decodeLikeEngine(sink.data, false);
			Decoded df = decodeLikeEngine(sink.data, true);
			CHECK(d.ok, "decoded through the engine call sequence (format %s, codec %s)", d.format.c_str(), d.codec.c_str());
			if (!d.ok) continue;
			printf("  decoded: %d ch, %d Hz, %d bytes/sample (%s%s), %ld samples, %ld bytes, m_duration %.1f ms\n", d.channels, d.rate, d.bytesPerSample,
			       av_get_sample_fmt_name(d.fmt), d.planar ? ", planar->interleaved" : "", d.totalSamples, d.byteSize, d.durationMs);
			printf("  with a flush at EOF (the engine does not do this): %ld samples (%+ld)\n", df.totalSamples, df.totalSamples - d.totalSamples);
			CHECK(d.channels == c.channels && d.rate == c.rate, "channels/rate preserved (%d ch, %d Hz)", d.channels, d.rate);
			CHECK(d.pcm.size() == (size_t)d.byteSize, "byte size matches accumulated PCM (%zu)", d.pcm.size());
			const long diff = d.totalSamples - samplesIn;
			if (c.exactCount) CHECK(diff == 0, "sample count exact: %ld == %ld", d.totalSamples, samplesIn);
			else CHECK(labs(diff) <= c.tolerance, "sample count within codec framing tolerance: %ld vs %ld (%+ld, limit %ld)", d.totalSamples, samplesIn, diff, c.tolerance);
			int lag = 0;
			const double corr = bestCorrelation(channel0(d), ref, c.codec == AV_CODEC_ID_VORBIS ? 4096 : 64, &lag);
			CHECK(corr >= c.minCorr, "waveform matches the generated tone: correlation %.4f at lag %d samples (need >= %.2f)", corr, lag, c.minCorr);
		}

		printf("== synthetic MP3 (valid MPEG-1 Layer III frames, 128 kbps, 44.1 kHz, silent payload) ==\n");
		{
			std::vector<uint8_t> mp3;
			const int frames = 40;
			for (int i = 0; i < frames; ++i) {
				const uint8_t hdr[4] = {0xFF, 0xFB, 0x90, 0x64};   // MPEG1 L3, no CRC, 128 kbps, 44100 Hz, no padding, joint stereo
				mp3.insert(mp3.end(), hdr, hdr + 4);
				mp3.insert(mp3.end(), 417 - 4, 0);              // 144*128000/44100 = 417 bytes per frame
			}
			Decoded d = decodeLikeEngine(mp3, false);
			CHECK(d.ok, "MP3 decoded through the engine call sequence (format %s, codec %s)", d.format.c_str(), d.codec.c_str());
			if (d.ok) {
				printf("  decoded: %d ch, %d Hz, %d bytes/sample (%s), %ld samples for %d frames (1152 each = %d)\n", d.channels, d.rate, d.bytesPerSample,
				       av_get_sample_fmt_name(d.fmt), d.totalSamples, frames, frames * 1152);
				CHECK(d.rate == 44100 && d.channels == 2, "MP3 rate/channels detected (44100 Hz stereo)");
				CHECK(d.totalSamples >= (frames - 2) * 1152 && d.totalSamples <= frames * 1152, "MP3 sample count within one frame of the frame count (%ld)", d.totalSamples);
			}
		}
		printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
		return g_fail ? 1 : 0;
	}
}
