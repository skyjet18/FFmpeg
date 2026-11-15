/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "libavformat/dashdec.c"

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

    av_assert0(seek_context && stream);
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

static void test_live_start(void)
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
    /* A window longer than the delay starts at the delay, as before. */
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
}

/* Segments of a fixed duration counted from availabilityStartTime. */
static void test_duration_live_start(void)
{
    struct representation fixed = {.fragment_duration = 3840, .fragment_timescale = 1000,
                                   .first_seq_no = 100};
    DASHContext c = {.min_buffer_time = 2, .is_live = 1,
                     .live_start_time = AV_NOPTS_VALUE};
    AVFormatContext s = {.priv_data = &c};

    av_assert0(duration_live_delay(&c, &fixed) == 16);
    c.suggested_presentation_delay = 24;
    av_assert0(duration_live_delay(&c, &fixed) == 24);
    c.live_delay = 5;
    av_assert0(duration_live_delay(&c, &fixed) == 5);
    c.live_delay = c.suggested_presentation_delay = 0;
    c.availability_start_time = get_current_time_in_sec() - 120;
    av_assert0(calc_cur_seg_no(&s, &fixed) == 127);
    /* 120 s are 31 segments and a quarter: the 32nd (number 131) is being
     * produced, the newest one to ask for is number 130 */
    av_assert0(calc_max_seg_no(&fixed, &c) == 130);
    /* suggestedPresentationDelay gives the segment it gave before */
    c.suggested_presentation_delay = 24;
    av_assert0(calc_cur_seg_no(&s, &fixed) == 125);
    c.suggested_presentation_delay = 0;
    c.availability_start_time = get_current_time_in_sec() + 5;
    av_assert0(calc_cur_seg_no(&s, &fixed) == 100);
}

static void test_subtitle_discard(void)
{
    AVStream subtitle = {0};
    AVStream *streams[] = {&subtitle};
    struct representation sub = {.assoc_stream = streams, .nb_assoc_stream = 1};
    struct representation *subs[] = {&sub};
    DASHContext c = {.n_subtitles = 1, .subtitles = subs};
    void *options = av_mallocz(sizeof(DASHContext));

    av_assert0(options);
    *(const AVClass **)options = &dash_class;
    av_opt_set_defaults(options);
    av_assert0(!((DASHContext *)options)->subtitle_default_discard);
    av_assert0(!av_opt_set(options, "dash_subtitle_default_discard", "1", 0));
    av_assert0(((DASHContext *)options)->subtitle_default_discard);
    av_opt_free(options);
    av_free(options);

    discard_initial_subtitles(&c);
    av_assert0(subtitle.discard == AVDISCARD_DEFAULT);
    c.subtitle_default_discard = 1;
    /* Manifest-known subtitles may still be unprobed DATA/NONE streams. */
    discard_initial_subtitles(&c);
    av_assert0(subtitle.discard == AVDISCARD_ALL);
}

/* A live presentation that ends: the reloaded manifest is static. */
#define MPD_HEAD(type) "<?xml version=\"1.0\"?>\n" \
    "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" " type \
    " availabilityStartTime=\"1970-01-01T00:00:00Z\" minBufferTime=\"PT2S\">\n"
#define MPD_VIDEO "<AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">" \
    "<Representation id=\"v\" bandwidth=\"1000\"><SegmentTemplate timescale=\"1000\" " \
    "duration=\"2000\" startNumber=\"1\" media=\"v_$Number$.m4s\"/></Representation></AdaptationSet>\n"
#define MPD_AUDIO(first, t) "<AdaptationSet contentType=\"audio\" mimeType=\"audio/mp4\">" \
    "<Representation id=\"a\" bandwidth=\"100\"><SegmentTemplate timescale=\"1000\" " \
    "startNumber=\"" first "\" media=\"a_$Number$.m4s\"><SegmentTimeline>" \
    "<S t=\"" t "\" d=\"2000\" r=\"9\"/></SegmentTimeline></SegmentTemplate></Representation></AdaptationSet>\n"

static const char live_mpd[] = MPD_HEAD("type=\"dynamic\" minimumUpdatePeriod=\"PT4S\"")
    "<Period id=\"p0\" start=\"PT0S\">\n" MPD_VIDEO MPD_AUDIO("11", "20000") "</Period></MPD>\n";
static const char ended_mpd[] = MPD_HEAD("type=\"static\" mediaPresentationDuration=\"PT60S\"")
    "<Period id=\"p0\" start=\"PT0S\">\n" MPD_VIDEO MPD_AUDIO("21", "40000") "</Period></MPD>\n";
static const char broken_mpd[] = MPD_HEAD("type=\"static\" mediaPresentationDuration=\"PT60S\"")
    "</MPD>\n";

static const char *mpd_text;
static int mpd_pos, segment_opens;

static int mpd_read(void *opaque, uint8_t *buf, int size)
{
    int left = strlen(mpd_text) - mpd_pos;

    if (left <= 0)
        return AVERROR_EOF;
    size = FFMIN(size, left);
    memcpy(buf, mpd_text + mpd_pos, size);
    mpd_pos += size;
    return size;
}

/* the manifest is mpd_text, every segment is missing */
static int mpd_io_open(AVFormatContext *s, AVIOContext **pb, const char *url,
                       int flags, AVDictionary **options)
{
    if (!av_match_ext(url, "mpd")) {
        segment_opens++;
        return AVERROR_HTTP_NOT_FOUND;
    }
    mpd_pos = 0;
    *pb = avio_alloc_context(av_malloc(4096), 4096, 0, NULL, mpd_read, NULL, NULL);
    return *pb ? 0 : AVERROR(ENOMEM);
}

static void test_live_to_static(void)
{
    AVIOInterruptCB interrupt = {.callback = test_interrupt};
    DASHContext c = {.video_rep_index = -1, .audio_rep_index = -1, .max_reload = 2,
                     .interrupt_callback = &interrupt, .live_start_time = AV_NOPTS_VALUE};
    AVFormatContext s = {.priv_data = &c, .io_open = mpd_io_open, .io_close2 = test_io_close,
                         .url = (char *)"http://localhost/live.mpd"};
    struct representation *video, *audio;
    struct fragment *seg;
    uint8_t buf[64];

    mpd_text = live_mpd;
    av_assert0(!parse_manifest(&s, s.url, NULL) && c.is_live);
    av_assert0(c.n_videos == 1 && c.n_audios == 1);
    video = c.videos[0];
    audio = c.audios[0];
    /* as open_demux_for_component() does */
    video->last_seq_no = calc_max_seg_no(video, &c);
    audio->last_seq_no = calc_max_seg_no(audio, &c);
    av_assert0(audio->last_seq_no == 20);
    audio->cur_seq_no = 21;

    /* SegmentTimeline: the static manifest gives the last segment, the
     * representation ends there instead of waiting for a next one */
    c.playlist_load_time = 0;
    mpd_text = ended_mpd;
    av_assert0(!refresh_manifest(&s));
    av_assert0(!c.is_live);
    av_assert0(audio->cur_seq_no == 21 && audio->last_seq_no == 30);
    audio->cur_seq_no = 31;
    av_assert0(!get_current_fragment(audio));
    /* SegmentTemplate with a duration: the end of mediaPresentationDuration
     * (one past the last segment, see dash_read_packet()) */
    av_assert0(video->last_seq_no == 31);
    video->cur_seq_no = 30;
    seg = get_current_fragment(video);
    av_assert0(seg && av_stristr(seg->url, "v_30.m4s"));
    free_fragment(&seg);
    video->cur_seq_no = 32;
    av_assert0(!get_current_fragment(video));

    /* a manifest that cannot be read does not end the presentation */
    c.is_live = 1;
    c.playlist_load_time = 0;
    mpd_text = broken_mpd;
    av_assert0(refresh_manifest(&s) < 0 && c.is_live);

    /* A SegmentTemplate with a duration is read by the clock, its manifest
     * is not reloaded while segments come: the segment after the end is
     * missing, the reloaded manifest says that the presentation has ended,
     * and the representation ends after that one segment. */
    free_video_list(&c);
    free_audio_list(&c);
    av_freep(&c.base_url);
    mpd_text = live_mpd;
    av_assert0(!parse_manifest(&s, s.url, NULL) && c.is_live);
    video = c.videos[0];
    video->last_seq_no = calc_max_seg_no(video, &c);
    c.playlist_load_time = 0;
    mpd_text = ended_mpd;
    segment_opens = 0;
    av_assert0(read_data(video, buf, sizeof(buf)) == AVERROR_EOF);
    av_assert0(!c.is_live && video->last_seq_no == 31);
    /* the first open and its two retries, no further segment */
    av_assert0(segment_opens == 3);

    free_video_list(&c);
    free_audio_list(&c);
    av_freep(&c.base_url);
}

int main(void)
{
    test_seek();
    test_fragment_retry();
    test_live_start();
    test_duration_live_start();
    test_subtitle_discard();
    test_live_to_static();
    return 0;
}
