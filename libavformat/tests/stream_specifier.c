/*
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

#include <stdio.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavformat/avformat.h"

typedef struct TestCase {
    const char *specifier;
    int expected[4];
} TestCase;

static int add_stream(AVFormatContext *ctx, enum AVMediaType type,
                      enum AVCodecID codec_id)
{
    AVStream *st = avformat_new_stream(ctx, NULL);

    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = type;
    st->codecpar->codec_id   = codec_id;
    return 0;
}

int main(void)
{
    static const TestCase tests[] = {
        { "",               { 1, 1, 1, 1 } },
        { "a",              { 1, 1, 0, 1 } },
        { "a:0",            { 1, 0, 0, 0 } },
        { "a:aac",          { 1, 0, 0, 1 } },
        { "a:aac:0",        { 1, 0, 0, 0 } },
        { "a:aac:1",        { 0, 0, 0, 1 } },
        { "v:h264",         { 0, 0, 1, 0 } },
        { "a:h264",         { 0, 0, 0, 0 } },
        { "p:7:a:0",        { 0, 1, 0, 0 } },
        { "p:7:a:aac:0",    { 0, 0, 0, 1 } },
        { "a:not_a_codec",  { AVERROR(EINVAL), AVERROR(EINVAL),
                               AVERROR(EINVAL), AVERROR(EINVAL) } },
    };
    AVFormatContext *ctx = avformat_alloc_context();
    int ret = 1;

    if (!ctx)
        return 1;

    if (add_stream(ctx, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC) < 0 ||
        add_stream(ctx, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_MP3) < 0 ||
        add_stream(ctx, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264) < 0 ||
        add_stream(ctx, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC) < 0)
        goto end;

    if (!av_new_program(ctx, 7))
        goto end;
    av_program_add_stream_index(ctx, 7, 1);
    av_program_add_stream_index(ctx, 7, 3);

    av_log_set_level(AV_LOG_QUIET);
    for (unsigned i = 0; i < FF_ARRAY_ELEMS(tests); i++) {
        for (unsigned j = 0; j < ctx->nb_streams; j++) {
            int actual = avformat_match_stream_specifier(ctx, ctx->streams[j],
                                                         tests[i].specifier);
            if (actual != tests[i].expected[j]) {
                fprintf(stderr, "specifier '%s', stream %u: expected %d, got %d\n",
                        tests[i].specifier, j, tests[i].expected[j], actual);
                goto end;
            }
        }
    }

    ret = 0;

end:
    avformat_free_context(ctx);
    return ret;
}