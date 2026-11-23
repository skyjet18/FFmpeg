/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavformat/avformat.h"
#include "libavutil/error.h"
#include "libavutil/mathematics.h"

int main(int argc, char **argv)
{
    AVFormatContext *s = NULL;
    AVPacket *pkt = av_packet_alloc();
    char command[128];
    int ret;

    if (argc != 2 || !pkt)
        return 2;
    av_log_set_level(AV_LOG_WARNING);
    ret = avformat_open_input(&s, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "open: %s\n", av_err2str(ret));
        return 1;
    }
    printf("opened %u\n", s->nb_streams);
    fflush(stdout);
    while (fgets(command, sizeof(command), stdin)) {
        if (!strncmp(command, "probe", 5)) {
            ret = avformat_find_stream_info(s, NULL);
        } else if (!strncmp(command, "select ", 7)) {
            int selected = atoi(command + 7);
            int ordinal = 0;
            for (unsigned i = 0; i < s->nb_streams; i++)
                if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                    s->streams[i]->discard = ordinal++ == selected ?
                                             AVDISCARD_DEFAULT : AVDISCARD_ALL;
            ret = 0;
        } else if (!strncmp(command, "read ", 5)) {
            int count = atoi(command + 5);
            ret = 0;
            while (count-- > 0 && (ret = av_read_frame(s, pkt)) >= 0) {
                AVStream *st = s->streams[pkt->stream_index];
                int64_t ts = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);
                printf("packet %d %"PRId64" %.*s\n", st->codecpar->codec_type,
                       ts, st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE ?
                       pkt->size : 0, pkt->data);
                av_packet_unref(pkt);
            }
            if (ret == AVERROR_EOF)
                ret = 0;
        } else if (!strncmp(command, "seek ", 5) ||
                   !strncmp(command, "seekany ", 8)) {
            int any = !strncmp(command, "seekany ", 8);
            int64_t seconds = strtoll(command + (any ? 8 : 5), NULL, 10);
            ret = av_seek_frame(s, -1, seconds * AV_TIME_BASE,
                                any ? AVSEEK_FLAG_ANY : AVSEEK_FLAG_BACKWARD);
        } else if (!strncmp(command, "streams", 7)) {
            for (unsigned i = 0; i < s->nb_streams; i++) {
                AVStream *st = s->streams[i];
                AVDictionaryEntry *language = av_dict_get(st->metadata, "language", NULL, 0);
                printf("stream %u %d %d %s\n", i, st->codecpar->codec_type,
                       st->codecpar->codec_id, language ? language->value : "-");
            }
            ret = 0;
        } else {
            ret = AVERROR(EINVAL);
        }
        printf("done %d\n", ret);
        fflush(stdout);
    }
    avformat_close_input(&s);
    av_packet_free(&pkt);
    return 0;
}
