/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
#include "../hls.c"

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
    struct playlist pls = {.ctx = avformat_alloc_context()};
    AVStream *audio, *video, *data;
    AVStream *first = test_stream(pls.ctx, 100, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC);
    AVStream *second = test_stream(pls.ctx, 200, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO);
    s->priv_data = &c;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    audio = pls.main_streams[0];
    video = pls.main_streams[1];
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
    {
        struct segment segment = {.duration = 10000000};
        struct segment *segments[] = {&segment};
        struct playlist *playlists[] = {&pls};
        c.ctx = s;
        c.first_timestamp = 0;
        c.n_playlists = 1;
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
    struct playlist pls = {.ctx = avformat_alloc_context()};
    AVStream *first = test_stream(pls.ctx, 0, AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE);
    AVStream *second = test_stream(pls.ctx, 0, AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE);
    AVStream *a, *b;
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
    /* Two changed PIDs are ambiguous: allocate two distinct new identities. */
    first->id = 10;
    second->id = 20;
    av_assert0(!update_streams_from_subdemuxer(s, &pls));
    av_assert0(pls.main_streams[0] != a && pls.main_streams[0] != b);
    av_assert0(pls.main_streams[1] != a && pls.main_streams[1] != b);
    av_assert0(pls.main_streams[0] != pls.main_streams[1]);
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

static void test_reactivation(void)
{
    HLSContext c = {.first_timestamp = 1000000, .cur_timestamp = 7500000};
    AVFormatContext *s = avformat_alloc_context();
    struct segment segment = {.duration = 2000000, .discontinuity_sequence = 4};
    struct segment *segments[] = {&segment, &segment, &segment, &segment};
    struct playlist embedded = {.ctx = avformat_alloc_context(), .needed = 1};
    struct playlist alternate = {.ctx = avformat_alloc_context(), .needed = 1,
        .finished = 1, .segments = segments, .n_segments = 4};
    struct playlist *playlists[] = {&embedded, &alternate};
    AVStream *video, *audio, *other;

    av_assert0(s && embedded.ctx && alternate.ctx);
    c.ctx = s;
    c.playlists = playlists;
    c.n_playlists = 2;
    s->priv_data = &c;
    embedded.parent = alternate.parent = s;
    test_stream(embedded.ctx, 100, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO);
    test_stream(embedded.ctx, 101, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC);
    test_stream(alternate.ctx, 100, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC);
    av_assert0(!update_streams_from_subdemuxer(s, &embedded));
    av_assert0(!update_streams_from_subdemuxer(s, &alternate));
    video = embedded.main_streams[0];
    audio = embedded.main_streams[1];
    other = alternate.main_streams[0];
    av_assert0(av_new_program(s, 0));
    for (unsigned i = 0; i < s->nb_streams; i++)
        av_program_add_stream_index(s, 0, i);
    other->discard = AVDISCARD_ALL;
    av_assert0(recheck_discard_flags(s, 1) == 1 && !alternate.needed);
    alternate.pkt = av_packet_alloc();
    av_assert0(alternate.pkt && !av_new_packet(alternate.pkt, 4));
    alternate.pb.pub.buffer = av_malloc(16);
    av_assert0(alternate.pb.pub.buffer);
    alternate.pb.pub.buf_ptr = alternate.pb.pub.buffer + 3;
    alternate.pb.pub.buf_end = alternate.pb.pub.buffer + 12;
    alternate.pb.pub.pos = 123;
    alternate.pb.pub.eof_reached = 1;
    alternate.pb.pub.error = AVERROR_EOF;
    alternate.discontinuity_pending = 1;
    alternate.discontinuity_offset = 123;
    audio->discard = AVDISCARD_ALL;
    other->discard = AVDISCARD_DEFAULT;
    av_assert0(recheck_discard_flags(s, 0) == 1);
    av_assert0(embedded.needed && alternate.needed);
    av_assert0(video->discard == AVDISCARD_DEFAULT && audio->discard == AVDISCARD_ALL);
    av_assert0(alternate.cur_seq_no == 3 && alternate.seek_timestamp == c.cur_timestamp);
    av_assert0(alternate.seek_flags == AVSEEK_FLAG_ANY && alternate.seek_stream_index == -1);
    av_assert0(!alternate.pkt->data && !alternate.pb.pub.pos &&
               !alternate.pb.pub.eof_reached && !alternate.pb.pub.error);
    av_assert0(alternate.pb.pub.buf_ptr == alternate.pb.pub.buffer &&
               alternate.pb.pub.buf_end == alternate.pb.pub.buffer);
    av_assert0(!alternate.discontinuity_pending &&
               alternate.discontinuity_offset == AV_NOPTS_VALUE &&
               alternate.active_discontinuity_sequence == 4);
    av_assert0(!recheck_discard_flags(s, 0));
    av_packet_free(&alternate.pkt);
    av_free(alternate.pb.pub.buffer);
    av_free(embedded.main_streams);
    av_free(embedded.associated_streams);
    av_free(alternate.main_streams);
    av_free(alternate.associated_streams);
    avformat_free_context(embedded.ctx);
    avformat_free_context(alternate.ctx);
    s->priv_data = NULL;
    avformat_free_context(s);
}

static void test_window_offsets(void)
{
    HLSContext c = {.first_timestamp = 1000000};
    struct segment segment = {.duration = 2000000};
    struct segment *segments[] = {&segment, &segment, &segment};
    struct playlist video = {.segments = segments, .n_segments = 3,
        .start_seq_no = 12, .window_start_offset = 4000000};
    struct playlist audio = {.segments = segments, .n_segments = 3,
        .start_seq_no = 91, .window_start_offset = 2000000};
    int64_t seq, start;

    av_assert0(find_timestamp_in_playlist(&c, &video, 7500000, &seq, &start));
    av_assert0(seq == 13 && start == 7000000);
    av_assert0(find_timestamp_in_playlist(&c, &audio, 7500000, &seq, &start));
    av_assert0(seq == 93 && start == 7000000);
    av_assert0(c.first_timestamp == 1000000);
}

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

    test_stream_mapping();
    test_reactivation();
    test_window_offsets();
    test_stream_mapping_edges();
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
