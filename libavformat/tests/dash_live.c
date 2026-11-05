/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "../dashdec.c"

static int open_attempts, open_errors, open_error;
static int64_t cancel_at;

static int test_interrupt(void *opaque)
{
    return cancel_at && av_gettime_relative() >= cancel_at;
}

static int test_io_open(AVFormatContext *s, AVIOContext **pb, const char *url,
                        int flags, AVDictionary **options)
{
    open_attempts++;
    if (open_attempts <= open_errors)
        return open_error;
    *pb = avio_alloc_context(av_malloc(32), 32, 0, NULL, NULL, NULL, NULL);
    return *pb ? 0 : AVERROR(ENOMEM);
}

static int test_io_close(AVFormatContext *s, AVIOContext *pb)
{
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    return 0;
}

static void test_fragment_retry(void)
{
    AVIOInterruptCB interrupt = {.callback = test_interrupt};
    char extensions[] = "ALL", url[] = "file:test.ts";
    DASHContext c = {.is_live = 1, .allowed_extensions = extensions,
                     .max_url_size = MAX_URL_SIZE, .interrupt_callback = &interrupt};
    AVFormatContext s = {.priv_data = &c, .io_open = test_io_open, .io_close2 = test_io_close};
    struct fragment fragment = {.url = url, .size = -1};
    struct representation rep = {.parent = &s, .cur_seg = &fragment};
    int64_t started;

    av_assert0(retryable_fragment_error(AVERROR_HTTP_NOT_FOUND));
    av_assert0(retryable_fragment_error(AVERROR_HTTP_SERVER_ERROR));
    av_assert0(!retryable_fragment_error(AVERROR_HTTP_FORBIDDEN));
    av_assert0(!retryable_fragment_error(AVERROR_HTTP_UNAUTHORIZED));
    av_assert0(!retryable_fragment_error(AVERROR_INVALIDDATA));
    open_attempts = 0;
    open_errors = 2;
    open_error = AVERROR_HTTP_NOT_FOUND;
    av_assert0(!open_media_fragment(&c, &rep) && open_attempts == 3);
    ff_format_io_close(&s, &rep.input);
    open_attempts = 0;
    open_errors = 10;
    av_assert0(open_media_fragment(&c, &rep) == open_error && open_attempts == 3);
    open_attempts = 0;
    open_error = AVERROR_HTTP_FORBIDDEN;
    av_assert0(open_media_fragment(&c, &rep) == open_error && open_attempts == 1);
    open_attempts = 0;
    open_error = AVERROR_HTTP_NOT_FOUND;
    c.is_live = 0;
    av_assert0(open_media_fragment(&c, &rep) == open_error && open_attempts == 1);
    c.is_live = 1;
    open_attempts = 0;
    started = av_gettime_relative();
    cancel_at = started + 30000;
    av_assert0(open_media_fragment(&c, &rep) == AVERROR_EXIT && open_attempts == 1);
    av_assert0(av_gettime_relative() - started < 250000);
    cancel_at = 0;
}

static void test_seek(void)
{
    struct timeline timeline = {.duration = 3840, .repeat = 7};
    struct timeline *times[] = {&timeline};
    struct representation rep = {.n_timelines = 1, .timelines = times,
        .fragment_timescale = 1000, .first_seq_no = 100, .last_seq_no = 107,
        .first_pts = AV_NOPTS_VALUE};
    DASHContext c = {.first_pts_time = AV_NOPTS_VALUE};
    AVFormatContext s = {.priv_data = &c};
    struct representation *videos[] = {&rep};
    AVFormatContext *seek_context = avformat_alloc_context();
    AVStream *stream = avformat_new_stream(seek_context, NULL);
    rep.parent = &s;
    av_assert0(!dash_seek(&s, &rep, 3839, 0, 1) && rep.cur_seq_no == 100);
    av_assert0(!dash_seek(&s, &rep, 3840, 0, 1) && rep.cur_seq_no == 101);
    av_assert0(!dash_seek(&s, &rep, 7680, 0, 1) && rep.cur_seq_no == 102);
    /* The origin is a shared media time, not ticks in the MPD timescale. */
    c.first_pts_time = 3840000;
    rep.first_pts = 184320; /* 48-kHz subdemuxer, 1-kHz manifest. */
    av_assert0(!dash_seek(&s, &rep, 0, 0, 1) && rep.cur_seq_no == 101);
    c.first_pts_time = AV_NOPTS_VALUE;
    timeline.starttime = INT64_C(10000000000000000);
    av_assert0(!dash_seek(&s, &rep, timeline.starttime + 3839, 0, 1) &&
               rep.cur_seq_no == 100);
    timeline.starttime = 0;
    /* Rounding the end down would seek to the next segment one ms early. */
    rep.fragment_timescale = 48000;
    timeline.duration = 184001;
    av_assert0(!dash_seek(&s, &rep, 3833, 0, 1) && rep.cur_seq_no == 100);
    av_assert0(!dash_seek(&s, &rep, 3834, 0, 1) && rep.cur_seq_no == 101);
    rep.fragment_timescale = 1000;
    timeline.duration = 3840;
    c.is_live = 1;
    c.min_buffer_time = 2;
    av_assert0(!dash_seek(&s, &rep, 1000000, 0, 1) && rep.cur_seq_no == 106);
    c.is_live = 0;
    rep.n_timelines = 0;
    rep.fragment_duration = 3840;
    av_assert0(!dash_seek(&s, &rep, 3839, 0, 1) && rep.cur_seq_no == 100);
    av_assert0(!dash_seek(&s, &rep, 3840, 0, 1) && rep.cur_seq_no == 101);
    /* Exercise the public callback with a time-base numerator other than 1. */
    c.n_videos = 1;
    c.videos = videos;
    seek_context->priv_data = &c;
    stream->time_base = (AVRational){1001, 30000};
    av_assert0(!dash_read_seek(seek_context, 0, 120, AVSEEK_FLAG_BACKWARD) &&
               rep.cur_seq_no == 101);
    seek_context->priv_data = NULL;
    avformat_free_context(seek_context);
}

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

    test_seek();
    test_fragment_retry();
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
    {
        struct representation fixed = {.fragment_duration = 3840, .fragment_timescale = 1000,
                                        .first_seq_no = 100};
        c.suggested_presentation_delay = 0;
        av_assert0(duration_live_delay(&c, &fixed) == 16);
        c.suggested_presentation_delay = 24;
        av_assert0(duration_live_delay(&c, &fixed) == 24);
        c.live_delay = 5;
        av_assert0(duration_live_delay(&c, &fixed) == 5);
        c.live_delay = c.suggested_presentation_delay = 0;
        c.is_startover = 0;
        c.availability_start_time = get_current_time_in_sec() - 120;
        av_assert0(calc_cur_seg_no(&s, &fixed) == 127);
        c.availability_start_time = get_current_time_in_sec() + 5;
        av_assert0(calc_cur_seg_no(&s, &fixed) == 100);
    }
    return 0;
}
