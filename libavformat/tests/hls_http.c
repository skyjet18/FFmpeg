/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavformat/avformat.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"

static int64_t deadline;

static int interrupted(void *opaque)
{
    return av_gettime_relative() >= deadline;
}

int main(int argc, char **argv)
{
    AVFormatContext *s = avformat_alloc_context();
    AVPacket *pkt = av_packet_alloc();
    AVDictionary *options = NULL;
    int audio[3] = {-1, -1, -1}, n_audio = 0, video = -1, ret, initial, live;
    int64_t target = 0;

    av_assert0(argc == 4 && s && pkt);
    initial = atoi(argv[2]);
    live = atoi(argv[3]);
    av_assert0(initial >= 0 && initial < 3 && (live == 0 || live == 1));
    deadline = av_gettime_relative() + 15000000;
    s->interrupt_callback.callback = interrupted;
    av_dict_set(&options, "live_start_index", "0", 0);
    ret = avformat_open_input(&s, argv[1], NULL, &options);
    av_dict_free(&options);
    if (ret < 0)
        goto end;
    ret = avformat_find_stream_info(s, NULL);
    if (ret < 0)
        goto end;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            video = i;
        else if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AVDictionaryEntry *name = av_dict_get(s->streams[i]->metadata, "comment", NULL, 0);
            int index;

            av_assert0(n_audio < 3);
            av_assert0(name);
            index = !strcmp(name->value, "embedded") ? 0 : atoi(name->value + 5);
            av_assert0(index >= 0 && index < 3 && audio[index] == -1);
            audio[index] = i;
            n_audio++;
        }
    }
    av_assert0(video >= 0 && n_audio == 3 && s->start_time != AV_NOPTS_VALUE);
    target = s->start_time;
    for (int stage = 0; stage < 7; stage++) {
        int selected = audio[(initial + stage) % 3];
        int seen[2] = {0};
        int64_t progress[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};

        for (int i = 0; i < 3; i++)
            s->streams[audio[i]]->discard =
                audio[i] == selected ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        if (stage && !live) {
            ret = avformat_seek_file(s, -1, INT64_MIN, target, INT64_MAX, 0);
            if (ret < 0)
                goto end;
        }
        for (int i = 0; i < 3000; i++) {
            int kind;
            int64_t timestamp;

            ret = av_read_frame(s, pkt);
            if (ret < 0)
                goto end;
            kind = pkt->stream_index == selected;
            if (pkt->stream_index != video && !kind) {
                av_packet_unref(pkt);
                continue;
            }
            timestamp = av_rescale_q(pkt->dts,
                                    s->streams[pkt->stream_index]->time_base,
                                    AV_TIME_BASE_Q);
            av_assert0(pkt->dts != AV_NOPTS_VALUE);
            if (!seen[kind]++) {
                printf("stage=%d type=%d first_us=%"PRId64" target_us=%"PRId64"\n",
                       stage, kind, timestamp, target);
                fflush(stdout);
            }
            /* A timestamp seek may return the preceding two-second segment. */
            av_assert0(timestamp >= target - 2100000);
            progress[kind] = timestamp;
            av_packet_unref(pkt);
            if (progress[0] >= target + 2400000 &&
                progress[1] >= target + 2400000)
                break;
        }
        av_assert0(seen[0] && seen[1] &&
                   progress[0] >= target + 2400000 &&
                   progress[1] >= target + 2400000);
        av_assert0(llabs(progress[0] - progress[1]) < 1000000);
        target = FFMAX(progress[0], progress[1]);
    }
    ret = 0;
end:
    fprintf(stderr, "result=%d\n", ret);
    av_packet_free(&pkt);
    avformat_close_input(&s);
    return ret < 0;
}
