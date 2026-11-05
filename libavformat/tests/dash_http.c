/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "../dashdec.c"

static int64_t deadline;

static int interrupted(void *opaque)
{
    return deadline && av_gettime_relative() >= deadline;
}

static int switch_test(AVFormatContext **ps, const char *url)
{
    AVFormatContext *s;
    AVPacket *pkt = av_packet_alloc();
    AVDictionary *options = NULL;
    const int64_t targets[] = {0, 12375000, 4375000, 30375000, 32375000};
    int audio[2], video = -1, n_audio = 0, ret, stage, i;

    av_assert0(pkt);
    av_dict_set(&options, "dash_subtitle_default_discard", "1", 0);
    ret = avformat_open_input(ps, url, NULL, &options);
    av_dict_free(&options);
    if (ret < 0)
        goto end;
    s = *ps;
    for (i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && n_audio < 2)
            audio[n_audio++] = i;
        if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            video = i;
    }
    av_assert0(n_audio == 2 && video >= 0);
    s->streams[audio[1]]->discard = AVDISCARD_ALL;
    for (stage = 0; stage < FF_ARRAY_ELEMS(targets); stage++) {
        int selected = audio[stage & 1], seen[2] = {0};
        int64_t progress[2] = {INT64_MIN, INT64_MIN};
        if (stage) {
            /* The newly selected component is closed while this seek runs. */
            if (stage < 4) {
                ret = avformat_seek_file(s, -1, INT64_MIN, targets[stage], INT64_MAX, 0);
                if (ret < 0)
                    goto end;
            }
            s->streams[audio[!(stage & 1)]]->discard = AVDISCARD_ALL;
            s->streams[selected]->discard = AVDISCARD_DEFAULT;
        }
        for (i = 0; i < 3000; i++) {
            int kind;
            int64_t pts;
            ret = av_read_frame(s, pkt);
            if (ret < 0)
                goto end;
            av_assert0(pkt->stream_index == video || pkt->stream_index == selected);
            kind = pkt->stream_index == selected;
            pts = av_rescale_q(pkt->pts, s->streams[pkt->stream_index]->time_base,
                               AV_TIME_BASE_Q);
            if (!seen[kind]++)
                printf("stage=%d type=%d first_us=%"PRId64"\n", stage, kind, pts);
            progress[kind] = pts;
            av_packet_unref(pkt);
            if (progress[0] >= targets[stage] + 2000000 &&
                progress[1] >= targets[stage] + 2000000)
                break;
        }
        av_assert0(i < 3000);
    }
end:
    av_packet_free(&pkt);
    return ret;
}

int main(int argc, char **argv)
{
    AVFormatContext *s = avformat_alloc_context();
    int ret;
    av_assert0(s && argc >= 3);
    s->interrupt_callback.callback = interrupted;
    if (!strcmp(argv[1], "switch")) {
        deadline = av_gettime_relative() + 15000000;
        ret = switch_test(&s, argv[2]);
        printf("result=%d\n", ret);
        avformat_close_input(&s);
        return ret < 0;
    } else if (!strcmp(argv[1], "fragment")) {
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
