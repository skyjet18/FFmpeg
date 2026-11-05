/*
 * MPEG-TS AAC timestamp dejitter tests
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavformat/avformat.h"
#include "libavformat/mux.h"

enum Mode { ADTS, RAW, RAW_SHORT, RAW_HE, LATM, OTHER };

typedef struct Expected {
    int stream;
    int64_t pts, dts;
} Expected;

typedef struct Test {
    AVFormatContext *ctx;
    Expected expected[4096];
    int count;
    enum Mode mode;
    int flush;
    const char *name;
} Test;

static int init(Test *t, const char *name, enum Mode mode, int rate,
                int threshold, int flush, int streams, int latm)
{
    int ret;

    *t = (Test) { .mode = mode, .flush = flush, .name = name };
    ret = avformat_alloc_output_context2(&t->ctx, NULL, "mpegts", NULL);
    if (ret < 0)
        return ret;
    t->ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    t->ctx->max_delay = 1000000;
    if ((ret = av_opt_set_int(t->ctx->priv_data, "mpegts_copyts", 1, 0)) < 0 ||
        (threshold >= 0 &&
         (ret = av_opt_set_int(t->ctx->priv_data, "aac_pts_dejitter", threshold, 0)) < 0) ||
        (latm && (ret = av_opt_set(t->ctx->priv_data, "mpegts_flags", "latm", 0)) < 0))
        return ret;
    for (int i = 0; i < streams; i++) {
        AVStream *st = avformat_new_stream(t->ctx, NULL);
        AVCodecParameters *par;

        if (!st)
            return AVERROR(ENOMEM);
        par = st->codecpar;
        par->codec_type = AVMEDIA_TYPE_AUDIO;
        par->codec_id = mode == LATM ? AV_CODEC_ID_AAC_LATM :
                        mode == OTHER ? AV_CODEC_ID_MP2 : AV_CODEC_ID_AAC;
        par->sample_rate = rate;
        par->frame_size = 1024;
        av_channel_layout_default(&par->ch_layout, 2);
        if (mode == RAW || mode == RAW_SHORT || mode == RAW_HE) {
            par->extradata_size = mode == RAW_HE ? 4 : 2;
            par->extradata = av_mallocz(par->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!par->extradata)
                return AVERROR(ENOMEM);
            par->extradata[0] = 0x11;
            par->extradata[1] = 0x90; /* AAC-LC, 48 kHz, stereo */
            if (mode == RAW_SHORT)
                par->extradata[1] |= 4;
            if (mode == RAW_HE) {
                /* HE-AAC, 24 kHz core, 48 kHz output, stereo */
                static const uint8_t asc[] = { 0x2b, 0x11, 0x88, 0x00 };
                memcpy(par->extradata, asc, sizeof(asc));
                par->frame_size = 2048;
            }
        }
    }
    if ((ret = avio_open_dyn_buf(&t->ctx->pb)) < 0)
        return ret;
    return avformat_write_header(t->ctx, NULL);
}

static int send_packet(Test *t, int stream, int64_t pts, int64_t dts,
                       int64_t expected_pts, int64_t expected_dts,
                       int blocks, int frames, int rate)
{
    AVPacket *pkt = av_packet_alloc();
    int ret;

    if (!pkt)
        return AVERROR(ENOMEM);
    ret = av_new_packet(pkt, 32 * frames);
    if (ret < 0)
        goto end;
    memset(pkt->data, 0, pkt->size);
    for (int i = 0; i < frames; i++) {
        uint8_t *p = pkt->data + 32 * i;

        if (t->mode == ADTS) {
            p[0] = 0xff;
            p[1] = 0xf1;
            p[2] = 0x40 | ((rate == 44100 ? 4 : 3) << 2);
            p[3] = 0x80;
            p[4] = 4;
            p[5] = 0x1f;
            p[6] = 0xfc | (blocks - 1);
        } else if (t->mode == LATM) {
            p[0] = 0x56;
            p[1] = 0xe0;
            p[2] = 29;
        }
    }
    pkt->stream_index = stream;
    pkt->pts = pts;
    pkt->dts = dts;
    pkt->duration = av_rescale(1024 * blocks * frames, 90000, rate);
    if (expected_pts != AV_NOPTS_VALUE) {
        if (t->count == FF_ARRAY_ELEMS(t->expected)) {
            ret = AVERROR(ENOSPC);
            goto end;
        }
        t->expected[t->count++] = (Expected) { stream, expected_pts, expected_dts };
    }
    ret = av_write_frame(t->ctx, pkt);
    if (ret >= 0 && (pkt->pts != pts || pkt->dts != dts))
        ret = AVERROR_BUG;
    if (ret >= 0 && t->flush)
        ret = av_write_frame(t->ctx, NULL);
end:
    av_packet_free(&pkt);
    return ret;
}

static int64_t read_pts(const uint8_t *p)
{
    return ((int64_t)(p[0] & 14) << 29) | (p[1] << 22) |
           ((p[2] & 254) << 14) | (p[3] << 7) | (p[4] >> 1);
}

static int finish(Test *t, int ret)
{
    uint8_t *buf = NULL;
    int size = 0, count = 0;

    if (ret >= 0)
        ret = av_write_trailer(t->ctx);
    if (t->ctx && t->ctx->pb)
        size = avio_close_dyn_buf(t->ctx->pb, &buf);
    if (t->ctx)
        t->ctx->pb = NULL;
    for (int off = 0; ret >= 0 && off + 188 <= size; off += 188) {
        const uint8_t *p = buf + off, *pes;
        int pid = AV_RB16(p + 1) & 8191;
        int start = 4 + ((p[3] & 32) ? p[4] + 1 : 0);
        int64_t pts, dts;
        const Expected *e;

        if (!(p[1] & 64) || !(p[3] & 16) || pid < 256 ||
            pid >= 256 + t->ctx->nb_streams || start + 14 > 188)
            continue;
        pes = p + start;
        if (AV_RB24(pes) != 1 || !(pes[7] & 128))
            continue;
        pts = read_pts(pes + 9);
        dts = (pes[7] & 64) ? read_pts(pes + 14) : pts;
        if (count >= t->count) {
            ret = AVERROR_INVALIDDATA;
            break;
        }
        e = &t->expected[count++];
        if (pid != 256 + e->stream ||
            pts != (e->pts & 0x1ffffffffLL) ||
            dts != (e->dts & 0x1ffffffffLL)) {
            fprintf(stderr, "%s: PES %d: got %"PRId64"/%"PRId64
                    ", expected %"PRId64"/%"PRId64"\n",
                    t->name, count, pts, dts, e->pts, e->dts);
            ret = AVERROR_INVALIDDATA;
        }
    }
    if (ret >= 0 && count != t->count)
        ret = AVERROR_INVALIDDATA;
    if (ret < 0)
        fprintf(stderr, "%s failed (PES %d/%d): %s\n",
                t->name, count, t->count, av_err2str(ret));
    av_free(buf);
    avformat_free_context(t->ctx);
    return ret < 0;
}

static int jitter(enum Mode mode, int rate, int threshold, int latm,
                  int64_t base)
{
    Test t;
    int ret = init(&t, "jitter", mode, rate, threshold, 1, 2, latm);

    for (int i = 0; ret >= 0 && i < 1200; i++) {
        for (int stream = 0; ret >= 0 && stream < 2; stream++) {
            int64_t exact = base + stream * 9000 + av_rescale(i * 1024LL, 90000, rate);
            int64_t pts = exact + (i ? (i % 3 - 1) * 450 : 0);
            int64_t expected = threshold == 0 || mode == OTHER ? pts : exact;

            ret = send_packet(&t, stream, pts, pts - 90,
                              expected, expected - 90, 1, 1, rate);
        }
    }
    return finish(&t, ret);
}

static int boundaries(void)
{
    Test t;
    int ret = init(&t, "threshold boundaries", ADTS, 48000, 10, 1, 1, 0);
    const int64_t input[] = { 90000, 92820, 92940, 96661, 99031 };
    const int64_t expected[] = { 90000, 91920, 93840, 96661, 98581 };

    for (int i = 0; ret >= 0 && i < FF_ARRAY_ELEMS(input); i++)
        ret = send_packet(&t, 0, input[i], input[i], expected[i], expected[i],
                          1, 1, 48000);
    return finish(&t, ret);
}

static int aggregation(void)
{
    Test t;
    int ret = init(&t, "PES resynchronization", ADTS, 48000, -1, 0, 1, 0);

    if (ret >= 0)
        ret = send_packet(&t, 0, 90000, 90000, 90000, 90000, 1, 1, 48000);
    if (ret >= 0)
        ret = send_packet(&t, 0, 92200, 92200, AV_NOPTS_VALUE, 0, 1, 1, 48000);
    if (ret >= 0)
        ret = send_packet(&t, 0, 110000, 110000, 110000, 110000, 1, 1, 48000);
    return finish(&t, ret);
}

static int sample_counts(void)
{
    Test t;
    int ret = init(&t, "ADTS sample counts and rate change", ADTS, 48000, -1, 1, 1, 0);

    if (ret >= 0)
        ret = send_packet(&t, 0, 90000, 90000, 90000, 90000, 2, 2, 48000);
    if (ret >= 0)
        ret = send_packet(&t, 0, 97900, 97900, 97680, 97680, 1, 1, 48000);
    if (ret >= 0)
        ret = send_packet(&t, 0, 100000, 100000, 100000, 100000, 1, 1, 44100);
    if (ret >= 0)
        ret = send_packet(&t, 0, 102500, 102500, 102090, 102090, 1, 1, 44100);
    return finish(&t, ret);
}

static int configured_samples(enum Mode mode, int samples)
{
    Test t;
    int ret = init(&t, "ASC sample clock", mode, 48000, -1, 1, 1, 1);
    int64_t step = av_rescale(samples, 90000, 48000);

    for (int i = 0; ret >= 0 && i < 10; i++) {
        int64_t exact = 90000 + i * step;
        int64_t pts = exact + (i ? 300 : 0);

        ret = send_packet(&t, 0, pts, pts, exact, exact, 1, 1, 48000);
    }
    return finish(&t, ret);
}

static int unavailable_samples(void)
{
    Test t;
    int ret = init(&t, "unavailable sample count resets clock", LATM, 48000, -1, 1, 1, 0);

    if (ret >= 0)
        ret = send_packet(&t, 0, 90000, 90000, 90000, 90000, 1, 1, 48000);
    if (ret >= 0) {
        t.ctx->streams[0]->codecpar->frame_size = 0;
        ret = send_packet(&t, 0, 92200, 92200, 92200, 92200, 0, 1, 48000);
    }
    if (ret >= 0) {
        t.ctx->streams[0]->codecpar->frame_size = 1024;
        ret = send_packet(&t, 0, 94400, 94400, 94400, 94400, 1, 1, 48000);
    }
    if (ret >= 0)
        ret = send_packet(&t, 0, 96500, 96500, 96320, 96320, 1, 1, 48000);
    return finish(&t, ret);
}

static int latm_samples(int frame_size)
{
    Test t;
    int ret = init(&t, "LATM sample accounting", LATM, 48000, -1, 1, 1, 0);

    if (ret >= 0) {
        t.ctx->streams[0]->codecpar->frame_size = frame_size;
        ret = send_packet(&t, 0, 90000, 90000, 90000, 90000, 1, 2, 48000);
    }
    if (ret >= 0)
        ret = send_packet(&t, 0, 94000, 94000, 93840, 93840, 1, 1, 48000);
    return finish(&t, ret);
}

static int clean(void)
{
    Test t;
    int ret = init(&t, "clean timestamps", ADTS, 48000, -1, 1, 1, 0);

    for (int i = 0; ret >= 0 && i < 10; i++) {
        int64_t pts = 90000 + i * 1920;
        ret = send_packet(&t, 0, pts, pts, pts, pts, 1, 1, 48000);
    }
    return finish(&t, ret);
}

static int reset_packet(int missing_pts)
{
    Test t;
    uint8_t data[32] = { 0x56, 0xe0, 29 };
    AVPacket pkt = { .data = data, .size = sizeof(data), .duration = 1920,
                     .pts = missing_pts ? AV_NOPTS_VALUE : 92200, .dts = 92200 };
    int ret = init(&t, "missing PTS or truncated frame resets clock",
                   LATM, 48000, -1, 1, 1, 0);

    if (ret >= 0)
        ret = send_packet(&t, 0, 90000, 90000, 90000, 90000, 1, 1, 48000);
    if (ret >= 0) {
        if (!missing_pts) {
            data[2] = 63;
            t.expected[t.count++] = (Expected) { 0, 92200, 92200 };
        }
        /* The generic muxing layer synthesizes missing PTS; exercise the
         * muxer's own reset path without that synthesis. */
        ret = ffofmt(t.ctx->oformat)->write_packet(t.ctx, &pkt);
        if (ret >= 0)
            ret = av_write_frame(t.ctx, NULL);
    }
    if (ret >= 0)
        ret = send_packet(&t, 0, 94400, 94400, 94400, 94400, 1, 1, 48000);
    if (ret >= 0)
        ret = send_packet(&t, 0, 96500, 96500, 96320, 96320, 1, 1, 48000);
    return finish(&t, ret);
}

int main(void)
{
    int failed = 0;

    av_log_set_level(AV_LOG_ERROR);
    failed |= jitter(ADTS, 48000, -1, 0, 90000);
    failed |= jitter(ADTS, 44100, -1, 0, 90000);
    failed |= jitter(ADTS, 48000, 0, 0, 90000);
    failed |= jitter(RAW, 48000, -1, 0, 90000);
    failed |= jitter(RAW, 48000, -1, 1, 90000);
    failed |= jitter(LATM, 48000, -1, 0, 90000);
    failed |= jitter(OTHER, 48000, -1, 0, 90000);
    failed |= jitter(ADTS, 48000, -1, 0, (1LL << 33) - 90000);
    failed |= jitter(ADTS, 48000, -1, 0, -90000);
    failed |= boundaries();
    failed |= aggregation();
    failed |= sample_counts();
    failed |= configured_samples(RAW_SHORT, 960);
    failed |= configured_samples(RAW_HE, 2048);
    failed |= unavailable_samples();
    failed |= latm_samples(1024);
    failed |= latm_samples(0);
    failed |= clean();
    failed |= reset_packet(0);
    failed |= reset_packet(1);
    return failed;
}
