/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "libavformat/hls.c"

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

int main(void)
{
    test_preselection();
    test_sequence_reset();
    test_event_start();
    return 0;
}
