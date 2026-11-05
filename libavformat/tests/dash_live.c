/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "../dashdec.c"

int main(void)
{
    struct timeline video_time = {.starttime = 0, .duration = 3840, .repeat = 7};
    struct timeline audio_time = {.starttime = 0, .duration = 184320, .repeat = 7};
    struct timeline *vt[] = {&video_time}, *at[] = {&audio_time};
    struct representation video = {.n_timelines = 1, .timelines = vt, .fragment_timescale = 1000};
    struct representation audio = {.n_timelines = 1, .timelines = at, .fragment_timescale = 48000};
    struct representation *videos[] = {&video}, *audios[] = {&audio};
    DASHContext c = {.n_videos = 1, .videos = videos, .n_audios = 1, .audios = audios,
                     .suggested_presentation_delay = 24, .min_buffer_time = 2,
                     .is_live = 1, .live_start_time = AV_NOPTS_VALUE};
    AVFormatContext s = {.priv_data = &c};
    AVStream subtitle = {0};
    struct representation sub = {.assoc_stream = &subtitle};
    struct representation *subs[] = {&sub};
    void *options = av_mallocz(sizeof(DASHContext));

    av_assert0(options);
    *(const AVClass **)options = &dash_class;
    av_opt_set_defaults(options);
    av_assert0(!((DASHContext *)options)->subtitle_default_discard);
    av_assert0(!av_opt_set(options, "dash_subtitle_default_discard", "1", 0));
    av_assert0(((DASHContext *)options)->subtitle_default_discard);
    av_opt_free(options);
    av_free(options);

    /* A 30.72-second window must not start on its expiring oldest segment. */
    av_assert0(calc_common_live_start(&c) == 7680000);
    av_assert0(calc_cur_seg_no(&s, &video) == 3);
    av_assert0(calc_cur_seg_no(&s, &audio) == 3);
    av_assert0(c.live_start_time == 7680000);
    audio_time.duration = 184000;
    c.live_start_time = AV_NOPTS_VALUE;
    av_assert0(calc_cur_seg_no(&s, &video) == 3);
    av_assert0(calc_cur_seg_no(&s, &audio) == 3);
    audio_time.duration = 184320;
    c.time_shift_buffer_depth = 10;
    c.live_start_time = AV_NOPTS_VALUE;
    av_assert0(calc_cur_seg_no(&s, &video) == 7);
    av_assert0(calc_cur_seg_no(&s, &audio) == 7);
    c.time_shift_buffer_depth = 0;
    video_time.repeat = audio_time.repeat = 39;
    c.live_start_time = AV_NOPTS_VALUE;
    av_assert0(calc_common_live_start(&c) == 129600000);
    /* Different segment counters do not imply different media origins. */
    video.first_seq_no = 100;
    audio.first_seq_no = 900;
    av_assert0(calc_cur_seg_no(&s, &video) - 100 == calc_cur_seg_no(&s, &audio) - 900);
    c.is_startover = 1;
    av_assert0(calc_cur_seg_no(&s, &video) == 100);
    video_time.repeat = audio_time.repeat = 0;
    c.live_start_time = AV_NOPTS_VALUE;
    av_assert0(calc_common_live_start(&c) == -1000);

    c.n_subtitles = 1;
    c.subtitles = subs;
    discard_initial_subtitles(&c);
    av_assert0(subtitle.discard == AVDISCARD_DEFAULT);
    c.subtitle_default_discard = 1;
    /* Manifest-known subtitles may still be unprobed DATA/NONE streams. */
    discard_initial_subtitles(&c);
    av_assert0(subtitle.discard == AVDISCARD_ALL);
    return 0;
}
