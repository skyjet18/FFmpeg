/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "../hls.c"

int main(void)
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
    preselect_playlists(&c);
    av_assert0(!low.excluded && !high.excluded);
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
    av_assert0(!low.excluded && !high.excluded && audio[0].excluded &&
               !audio[139].excluded);
    c.hls_quality_mode = 1;
    preselect_playlists(&c);
    av_assert0(c.selected_variant == 0 && !low.excluded && high.excluded);
    /* A rendition URL shared with a selected variant must remain open. */
    renditions[0].playlist = &low;
    preselect_playlists(&c);
    av_assert0(!low.excluded);
    low.start_seq_no = 31;
    low.n_segments = 5;
    low.cur_seq_no = 42; /* Six beyond the window is still tolerated. */
    av_assert0(!resync_sequence(&low));
    low.cur_seq_no++;
    av_assert0(resync_sequence(&low) && low.cur_seq_no == 34);
    low.finished = 1;
    low.cur_seq_no = 325;
    av_assert0(!resync_sequence(&low));
    low.finished = 0;
    low.type = PLS_TYPE_EVENT;
    c.live_start_index = -3;
    c.cur_timestamp = AV_NOPTS_VALUE;
    c.first_packet = 1;
    av_assert0(select_cur_seq_no(&c, &low) == 33);
    return 0;
}
