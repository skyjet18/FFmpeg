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
    DASHContext c = {.cur_timestamp = 1113750, .first_pts_time = AV_NOPTS_VALUE};
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
    c.first_pts_time = 3 * AV_TIME_BASE;
    av_assert0(!dash_seek(&s, &rep, 9500, 0, 1) && rep.cur_seq_no == 103);
    c.first_pts_time = AV_NOPTS_VALUE;
    /* Exercise the public callback with a time-base numerator other than 1. */
    c.n_videos = 1;
    c.videos = videos;
    seek_context->priv_data = &c;
    stream->time_base = (AVRational){1001, 30000};
    av_assert0(!dash_read_seek(seek_context, 0, 120, AVSEEK_FLAG_BACKWARD) &&
               rep.cur_seq_no == 101);
    seek_context->priv_data = NULL;
    avformat_free_context(seek_context);
    rep.n_fragments = 1;
    c.first_pts_time = 3 * AV_TIME_BASE;
    av_assert0(!dash_seek(&s, &rep, 12375, AVSEEK_FLAG_BACKWARD, 1));
    av_assert0(rep.seek_pending && rep.seek_pos_msec == 15375 &&
               rep.seek_flags == AVSEEK_FLAG_BACKWARD && rep.cur_timestamp == 1383750);
    av_assert0(dash_seek(&s, &rep, INT64_MAX, 0, 1) == AVERROR(EINVAL));
    av_assert0(rep.seek_pending && rep.seek_pos_msec == 15375);
    s.interrupt_callback.callback = test_interrupt;
    cancel_at = 1;
    av_assert0(recheck_discard_flags(&s, videos, 1) == AVERROR_EXIT);
    av_assert0(!rep.ctx && rep.seek_pending);
    cancel_at = 0;
}

static void test_reactivate_representation_position(void)
{
    struct timeline timeline = {.duration = 1000, .repeat = 30};
    struct timeline *timelines[] = {&timeline};
    AVIOInterruptCB interrupt = {.callback = test_interrupt};
    AVStream old_stream = {.discard = AVDISCARD_ALL};
    AVStream selected_stream = {.discard = AVDISCARD_DEFAULT};
    struct representation old = {
        .cur_seq_no = 109,
        .assoc_stream = &old_stream,
    };
    struct representation selected = {
        .type = AVMEDIA_TYPE_AUDIO,
        .n_timelines = 1,
        .timelines = timelines,
        .fragment_timescale = 1000,
        .first_seq_no = 900,
        .last_seq_no = 930,
        .cur_seq_no = 901,
        .assoc_stream = &selected_stream,
    };
    struct representation *audios[] = {&old, &selected};
    DASHContext c = {
        .cur_timestamp = 855000,
        .n_audios = 2,
        .audios = audios,
        .first_pts_time = 3 * AV_TIME_BASE,
    };
    AVFormatContext s = {
        .priv_data = &c,
        .interrupt_callback = interrupt,
    };

    old.parent = selected.parent = &s;
    cancel_at = 1;
    av_assert0(recheck_discard_flags(&s, audios, 2) == AVERROR_EXIT);
    av_assert0(selected.cur_seq_no == 912 && selected.drop_pending &&
               !selected.seek_pending && selected.drop_target == c.cur_timestamp &&
               selected.seek_pos_msec == 9500);
    {
        AVPacket packet = {.pts = 9000};
        AVRational time_base = {1, 1000};

        av_assert0(dash_drop_packet_before_seek_target(&selected, &packet, time_base));
        av_assert0(selected.drop_pending);
        packet.pts = 9600;
        av_assert0(!dash_drop_packet_before_seek_target(&selected, &packet, time_base));
        av_assert0(!selected.drop_pending);
        selected.drop_pending = 1;
        packet.pts = AV_NOPTS_VALUE;
        av_assert0(dash_drop_packet_before_seek_target(&selected, &packet, time_base) ==
                   AVERROR_INVALIDDATA);
        av_assert0(selected.drop_pending);
    }
    cancel_at = 0;
}

#if CONFIG_NUT_MUXER && CONFIG_NUT_DEMUXER
typedef struct FixtureBuffer {
    uint8_t *data;
    int size, position;
} FixtureBuffer;

static int fixture_read(void *opaque, uint8_t *buffer, int size)
{
    FixtureBuffer *fixture = opaque;
    size = FFMIN(size, fixture->size - fixture->position);
    if (!size)
        return AVERROR_EOF;
    memcpy(buffer, fixture->data + fixture->position, size);
    fixture->position += size;
    return size;
}

static int64_t fixture_seek(void *opaque, int64_t offset, int whence)
{
    FixtureBuffer *fixture = opaque;
    if (whence == AVSEEK_SIZE)
        return fixture->size;
    if (whence == SEEK_CUR)
        offset += fixture->position;
    else if (whence == SEEK_END)
        offset += fixture->size;
    else if (whence != SEEK_SET)
        return AVERROR(EINVAL);
    if (offset < 0 || offset > fixture->size)
        return AVERROR(EINVAL);
    fixture->position = offset;
    return offset;
}

static int fixture_open(AVFormatContext *parent, AVIOContext **pb, const char *url,
                        int flags, AVDictionary **options)
{
    AVFormatContext *mux = NULL;
    AVStream *stream;
    AVPacket *packet = av_packet_alloc();
    FixtureBuffer *fixture = av_mallocz(sizeof(*fixture));
    int origin, number, i;

    av_assert0(packet && fixture);
    av_assert0(sscanf(url, "file:test-%d-%d.nut", &origin, &number) == 2);
    av_assert0(!avformat_alloc_output_context2(&mux, NULL, "nut", NULL));
    stream = avformat_new_stream(mux, NULL);
    av_assert0(stream);
    stream->time_base = (AVRational){1, 48000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    stream->codecpar->sample_rate = 48000;
    av_channel_layout_default(&stream->codecpar->ch_layout, 1);
    stream->codecpar->bits_per_coded_sample = 16;
    av_assert0(!avio_open_dyn_buf(&mux->pb));
    av_assert0(!avformat_write_header(mux, NULL));
    for (i = 0; i < 10; i++) {
        av_assert0(!av_new_packet(packet, 9600));
        memset(packet->data, 0, packet->size);
        packet->pts = packet->dts = av_rescale_q((number - origin) * INT64_C(48000) +
                          i * 4800, (AVRational){1, 48000}, stream->time_base);
        packet->duration = av_rescale_q(4800, (AVRational){1, 48000}, stream->time_base);
        packet->flags = AV_PKT_FLAG_KEY;
        av_assert0(!av_write_frame(mux, packet));
        av_packet_unref(packet);
    }
    av_assert0(!av_write_trailer(mux));
    fixture->size = avio_close_dyn_buf(mux->pb, &fixture->data);
    av_assert0(fixture->size > 0);
    avformat_free_context(mux);
    av_packet_free(&packet);
    *pb = avio_alloc_context(av_malloc(4096), 4096, 0, fixture,
                             fixture_read, NULL, fixture_seek);
    av_assert0(*pb && (*pb)->buffer);
    return 0;
}

static int fixture_close(AVFormatContext *parent, AVIOContext *pb)
{
    FixtureBuffer *fixture = pb->opaque;
    av_free(fixture->data);
    av_free(fixture);
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    return 0;
}

static void test_reactivate_reopen(int live, int timeline_mode)
{
    char extensions[] = "ALL";
    AVIOInterruptCB interrupt = {.callback = test_interrupt};
    AVStream streams[2] = {{.discard = AVDISCARD_ALL}, {.discard = AVDISCARD_DEFAULT}};
    struct timeline timeline = {.duration = 1000, .repeat = 30};
    struct timeline *timelines[] = {&timeline};
    struct representation reps[2] = {{0}};
    struct representation *audios[] = {&reps[0], &reps[1]};
    DASHContext c = {.is_live = live, .first_pts_time = 3 * AV_TIME_BASE,
                     .cur_timestamp = 855000, .n_audios = 2, .audios = audios,
                     .allowed_extensions = extensions, .max_url_size = MAX_URL_SIZE,
                     .interrupt_callback = &interrupt, .minimum_update_period = 60,
                     .time_shift_buffer_depth = 20, .min_buffer_time = live ? 10 : 0};
    AVFormatContext *parent = avformat_alloc_context();
    AVPacket *packet = av_packet_alloc();
    int i;

    av_assert0(parent && packet);
    c.playlist_load_time = get_current_time_in_sec();
    c.availability_start_time = c.playlist_load_time - 30;
    parent->priv_data = &c;
    parent->io_open = fixture_open;
    parent->io_close2 = fixture_close;
    parent->interrupt_callback = interrupt;
    for (i = 0; i < 2; i++) {
        reps[i].parent = parent;
        reps[i].type = AVMEDIA_TYPE_AUDIO;
        reps[i].first_seq_no = i ? 900 : 100;
        reps[i].last_seq_no = reps[i].first_seq_no + 30;
        reps[i].fragment_timescale = 1000;
        reps[i].first_pts = AV_NOPTS_VALUE;
        reps[i].assoc_stream = &streams[i];
        reps[i].stream_index = i;
        if (timeline_mode) {
            reps[i].n_timelines = 1;
            reps[i].timelines = timelines;
        } else {
            reps[i].fragment_duration = 1000;
        }
        reps[i].url_template = av_strdup(i ? "file:test-900-$Number$.nut" :
                                            "file:test-100-$Number$.nut");
        av_assert0(reps[i].url_template);
    }
    /* Reopen and consume real packets, switching back and forth in one segment. */
    for (i = 0; i < 4; i++) {
        int selected = !(i & 1);
        int64_t target = 855000 + i * 9000;
        c.cur_timestamp = target;
        streams[selected].discard = AVDISCARD_DEFAULT;
        streams[!selected].discard = AVDISCARD_ALL;
        av_assert0(!recheck_discard_flags(parent, audios, 2));
        av_assert0(reps[selected].ctx && !reps[selected].seek_pending &&
                   reps[selected].drop_pending);
        av_assert0(reps[selected].cur_seq_no == reps[selected].first_seq_no +
                   (live && timeline_mode ? 11 : 12));
        av_assert0(!dash_read_packet(parent, packet));
        av_assert0(packet->stream_index == selected);
        av_assert0(av_rescale_q(packet->pts, reps[selected].ctx->streams[0]->time_base,
                               (AVRational){1, 90000}) == target);
        av_assert0(!reps[selected].drop_pending && c.cur_timestamp == target);
        av_assert0(reps[selected].cur_seq_no == reps[selected].first_seq_no + 12);
        av_packet_unref(packet);
    }
    for (i = 0; i < 2; i++) {
        if (reps[i].ctx)
            close_demux_for_component(&reps[i]);
        ff_format_io_close(parent, &reps[i].input);
        free_fragment(&reps[i].cur_seg);
        av_free(reps[i].url_template);
    }
    parent->priv_data = NULL;
    avformat_free_context(parent);
    av_packet_free(&packet);
}

static void test_single_file_reopen(void)
{
    char extensions[] = "ALL", url[] = "file:test-0-0.nut";
    AVIOInterruptCB interrupt = {.callback = test_interrupt};
    struct fragment fragment = {.url = url, .size = -1, .seekable = 1};
    struct fragment *fragments[] = {&fragment};
    struct representation rep = {.type = AVMEDIA_TYPE_AUDIO, .n_fragments = 1,
                                  .fragments = fragments, .first_pts = AV_NOPTS_VALUE};
    struct representation *audios[] = {&rep};
    DASHContext c = {.first_pts_time = AV_NOPTS_VALUE, .cur_timestamp = 45000,
                     .n_audios = 1, .audios = audios, .allowed_extensions = extensions,
                     .max_url_size = MAX_URL_SIZE, .interrupt_callback = &interrupt};
    AVFormatContext *parent = avformat_alloc_context();
    AVPacket *packet = av_packet_alloc();

    av_assert0(parent && packet);
    parent->priv_data = &c;
    parent->io_open = fixture_open;
    parent->io_close2 = fixture_close;
    parent->interrupt_callback = interrupt;
    rep.parent = parent;
    av_assert0(!recheck_discard_flags(parent, audios, 1));
    av_assert0(rep.ctx && !rep.seek_pending && rep.drop_pending);
    av_assert0(!dash_read_packet(parent, packet));
    av_assert0(av_rescale_q(packet->pts, rep.ctx->streams[0]->time_base,
                           (AVRational){1, 90000}) == 45000);
    av_assert0(!rep.drop_pending);
    close_demux_for_component(&rep);
    ff_format_io_close(parent, &rep.input);
    free_fragment(&rep.cur_seg);
    parent->priv_data = NULL;
    avformat_free_context(parent);
    av_packet_free(&packet);
}
#endif

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
    test_reactivate_representation_position();
#if CONFIG_NUT_MUXER && CONFIG_NUT_DEMUXER
    test_reactivate_reopen(0, 0);
    test_reactivate_reopen(0, 1);
    test_reactivate_reopen(1, 0);
    test_reactivate_reopen(1, 1);
    test_single_file_reopen();
#endif
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
