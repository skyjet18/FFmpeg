/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "../dashdec.c"

static int64_t deadline;

static int interrupted(void *opaque)
{
    return deadline && av_gettime_relative() >= deadline;
}

int main(int argc, char **argv)
{
    AVFormatContext *s = avformat_alloc_context();
    int ret;
    av_assert0(s && argc >= 3);
    s->interrupt_callback.callback = interrupted;
    if (!strcmp(argv[1], "fragment")) {
        char extensions[] = "ALL";
        DASHContext c = {.allowed_extensions = extensions,
            .max_url_size = MAX_URL_SIZE, .interrupt_callback = &s->interrupt_callback};
        struct fragment fragment = {.url = argv[2], .size = -1};
        struct representation rep = {.parent = s, .cur_seg = &fragment};
        int64_t start = av_gettime_relative();
        av_assert0(argc == 5);
        c.is_live = atoi(argv[3]);
        if (atoi(argv[4]))
            deadline = start + atoi(argv[4]) * 1000;
        s->priv_data = &c;
        s->url = av_strdup(argv[2]);
        av_assert0(s->url);
        ret = open_media_fragment(&c, &rep);
        printf("result=%d elapsed=%"PRId64"\n", ret, av_gettime_relative() - start);
        ff_format_io_close(s, &rep.input);
        forget_redirect(&c);
        s->priv_data = NULL;
        avformat_free_context(s);
        return 0;
    } else {
        AVDictionary *options = NULL;
        AVPacket *packet = av_packet_alloc();
        int count = 0;
        av_assert0(packet);
        deadline = av_gettime_relative() + 10000000;
        av_dict_set(&options, "dash_subtitle_default_discard", "1", 0);
        ret = avformat_open_input(&s, argv[2], NULL, &options);
        av_dict_free(&options);
        if (ret >= 0) {
            while (count < 400 && (ret = av_read_frame(s, packet)) >= 0) {
                AVStream *stream = s->streams[packet->stream_index];
                printf("packet=%d type=%d pts=%"PRId64" dts=%"PRId64"\n",
                       packet->stream_index, stream->codecpar->codec_type,
                       packet->pts, packet->dts);
                count++;
                av_packet_unref(packet);
            }
        }
        printf("result=%d packets=%d\n", ret, count);
        av_packet_free(&packet);
        avformat_close_input(&s);
        return count ? 0 : 1;
    }
}
