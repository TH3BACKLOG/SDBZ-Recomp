// FmvDecoder.cpp -- FFmpeg half of the detached host-side FMV player.
//
// WHY libavformat AND NOT THE EXISTING PES SPLITTER
// -------------------------------------------------
// The .SFD files are CRI SofDec: an MPEG-1 *program stream* carrying mpeg1video
// on PES id 0xE0 and -- this is the trap -- **CRI ADX** on PES id 0xC0, not MPEG
// audio. ffprobe on the same FFmpeg n7.1 tree this runtime links resolves it as
// `adpcm_adx` for all three files:
//
//   ATARI.SFD  mpeg1video 256x448 30000/1001  adpcm_adx 48000x2   4.054 s
//   OKR.SFD    mpeg1video 256x448 30000/1001  adpcm_adx 48000x2   5.056 s
//   OP.SFD     mpeg1video 512x448 30000/1001  adpcm_adx 48000x2  40.358 s
//
// Kernel/Stubs/MPEG.cpp's processPssBuffer() classifies 0xC0 as generic audio
// (isAudioStreamId, ~:827) and forwards the raw PES payload to guest callbacks.
// Reusing that would produce silence or noise. libavformat's mpegps demuxer has
// SofDec support and gets this right, and avformat/swresample are already linked
// into ps2_runtime (CMakeLists.txt:362-396, :616) -- so this costs no new
// dependency.
//
// MpegFfmpegDecoder in MPEG.cpp is NOT reusable: it lives in an anonymous
// namespace inside ps2_stubs, is video-only, and is driven by av_parser_parse2 on
// an elementary stream. Its send/receive/sws idiom (~:285-441) is transcribed
// here; nothing is linked against it.
//
// NO RAYLIB IN THIS FILE. See FmvHost.h.

#include "FmvHost.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#if PS2X_HAS_FFMPEG

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace ps2x_fmv
{
    namespace
    {
        std::string ffErr(int err)
        {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
            if (av_strerror(err, buf.data(), buf.size()) < 0)
            {
                return "unknown FFmpeg error";
            }
            return std::string(buf.data());
        }

        void configureLogLevel()
        {
            static std::once_flag once;
            std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
        }
    }

    struct Source::Impl
    {
        AVFormatContext *fmt = nullptr;
        AVCodecContext *vdec = nullptr;
        AVCodecContext *adec = nullptr;
        SwsContext *sws = nullptr;
        SwrContext *swr = nullptr;
        AVPacket *pkt = nullptr;
        AVFrame *frame = nullptr;

        int vIdx = -1;
        int aIdx = -1;
        AVRational vTimeBase{1, 90000};
        AVRational aTimeBase{1, 90000};
        int64_t vStartTime = 0;

        StreamInfo info;
        bool eof = false;
        bool flushed = false;
        std::string error;

        // Reused across frames so a 917 KB RGBA buffer is not reallocated 30x/s.
        std::vector<uint8_t> scratchRgba;

        ~Impl() { teardown(); }

        void teardown()
        {
            if (sws) { sws_freeContext(sws); sws = nullptr; }
            if (swr) { swr_free(&swr); }
            if (vdec) { avcodec_free_context(&vdec); }
            if (adec) { avcodec_free_context(&adec); }
            if (frame) { av_frame_free(&frame); }
            if (pkt) { av_packet_free(&pkt); }
            if (fmt) { avformat_close_input(&fmt); }
        }

        bool openDecoder(int streamIdx, AVCodecContext *&out, std::string &err)
        {
            AVStream *st = fmt->streams[streamIdx];
            const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!codec)
            {
                err = "no decoder for codec id " + std::to_string(static_cast<int>(st->codecpar->codec_id));
                return false;
            }
            out = avcodec_alloc_context3(codec);
            if (!out)
            {
                err = "avcodec_alloc_context3 failed";
                return false;
            }
            int rc = avcodec_parameters_to_context(out, st->codecpar);
            if (rc < 0)
            {
                err = "avcodec_parameters_to_context: " + ffErr(rc);
                return false;
            }
            rc = avcodec_open2(out, codec, nullptr);
            if (rc < 0)
            {
                err = std::string("avcodec_open2(") + codec->name + "): " + ffErr(rc);
                return false;
            }
            return true;
        }

        // Pull every ready frame out of the video decoder and convert to RGBA.
        void drainVideo(std::deque<VideoFrame> &vq)
        {
            if (!vdec)
            {
                return;
            }
            for (;;)
            {
                const int rc = avcodec_receive_frame(vdec, frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                {
                    return;
                }
                if (rc < 0)
                {
                    error = "video receive: " + ffErr(rc);
                    return;
                }

                const int w = frame->width;
                const int h = frame->height;
                if (w <= 0 || h <= 0)
                {
                    av_frame_unref(frame);
                    continue;
                }

                sws = sws_getCachedContext(sws, w, h, static_cast<AVPixelFormat>(frame->format),
                                           w, h, AV_PIX_FMT_RGBA,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws)
                {
                    error = "sws_getCachedContext failed";
                    av_frame_unref(frame);
                    return;
                }

                const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
                scratchRgba.resize(bytes);
                uint8_t *dstData[4] = {scratchRgba.data(), nullptr, nullptr, nullptr};
                int dstLinesize[4] = {w * 4, 0, 0, 0};
                sws_scale(sws, frame->data, frame->linesize, 0, h, dstData, dstLinesize);

                int64_t ts = frame->best_effort_timestamp;
                if (ts == AV_NOPTS_VALUE)
                {
                    ts = frame->pts;
                }

                VideoFrame vf;
                vf.width = w;
                vf.height = h;
                vf.rgba.assign(scratchRgba.begin(), scratchRgba.end());
                vf.pts = (ts == AV_NOPTS_VALUE)
                             ? -1.0
                             : (static_cast<double>(ts - vStartTime) * av_q2d(vTimeBase));
                vq.push_back(std::move(vf));

                av_frame_unref(frame);
            }
        }

        void drainAudio(std::vector<int16_t> &pcm)
        {
            if (!adec || !swr)
            {
                return;
            }
            for (;;)
            {
                const int rc = avcodec_receive_frame(adec, frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                {
                    return;
                }
                if (rc < 0)
                {
                    // Audio is never fatal -- video keeps playing.
                    return;
                }

                const int outRate = info.audioRate;
                const int outCh = info.audioChannels;
                const int64_t delay = swr_get_delay(swr, frame->sample_rate ? frame->sample_rate : outRate);
                const int maxOut = static_cast<int>(av_rescale_rnd(
                    delay + frame->nb_samples,
                    outRate,
                    frame->sample_rate ? frame->sample_rate : outRate,
                    AV_ROUND_UP));

                if (maxOut > 0)
                {
                    const size_t base = pcm.size();
                    pcm.resize(base + static_cast<size_t>(maxOut) * static_cast<size_t>(outCh));
                    uint8_t *outPlanes[1] = {reinterpret_cast<uint8_t *>(pcm.data() + base)};
                    const int got = swr_convert(swr, outPlanes, maxOut,
                                                const_cast<const uint8_t **>(frame->data),
                                                frame->nb_samples);
                    if (got < 0)
                    {
                        pcm.resize(base);
                    }
                    else
                    {
                        pcm.resize(base + static_cast<size_t>(got) * static_cast<size_t>(outCh));
                    }
                }

                av_frame_unref(frame);
            }
        }
    };

    Source::Source() : m_impl(new Impl()) {}

    Source::~Source()
    {
        delete m_impl;
        m_impl = nullptr;
    }

    void Source::close()
    {
        if (m_impl)
        {
            m_impl->teardown();
        }
    }

    const StreamInfo &Source::info() const { return m_impl->info; }
    bool Source::eof() const { return m_impl->eof; }
    const std::string &Source::error() const { return m_impl->error; }

    bool Source::open(const std::string &path, std::string &err)
    {
        configureLogLevel();
        Impl &d = *m_impl;

        int rc = avformat_open_input(&d.fmt, path.c_str(), nullptr, nullptr);
        if (rc < 0)
        {
            // SofDec streams normally probe fine, but force the MPEG-PS demuxer
            // rather than giving up on a probe miss.
            const AVInputFormat *ps = av_find_input_format("mpeg");
            if (ps)
            {
                rc = avformat_open_input(&d.fmt, path.c_str(), ps, nullptr);
            }
        }
        if (rc < 0)
        {
            err = "avformat_open_input: " + ffErr(rc);
            return false;
        }

        rc = avformat_find_stream_info(d.fmt, nullptr);
        if (rc < 0)
        {
            err = "avformat_find_stream_info: " + ffErr(rc);
            return false;
        }

        d.vIdx = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (d.vIdx < 0)
        {
            err = "no video stream";
            return false;
        }
        d.aIdx = av_find_best_stream(d.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        if (!d.openDecoder(d.vIdx, d.vdec, err))
        {
            return false;
        }

        AVStream *vs = d.fmt->streams[d.vIdx];
        d.vTimeBase = vs->time_base;
        d.vStartTime = (vs->start_time == AV_NOPTS_VALUE) ? 0 : vs->start_time;

        d.info.width = d.vdec->width;
        d.info.height = d.vdec->height;
        // avg_frame_rate, NOT r_frame_rate: r_frame_rate reads 60000/1001 here,
        // which is the FIELD rate, not the frame rate.
        d.info.fps = (vs->avg_frame_rate.den != 0) ? av_q2d(vs->avg_frame_rate) : 0.0;
        d.info.durationSec = (d.fmt->duration != AV_NOPTS_VALUE)
                                 ? static_cast<double>(d.fmt->duration) / AV_TIME_BASE
                                 : 0.0;
        const AVCodec *vc = avcodec_find_decoder(vs->codecpar->codec_id);
        d.info.videoCodec = vc ? vc->name : "?";

        if (d.aIdx >= 0)
        {
            std::string aerr;
            if (d.openDecoder(d.aIdx, d.adec, aerr))
            {
                AVStream *as = d.fmt->streams[d.aIdx];
                d.aTimeBase = as->time_base;
                d.info.audioRate = d.adec->sample_rate > 0 ? d.adec->sample_rate : 48000;
                d.info.audioChannels = 2;
                const AVCodec *ac = avcodec_find_decoder(as->codecpar->codec_id);
                d.info.audioCodec = ac ? ac->name : "?";

                AVChannelLayout outLayout;
                av_channel_layout_default(&outLayout, d.info.audioChannels);
                const int src = swr_alloc_set_opts2(&d.swr,
                                                    &outLayout, AV_SAMPLE_FMT_S16, d.info.audioRate,
                                                    &d.adec->ch_layout, d.adec->sample_fmt,
                                                    d.adec->sample_rate,
                                                    0, nullptr);
                av_channel_layout_uninit(&outLayout);
                if (src < 0 || swr_init(d.swr) < 0)
                {
                    if (d.swr) { swr_free(&d.swr); }
                    avcodec_free_context(&d.adec);
                    d.aIdx = -1;
                }
                else
                {
                    d.info.hasAudio = true;
                }
            }
            else
            {
                // Audio failure never aborts playback -- video only.
                d.aIdx = -1;
                d.info.audioCodec = "unavailable";
            }
        }

        d.pkt = av_packet_alloc();
        d.frame = av_frame_alloc();
        if (!d.pkt || !d.frame)
        {
            err = "av_packet_alloc / av_frame_alloc failed";
            return false;
        }
        return true;
    }

    bool Source::pump(std::deque<VideoFrame> &vq, std::vector<int16_t> &pcm, size_t maxQueued)
    {
        Impl &d = *m_impl;
        if (!d.fmt || !d.error.empty())
        {
            return false;
        }

        d.drainVideo(vq);
        d.drainAudio(pcm);

        while (vq.size() < maxQueued && !d.eof && d.error.empty())
        {
            const int rc = av_read_frame(d.fmt, d.pkt);
            if (rc < 0)
            {
                if (!d.flushed)
                {
                    d.flushed = true;
                    if (d.vdec) { avcodec_send_packet(d.vdec, nullptr); }
                    if (d.adec) { avcodec_send_packet(d.adec, nullptr); }
                    d.drainVideo(vq);
                    d.drainAudio(pcm);
                }
                d.eof = true;
                break;
            }

            if (d.pkt->stream_index == d.vIdx)
            {
                const int sr = avcodec_send_packet(d.vdec, d.pkt);
                if (sr < 0 && sr != AVERROR(EAGAIN))
                {
                    d.error = "video send: " + ffErr(sr);
                }
                d.drainVideo(vq);
            }
            else if (d.aIdx >= 0 && d.pkt->stream_index == d.aIdx)
            {
                if (avcodec_send_packet(d.adec, d.pkt) >= 0)
                {
                    d.drainAudio(pcm);
                }
            }
            av_packet_unref(d.pkt);
        }

        return !d.eof && d.error.empty();
    }

    bool decoderAvailable() { return true; }
}

#else // !PS2X_HAS_FFMPEG

// The three extern "C" entry points in FmvHost.cpp are unconditional, so this
// file must still provide the facade or ps2_runtime will not link. Everything
// reports failure, and FmvHost.cpp degrades to skip semantics -- never a hang.

namespace ps2x_fmv
{
    struct Source::Impl
    {
        StreamInfo info;
        std::string error{"built without FFmpeg (PS2X_ENABLE_FFMPEG=OFF)"};
    };

    Source::Source() : m_impl(new Impl()) {}
    Source::~Source() { delete m_impl; m_impl = nullptr; }
    void Source::close() {}
    const StreamInfo &Source::info() const { return m_impl->info; }
    bool Source::eof() const { return true; }
    const std::string &Source::error() const { return m_impl->error; }

    bool Source::open(const std::string &, std::string &err)
    {
        err = m_impl->error;
        return false;
    }

    bool Source::pump(std::deque<VideoFrame> &, std::vector<int16_t> &, size_t)
    {
        return false;
    }

    bool decoderAvailable() { return false; }
}

#endif // PS2X_HAS_FFMPEG
