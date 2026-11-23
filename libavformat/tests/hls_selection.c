/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "libavformat/hls.c"

static AVStream *test_stream(AVFormatContext *s, int id, enum AVMediaType type,
                            enum AVCodecID codec)
{
    AVStream *stream = avformat_new_stream(s, NULL);
    av_assert0(stream);
    stream->id = id;
    stream->codecpar->codec_type = type;
    stream->codecpar->codec_id = codec;
    stream->time_base = (AVRational){1, 90000};
    return stream;
}

static void test_stream_mapping(void)
{
    HLSContext c = {0};
    AVFormatContext *s = avformat_alloc_context();
    struct playlist pls = {.ctx = avformat_alloc_context(), .timed_id3_stream_index = -1};
    AVStream *audio, *video, *data;
    AVStream *first, *second;

    av_assert0(s && pls.ctx);
    first  = test_stream(pls.ctx, 100, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC);
    second = test_stream(pls.ctx, 200, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO);
    s->priv_data = &c;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    audio = pls.main_streams[0];
    video = pls.main_streams[1];
    /* Nothing changed: the streams and their number stay. */
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] == audio && pls.main_streams[1] == video &&
               s->nb_streams == 2 && pls.n_associated_streams == 2);
    video->discard = AVDISCARD_DEFAULT;
    audio->discard = AVDISCARD_ALL;
    /* A video-only ad occupies the old audio index and adds ID3 in the video slot. */
    first->id = 200;
    first->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    first->codecpar->codec_id = AV_CODEC_ID_MPEG2VIDEO;
    second->id = 300;
    second->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    second->codecpar->codec_id = AV_CODEC_ID_TIMED_ID3;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] == video && video->discard == AVDISCARD_DEFAULT);
    av_assert0(pls.main_streams[1] != audio && pls.main_streams[1] != video);
    data = pls.main_streams[1];
    av_assert0(data->codecpar->codec_type == AVMEDIA_TYPE_DATA);
    av_assert0(audio->codecpar->codec_type == AVMEDIA_TYPE_AUDIO);
    av_assert0(pls.timed_id3_stream_index == 1);
    {
        struct segment segment = {.duration = 10000000};
        struct segment *segments[] = {&segment};
        /* Playlists a seek has to leave alone: excluded by the preselection
         * (never loaded), broken, without segments. They have no packet. */
        struct playlist excluded = {.excluded = 1}, broken = {.broken = 1}, empty = {0};
        struct playlist *playlists[] = {&excluded, &pls, &broken, &empty};
        excluded.seek_timestamp = broken.seek_timestamp = empty.seek_timestamp = AV_NOPTS_VALUE;
        excluded.segments = broken.segments = segments;
        excluded.n_segments = broken.n_segments = 1;
        c.ctx = s;
        c.first_timestamp = 0;
        c.n_playlists = 4;
        c.playlists = playlists;
        pls.parent = s;
        pls.segments = segments;
        pls.n_segments = 1;
        pls.pkt = av_packet_alloc();
        av_assert0(pls.pkt && av_new_program(s, 0));
        av_program_add_stream_index(s, 0, audio->index);
        audio->discard = AVDISCARD_DEFAULT;
        video->discard = data->discard = AVDISCARD_ALL;
        /* An absent selected track must neither stop the playlist nor block seeks. */
        av_assert0(playlist_needed(&pls));
        av_assert0(!hls_read_seek(s, audio->index, 0, AVSEEK_FLAG_ANY));
        av_assert0(pls.seek_stream_index == audio->index);
        av_assert0(pls.seek_timestamp == 0 &&
                   excluded.seek_timestamp == AV_NOPTS_VALUE &&
                   broken.seek_timestamp == AV_NOPTS_VALUE &&
                   empty.seek_timestamp == AV_NOPTS_VALUE);
        audio->discard = AVDISCARD_ALL;
        av_assert0(!playlist_needed(&pls));
        av_packet_free(&pls.pkt);
        c.n_playlists = 0;
    }
    /* Returning to the original layout must recover the same selected slots. */
    first->id = 100;
    first->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    first->codecpar->codec_id = AV_CODEC_ID_AAC;
    second->id = 200;
    second->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    second->codecpar->codec_id = AV_CODEC_ID_MPEG2VIDEO;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] == audio && pls.main_streams[1] == video);
    /* Two audio identities must never collapse to the first audio stream. */
    second->id = 101;
    second->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    second->codecpar->codec_id = AV_CODEC_ID_AAC;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] == audio && pls.main_streams[1] != audio);
    {
        AVStream *other_audio = pls.main_streams[1];
        first->id = 101;
        second->id = 100;
        av_assert0(!update_streams_from_subdemuxer(s, &pls));
        av_assert0(pls.main_streams[0] == other_audio && pls.main_streams[1] == audio);
        first->id = 102; /* Exactly one unmatched incoming and stored audio. */
        av_assert0(!update_streams_from_subdemuxer(s, &pls));
        av_assert0(pls.main_streams[0] == other_audio && pls.main_streams[1] == audio);
    }
    av_free(pls.main_streams);
    av_free(pls.associated_streams);
    avformat_free_context(pls.ctx);
    s->priv_data = NULL;
    avformat_free_context(s);
}

static void test_stream_mapping_edges(void)
{
    HLSContext c = {0};
    AVFormatContext *s = avformat_alloc_context();
    struct playlist pls = {.ctx = avformat_alloc_context(), .timed_id3_stream_index = -1};
    AVStream *first, *second;
    AVStream *a, *b, *late;

    av_assert0(s && pls.ctx);
    first  = test_stream(pls.ctx, 0, AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE);
    second = test_stream(pls.ctx, 0, AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE);
    s->priv_data = &c;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    a = pls.main_streams[0];
    b = pls.main_streams[1];
    av_assert0(a != b);
    /* No usable IDs: retain positional identities as codecs become known. */
    first->codecpar->codec_type = second->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    first->codecpar->codec_id = second->codecpar->codec_id = AV_CODEC_ID_AAC;
    a->discard = AVDISCARD_ALL;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] == a && pls.main_streams[1] == b);
    av_assert0(a->codecpar->codec_id == AV_CODEC_ID_AAC && a->discard == AVDISCARD_ALL);
    first->codecpar->codec_id = AV_CODEC_ID_MP3;
    first->time_base = (AVRational){1, 48000};
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] == a && a->codecpar->codec_id == AV_CODEC_ID_MP3);
    av_assert0(a->time_base.den == 48000 && a->discard == AVDISCARD_ALL);
    /* A stream the subdemuxer finds later is appended, the others stay. */
    test_stream(pls.ctx, 7, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO);
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    late = pls.main_streams[2];
    av_assert0(pls.main_streams[0] == a && pls.main_streams[1] == b &&
               late != a && late != b && late->index == 2 && s->nb_streams == 3);
    /* Two changed PIDs are ambiguous: allocate two distinct new identities. */
    first->id = 10;
    second->id = 20;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] != a && pls.main_streams[0] != b);
    av_assert0(pls.main_streams[1] != a && pls.main_streams[1] != b);
    av_assert0(pls.main_streams[0] != pls.main_streams[1]);
    av_assert0(pls.main_streams[2] == late);
    /* Returning duplicate IDs are still ambiguous: do not guess an old selection. */
    first->id = second->id = 0;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] != pls.main_streams[1]);
    av_assert0(pls.main_streams[0] != a && pls.main_streams[1] != b);
    av_free(pls.main_streams);
    av_free(pls.associated_streams);
    avformat_free_context(pls.ctx);
    s->priv_data = NULL;
    avformat_free_context(s);
}

static void test_preselection(void)
{
    struct playlist low = {0}, high = {0}, audio[140];
    struct playlist *low_pls[] = {&low}, *high_pls[] = {&high};
    struct variant low_var = {.bandwidth = 100, .n_playlists = 1, .playlists = low_pls};
    struct variant high_var = {.bandwidth = 900, .n_playlists = 1, .playlists = high_pls};
    struct variant *variants[] = {&low_var, &high_var};
    struct rendition renditions[140] = {{0}};
    struct rendition *rendition_ptrs[140];
    struct playlist *playlists[142] = {&low, &high};
    HLSContext c = {.n_variants = 2, .variants = variants,
                    .n_playlists = 142, .playlists = playlists,
                    .n_renditions = 140, .renditions = rendition_ptrs};
    int i;

    memset(audio, 0, sizeof(audio));
    av_strlcpy(low_var.audio_group, "audio", sizeof(low_var.audio_group));
    av_strlcpy(high_var.audio_group, "audio", sizeof(high_var.audio_group));
    for (i = 0; i < 140; i++) {
        renditions[i].type = AVMEDIA_TYPE_AUDIO;
        av_strlcpy(renditions[i].group_id, "audio", sizeof(renditions[i].group_id));
        renditions[i].playlist = &audio[i];
        rendition_ptrs[i] = &renditions[i];
        playlists[i + 2] = &audio[i];
    }
    /* the defaults exclude nothing */
    preselect_playlists(&c);
    av_assert0(!low.excluded && !high.excluded);
    for (i = 0; i < 140; i++)
        av_assert0(!audio[i].excluded);
    c.hls_quality_mode = 2;
    preselect_playlists(&c);
    av_assert0(c.selected_variant == 1 && low.excluded && !high.excluded);
    for (i = 0; i < 140; i++)
        av_assert0(!audio[i].excluded);
    c.hls_audio_default_only = 1;
    preselect_playlists(&c);
    for (i = 0; i < 140; i++)
        av_assert0(!audio[i].excluded); /* No default: retain the entire group. */
    renditions[139].disposition = AV_DISPOSITION_DEFAULT;
    preselect_playlists(&c);
    for (i = 0; i < 140; i++)
        av_assert0(audio[i].excluded == (i != 139));
    c.hls_quality_mode = 0;
    preselect_playlists(&c);
    av_assert0(c.selected_variant == 0 && !low.excluded && !high.excluded &&
               audio[0].excluded && !audio[139].excluded);
    c.hls_quality_mode = 1;
    preselect_playlists(&c);
    av_assert0(c.selected_variant == 0 && !low.excluded && high.excluded);
    /* A rendition URL shared with a selected variant must remain open. */
    c.hls_quality_mode = 2;
    renditions[0].playlist = &low;
    preselect_playlists(&c);
    av_assert0(c.selected_variant == 1 && low.excluded);
    renditions[0].disposition = AV_DISPOSITION_DEFAULT;
    renditions[139].disposition = 0;
    preselect_playlists(&c);
    av_assert0(!low.excluded && !high.excluded && audio[139].excluded);
    /* The renditions of another group go with their variant. */
    av_strlcpy(low_var.audio_group, "other", sizeof(low_var.audio_group));
    av_strlcpy(renditions[5].group_id, "other", sizeof(renditions[5].group_id));
    c.hls_audio_default_only = 0;
    preselect_playlists(&c);
    av_assert0(audio[5].excluded && !audio[6].excluded && !high.excluded);
    c.hls_quality_mode = 1;
    preselect_playlists(&c);
    av_assert0(!audio[5].excluded && audio[6].excluded && high.excluded && !low.excluded);
    /* Both options off again: everything is back. */
    c.hls_quality_mode = 0;
    preselect_playlists(&c);
    av_assert0(!audio[5].excluded && !audio[6].excluded && !high.excluded && !low.excluded);
}

/* The media sequence of a live playlist restarted far below the position
 * read ("avformat/hls: resync to the live edge after a media sequence
 * wrap"): reading goes on one segment before the live edge. A position up
 * to six segments past the window is a lagging playlist, not a restart. */
static void test_sequence_reset(void)
{
    struct segment seg = {.duration = 300000};
    struct segment *segments[] = {&seg, &seg, &seg, &seg, &seg};
    struct playlist pls = {.start_seq_no = 31, .n_segments = 5, .segments = segments};
    HLSContext c = {.max_reload = 1, .m3u8_hold_counters = 1000};

    pls.cur_seq_no = pls.last_seq_no = 42; /* Six beyond the window is still tolerated. */
    pls.last_load_time = av_gettime_relative();
    av_assert0(reload_playlist(&pls, &c) == AVERROR_EOF && pls.cur_seq_no == 42);
    pls.cur_seq_no = pls.last_seq_no = 43;
    pls.last_load_time = av_gettime_relative();
    av_assert0(!reload_playlist(&pls, &c) && pls.cur_seq_no == 34 && pls.last_seq_no == 34);
}

/* An EVENT playlist that is not finished starts like any live playlist. */
static void test_event_start(void)
{
    struct segment seg = {.duration = 300000};
    struct segment *segments[] = {&seg, &seg, &seg, &seg, &seg};
    struct playlist pls = {.start_seq_no = 31, .n_segments = 5, .segments = segments,
                           .type = PLS_TYPE_EVENT};
    HLSContext c = {.live_start_index = -3, .cur_timestamp = AV_NOPTS_VALUE,
                    .first_timestamp = AV_NOPTS_VALUE, .first_packet = 1};

    av_assert0(select_cur_seq_no(&c, &pls) == 33);
    pls.finished = 1;
    av_assert0(select_cur_seq_no(&c, &pls) == 31);
}

/* A live rendition without discontinuities that is switched in again: by
 * the media sequence number of the playlists being read, as before, unless
 * its timeline anchor says that this number is another position. */
static void test_live_switch_in(void)
{
    struct segment seg = {.duration = 2000000};
    struct segment *segments[] = {&seg, &seg, &seg, &seg, &seg};
    struct playlist pls = {.start_seq_no = 110, .n_segments = 5, .segments = segments,
                           .timeline_seq_no = 110, .timeline_time = 20000000};
    HLSContext c = {.live_start_index = -3, .first_timestamp = 1000000};

    /* the playlists being read count from another number: by time */
    pls.last_load_time = av_gettime_relative();
    c.cur_seq_no = 12;
    c.cur_timestamp = 27500000;
    av_assert0(select_cur_seq_no(&c, &pls) == 113);
    c.cur_timestamp = 20000000;
    av_assert0(select_cur_seq_no(&c, &pls) == 110);
    c.cur_timestamp = 29999999;
    av_assert0(select_cur_seq_no(&c, &pls) == 114);
    /* a playing position that is not in the window by the anchor: the
     * anchor is not used, live_start_index as before */
    c.cur_timestamp = 19000000;
    av_assert0(select_cur_seq_no(&c, &pls) == 112);
    c.cur_timestamp = 30000000;
    av_assert0(select_cur_seq_no(&c, &pls) == 112);
    /* an anchor that is not on the first segment of the window */
    pls.timeline_seq_no = 112;
    pls.timeline_time   = 24000000;
    c.cur_timestamp = 27500000;
    av_assert0(select_cur_seq_no(&c, &pls) == 113);
    c.cur_timestamp = 21000000;
    av_assert0(select_cur_seq_no(&c, &pls) == 110);
    /* the same numbering: the number is taken, also one segment off */
    c.cur_seq_no = 112;
    c.cur_timestamp = 25000000;
    av_assert0(select_cur_seq_no(&c, &pls) == 112);
    c.cur_timestamp = 26100000;
    av_assert0(select_cur_seq_no(&c, &pls) == 112);
    c.cur_timestamp = 23900000;
    av_assert0(select_cur_seq_no(&c, &pls) == 112);
    /* a number that is in the window by chance, but two segments away */
    c.cur_seq_no = 110;
    c.cur_timestamp = 27500000;
    av_assert0(select_cur_seq_no(&c, &pls) == 113);
    /* no anchor (the window has passed since the playlist was read): the
     * number if it is in the window, else live_start_index, as before */
    pls.timeline_time = AV_NOPTS_VALUE;
    av_assert0(select_cur_seq_no(&c, &pls) == 110);
    c.cur_seq_no = 12;
    av_assert0(select_cur_seq_no(&c, &pls) == 112);
}

/* A selected subtitle rendition is read at most one segment ahead of the
 * audio/video position, and waiting for that requests nothing and does not
 * count as a playlist that holds. */
static void test_subtitle_wait(void)
{
    HLSContext c = {.first_timestamp = 0, .av_timestamp = 0,
                    .m3u8_hold_counters = 1000};
    AVFormatContext *s = avformat_alloc_context();
    struct segment segment = {.duration = 10000000};
    struct segment *segments[] = {&segment, &segment, &segment, &segment, &segment};
    struct playlist video = {.needed = 1};
    struct playlist subtitle = {.parent = s, .needed = 1, .is_subtitle = 1,
                                .finished = 1, .segments = segments, .n_segments = 5,
                                .cur_seq_no = 3, .last_seq_no = 3,
                                .timeline_time = AV_NOPTS_VALUE,
                                .subtitle_gate_origin = AV_NOPTS_VALUE};
    struct playlist *playlists[] = {&video, &subtitle};

    av_assert0(s);
    s->priv_data = &c;
    c.ctx = s;
    c.playlists = playlists;
    c.n_playlists = 2;
    for (int i = 0; i < 1500; i++)
        av_assert0(read_subtitle_packet(&subtitle, NULL) == AVERROR(EAGAIN));
    av_assert0(!subtitle.m3u8_hold_counters);
    /* segment 2 is played: segment 3 may be loaded, segment 4 not yet */
    c.av_timestamp = 20000000;
    av_assert0(subtitle_segment_ready(&c, &subtitle));
    subtitle.cur_seq_no = 4;
    av_assert0(!subtitle_segment_ready(&c, &subtitle));
    c.av_timestamp = 29999999;
    av_assert0(!subtitle_segment_ready(&c, &subtitle));
    c.av_timestamp = 30000000;
    av_assert0(subtitle_segment_ready(&c, &subtitle));
    /* no audio/video position yet: nothing */
    c.av_timestamp = AV_NOPTS_VALUE;
    av_assert0(!subtitle_segment_ready(&c, &subtitle));
    /* no audio/video playlist: the subtitles are all there is */
    video.needed = 0;
    av_assert0(subtitle_segment_ready(&c, &subtitle));
    video.needed = 1;

    /* live: the segment selected is taken to start at the position played
     * when the rendition was switched in */
    subtitle.finished = 0;
    subtitle.cur_seq_no = 2;
    c.av_timestamp = 500000000;
    av_assert0(subtitle_segment_ready(&c, &subtitle));
    av_assert0(subtitle.subtitle_gate_origin == 480000000);
    subtitle.cur_seq_no = 3;
    av_assert0(subtitle_segment_ready(&c, &subtitle));
    subtitle.cur_seq_no = 4;
    av_assert0(!subtitle_segment_ready(&c, &subtitle));
    c.av_timestamp = 510000000;
    av_assert0(subtitle_segment_ready(&c, &subtitle));

    /* the end of a live window: no reload before its interval */
    subtitle.cur_seq_no = 5;
    subtitle.last_load_time = av_gettime_relative();
    for (int i = 0; i < 1500; i++)
        av_assert0(read_subtitle_packet(&subtitle, NULL) == AVERROR(EAGAIN));
    av_assert0(!subtitle.m3u8_hold_counters);
    s->priv_data = NULL;
    avformat_free_context(s);
}

/* The stream of a subtitle rendition that is not loaded yet: WebVTT,
 * discarded, with the language and disposition of the rendition. */
static void test_lazy_subtitle_stream(void)
{
    HLSContext c = {0};
    AVFormatContext *s = avformat_alloc_context();
    struct rendition rend = {.type = AVMEDIA_TYPE_SUBTITLE,
                             .disposition = AV_DISPOSITION_FORCED};
    struct rendition *rends[] = {&rend};
    struct playlist pls = {.parent = s, .index = 3, .is_subtitle = 1,
                           .renditions = rends, .n_renditions = 1};

    av_assert0(s);
    s->priv_data = &c;
    c.ctx = s;
    strcpy(rend.language, "de");
    av_assert0(add_lazy_subtitle_stream(s, &pls) == 0);
    av_assert0(s->nb_streams == 1 && pls.n_main_streams == 1);
    av_assert0(s->streams[0]->id == 3);
    av_assert0(s->streams[0]->discard == AVDISCARD_ALL);
    av_assert0(s->streams[0]->codecpar->codec_id == AV_CODEC_ID_WEBVTT);
    av_assert0(s->streams[0]->disposition & AV_DISPOSITION_FORCED);
    av_assert0(!strcmp(av_dict_get(s->streams[0]->metadata, "language", NULL, 0)->value, "de"));
    /* not needed while discarded, without a demuxer context */
    av_assert0(av_new_program(s, 0));
    av_program_add_stream_index(s, 0, 0);
    av_assert0(!playlist_needed(&pls));
    s->streams[0]->discard = AVDISCARD_DEFAULT;
    av_assert0(playlist_needed(&pls));
    av_freep(&pls.main_streams);
    av_freep(&pls.associated_streams);
    s->priv_data = NULL;
    avformat_free_context(s);
}

int main(void)
{
    test_subtitle_wait();
    test_lazy_subtitle_stream();
    test_live_switch_in();
    test_stream_mapping();
    test_stream_mapping_edges();
    test_preselection();
    test_sequence_reset();
    test_event_start();
    return 0;
}
