/*
 * Apple HTTP Live Streaming demuxer
 * Copyright (c) 2010 Martin Storsjo
 * Copyright (c) 2013 Anssi Hannula
 * Copyright (c) 2021 Nachiket Tarate
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Apple HTTP Live Streaming demuxer
 * https://www.rfc-editor.org/rfc/rfc8216.txt
 */

#include "config_components.h"

#include "libavformat/http.h"
#include "libavutil/aes.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/base64.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/dict.h"
#include "libavutil/time.h"
#include "libavutil/uuid.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "avio_internal.h"
#include "id3v2.h"
#include "url.h"

#include "hls_sample_encryption.h"

#define INITIAL_BUFFER_SIZE 32768

/* timestamp offsets of the last discontinuity regions, see HLSContext */
#define HLS_DISC_OFFSETS 16

#define MAX_FIELD_LEN 64
#define MAX_CHARACTERISTICS_LEN 512

#define MPEG_TIME_BASE 90000
#define MPEG_TIME_BASE_Q (AVRational){1, MPEG_TIME_BASE}

/* live_start_index not set by the caller: -3, but an unfinished EVENT
 * playlist starts on its first segment */
#define LIVE_START_INDEX_AUTO INT_MIN

/*
 * An apple http stream consists of a playlist with media segment files,
 * played sequentially. There may be several playlists with the same
 * video content, in different bandwidth variants, that are played in
 * parallel (preferably only one bandwidth variant at a time). In this case,
 * the user supplied the url to a main playlist that only lists the variant
 * playlists.
 *
 * If the main playlist doesn't point at any variants, we still create
 * one anonymous toplevel variant for this, to maintain the structure.
 */

enum KeyType {
    KEY_NONE,
    KEY_AES_128,
    KEY_SAMPLE_AES
};

struct segment {
    int64_t duration;
    int64_t discontinuity_sequence;
    int64_t url_offset;
    int64_t size;
    int discontinuity;
    char *url;
    char *key;
    enum KeyType key_type;
    uint8_t iv[16];
    /* index of the CENC key tags in playlist.cenc_key_tags, -1 if none */
    int cenc_key_tags;
    /* associated Media Initialization Section, treated as a segment */
    struct segment *init_section;
};

struct rendition;

/* What the CENC key tags in front of a segment carry for the key requests of
 * the mov demuxer (cenc_decryption_key_url): the pssh boxes of their data:
 * URIs and the KEYID. */
struct cenc_key_tags {
    char *pssh;     /* base64 pssh boxes, comma separated, or NULL */
    char kid[33];   /* KEYID, 32 lowercase hex digits, or empty */
};

enum PlaylistType {
    PLS_TYPE_UNSPECIFIED,
    PLS_TYPE_EVENT,
    PLS_TYPE_VOD
};

/*
 * Each playlist has its own demuxer. If it currently is active,
 * it has an open AVIOContext too, and potentially an AVPacket
 * containing the next packet from this stream.
 */

/* A main demuxer stream of a playlist and the id its subdemuxer stream had
 * when it was created or last matched. */
struct hls_stream {
    AVStream *stream;
    int id;
};

struct playlist {
    char url[MAX_URL_SIZE];
    FFIOContext pb;
    uint8_t* read_buffer;
    AVIOContext *input;
    int input_read_done;
    int64_t input_end; /* end of the byte range requested on input, or -1 */
    int discontinuity_pending;
    AVIOContext *input_next;
    int input_next_requested;
    int64_t input_next_end;
    AVFormatContext *parent;
    int index;
    AVFormatContext *ctx;
    AVPacket *pkt;
    int has_noheader_flag;

    /* main demuxer streams associated with this playlist
     * indexed by the subdemuxer stream indexes */
    AVStream **main_streams;
    int n_main_streams;
    /* every main demuxer stream ever created for this playlist, also those
     * the subdemuxer does not deliver at the moment */
    struct hls_stream *associated_streams;
    int n_associated_streams;

    int finished;
    enum PlaylistType type;
    int64_t target_duration;
    int64_t start_seq_no;
    int time_offset_flag;
    int64_t start_time_offset;
    int n_segments;
    struct segment **segments;
    int needed;
    int broken;
    /* left out by hls_quality_mode / hls_audio_default_only: the playlist is
     * never loaded and has no demuxer and no streams */
    int excluded;
    int64_t cur_seq_no;
    int64_t last_seq_no;
    /* #EXT-X-DISCONTINUITY: has_discontinuity is set once the playlist has
     * carried the tag (kept across reloads); everything that changes the
     * reading (demuxer restarts, timestamp offsets, selection by timestamp)
     * is gated on it, a playlist without the tag is read as before.
     * discontinuity_sequence_seen: the last parse had a valid
     * #EXT-X-DISCONTINUITY-SEQUENCE, the region numbers are those of the
     * server and shared with the other playlists. */
    int has_discontinuity;
    int discontinuity_sequence_seen;
    /* The discontinuity sequence of the region the segment demuxer
     * currently reads, the first segment of that region and the offset
     * (AV_TIME_BASE) that maps the raw timestamps of the region onto the
     * continuous presentation timeline, AV_NOPTS_VALUE until the first
     * packet of the region tells it. discontinuity_region_start: the next
     * packet is the first one since the region was (re)started. */
    int64_t active_discontinuity_sequence;
    int64_t discontinuity_start_seq_no;
    int64_t discontinuity_offset;
    int discontinuity_region_start;
    /* Anchor of the presentation timeline on this playlist: segment
     * timeline_seq_no starts at timeline_time (AV_TIME_BASE). Set by the
     * first packet after a (re)start, moved along when segments drop out of
     * a live window, AV_NOPTS_VALUE until then and after a reload that
     * shares no segment with the previous list. */
    int64_t timeline_seq_no;
    int64_t timeline_time;
    int m3u8_hold_counters;
    int64_t cur_seg_offset;
    int64_t last_load_time;

    /* Currently active Media Initialization Section */
    struct segment *cur_init_section;
    uint8_t *init_sec_buf;
    unsigned int init_sec_buf_size;
    unsigned int init_sec_data_len;
    unsigned int init_sec_buf_read_offset;

    /* The start of the segment that skip_image_head() read: head_buf holds
     * head_len bytes, read_from_url() returns those from head_pos on before
     * it reads the input again. head_check: a segment was started on the
     * input and is not checked yet. */
    uint8_t *head_buf;
    unsigned int head_buf_size;
    int head_len;
    int head_pos;
    int head_check;
    int head_logged;

    char key_url[MAX_URL_SIZE];
    uint8_t key[16];

    /* The key URL whose requests failed, how often in a row and the error of
     * the last try. Once it counts as failed for good (key_failed_for_good())
     * it is not requested again for the following segments until the
     * playlist is reloaded or a segment names another key; after an error
     * that may pass (5xx, timeout) only until segment key_retry_seq_no or a
     * seek. */
    char key_fail_url[MAX_URL_SIZE];
    int key_fail_count;
    int key_fail_ret;
    int64_t key_retry_seq_no;

    /* The distinct CENC key tag sets of the playlist (only collected with
     * cenc_decryption_key_url) and the one passed to the segment demuxer,
     * as index + 1 (0: none). */
    struct cenc_key_tags **cenc_key_tags;
    int n_cenc_key_tags;
    int cenc_key_tags_applied;

    /* ID3 timestamp handling (elementary audio streams have ID3 timestamps
     * (and possibly other ID3 tags) in the beginning of each segment) */
    int is_id3_timestamped; /* -1: not yet known */
    int64_t id3_mpegts_timestamp; /* in mpegts tb */
    int64_t id3_offset; /* in stream original tb */
    uint8_t* id3_buf; /* temp buffer for id3 parsing */
    unsigned int id3_buf_size;
    AVDictionary *id3_initial; /* data from first id3 tag */
    int id3_found; /* ID3 tag found at some point */
    int id3_changed; /* ID3 tag data has changed at some point */
    ID3v2ExtraMeta *id3_deferred_extra; /* stored here until subdemuxer is opened */

    HLSAudioSetupInfo audio_setup_info;

    /* Constant offset of this playlist's DTS baseline relative to
     * c->first_timestamp. */
    int64_t ts_offset;
    int64_t seek_timestamp;
    int seek_flags;
    int seek_stream_index; /* into main demuxer stream array */

    /* Renditions associated with this playlist, if any.
     * Alternative rendition playlists have a single rendition associated
     * with them, and variant main Media Playlists may have
     * multiple (playlist-less) renditions associated with them. */
    int n_renditions;
    struct rendition **renditions;

    /* Media Initialization Sections (EXT-X-MAP) associated with this
     * playlist, if any. */
    int n_init_sections;
    struct segment **init_sections;
    int is_subtitle; /* Indicates if it's a subtitle playlist */
    /* Live subtitle playlist: where the first segment of its window is on
     * the audio/video timeline, estimated when it was switched in (see
     * subtitle_segment_ready()); only used to pace the segment requests. */
    int64_t subtitle_gate_origin;

    /* Some HLS streams repeat the current timed-ID3 metadata at every segment
     * boundary so late-joining clients get a fresh copy. Track the last seen
     * value to avoid raising AVSTREAM_EVENT_FLAG_METADATA_UPDATED spuriously
     * on those duplicates. timed_id3_stream_index is the subdemuxer stream
     * index of the first timed-ID3 stream seen; -1 if none. */
    AVDictionary *timed_id3_metadata;
    int timed_id3_stream_index;
};

/*
 * Renditions are e.g. alternative subtitle or audio streams.
 * The rendition may either be an external playlist or it may be
 * contained in the main Media Playlist of the variant (in which case
 * playlist is NULL).
 */
struct rendition {
    enum AVMediaType type;
    struct playlist *playlist;
    char group_id[MAX_FIELD_LEN];
    char language[MAX_FIELD_LEN];
    char name[MAX_FIELD_LEN];
    int disposition;
};

struct variant {
    int bandwidth;

    /* every variant contains at least the main Media Playlist in index 0 */
    int n_playlists;
    struct playlist **playlists;

    char audio_group[MAX_FIELD_LEN];
    char video_group[MAX_FIELD_LEN];
    char subtitles_group[MAX_FIELD_LEN];
};

typedef struct HLSContext {
    AVClass *class;
    AVFormatContext *ctx;
    int n_variants;
    struct variant **variants;
    int n_playlists;
    struct playlist **playlists;
    int n_renditions;
    struct rendition **renditions;

    int64_t cur_seq_no;
    int m3u8_hold_counters;
    int live_start_index;
    int prefer_x_start;
    int first_packet;
    int64_t first_timestamp;
    struct playlist *first_timestamp_pls;
    int64_t cur_timestamp;
    /* Timestamp offsets of the last discontinuity regions. An entry without
     * owner belongs to a region numbered by #EXT-X-DISCONTINUITY-SEQUENCE
     * and is shared by all playlists that carry it (the renditions of a
     * stream carry the same timestamps and the same numbers); the local
     * numbers of a playlist without it only match its own entries. */
    struct {
        int64_t sequence;
        int64_t offset;
        const struct playlist *owner;
    } disc_offsets[HLS_DISC_OFFSETS];
    int n_disc_offsets;
    int disc_offsets_next;
    AVIOInterruptCB *interrupt_callback;
    AVDictionary *avio_opts;
    AVDictionary *seg_format_opts;
    char *cenc_decryption_key_url;
    char *allowed_extensions;
    char *allowed_segment_extensions;
    int extension_picky;
    int skip_image_head;
    int max_reload;
    char *key_uri_replace_old;
    char *key_uri_replace_new;
    int http_persistent;
    int http_multiple;
    int http_seekable;
    int seg_max_retry;
    int cenc_logged; /* the notice about CENC encrypted segments was logged */
    AVIOContext *playlist_pb;
    HLSCryptoContext  crypto_ctx;
    int hls_quality_mode;
    int hls_audio_default_only;
    /* playing position of the audio/video playlists on the timeline of
     * c->first_timestamp (subtitle packets left out) */
    int64_t av_timestamp;
    /* the variant whose main playlist tells duration and seekability:
     * the one chosen by hls_quality_mode, else the first */
    int selected_variant;
} HLSContext;

static void free_segment_dynarray(struct segment **segments, int n_segments)
{
    int i;
    for (i = 0; i < n_segments; i++) {
        av_freep(&segments[i]->key);
        av_freep(&segments[i]->url);
        av_freep(&segments[i]);
    }
}

static void free_segment_list(struct playlist *pls)
{
    free_segment_dynarray(pls->segments, pls->n_segments);
    av_freep(&pls->segments);
    pls->n_segments = 0;
}

static void free_init_section_list(struct playlist *pls)
{
    int i;
    for (i = 0; i < pls->n_init_sections; i++) {
        av_freep(&pls->init_sections[i]->key);
        av_freep(&pls->init_sections[i]->url);
        av_freep(&pls->init_sections[i]);
    }
    av_freep(&pls->init_sections);
    pls->n_init_sections = 0;
}

static void free_playlist_list(HLSContext *c)
{
    int i;
    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];
        free_segment_list(pls);
        free_init_section_list(pls);
        for (int j = 0; j < pls->n_cenc_key_tags; j++) {
            av_freep(&pls->cenc_key_tags[j]->pssh);
            av_freep(&pls->cenc_key_tags[j]);
        }
        av_freep(&pls->cenc_key_tags);
        av_freep(&pls->main_streams);
        av_freep(&pls->associated_streams);
        av_freep(&pls->renditions);
        av_freep(&pls->id3_buf);
        av_dict_free(&pls->id3_initial);
        av_dict_free(&pls->timed_id3_metadata);
        ff_id3v2_free_extra_meta(&pls->id3_deferred_extra);
        av_freep(&pls->init_sec_buf);
        av_freep(&pls->head_buf);
        av_packet_free(&pls->pkt);
        av_freep(&pls->pb.pub.buffer);
        ff_format_io_close(c->ctx, &pls->input);
        pls->input_read_done = 0;
        ff_format_io_close(c->ctx, &pls->input_next);
        pls->input_next_requested = 0;
        if (pls->ctx) {
            pls->ctx->pb = NULL;
            avformat_close_input(&pls->ctx);
        }
        av_free(pls);
    }
    av_freep(&c->playlists);
    c->n_playlists = 0;
}

static void free_variant_list(HLSContext *c)
{
    int i;
    for (i = 0; i < c->n_variants; i++) {
        struct variant *var = c->variants[i];
        av_freep(&var->playlists);
        av_free(var);
    }
    av_freep(&c->variants);
    c->n_variants = 0;
}

static void free_rendition_list(HLSContext *c)
{
    int i;
    for (i = 0; i < c->n_renditions; i++)
        av_freep(&c->renditions[i]);
    av_freep(&c->renditions);
    c->n_renditions = 0;
}

static struct playlist *new_playlist(HLSContext *c, const char *url,
                                     const char *base)
{
    struct playlist *pls;
    char abs_url[MAX_URL_SIZE];

    ff_make_absolute_url(abs_url, sizeof(abs_url), base, url);
    if (!abs_url[0])
        return NULL;

    for (int i = 0; i < c->n_playlists; i++) {
        if (!strcmp(c->playlists[i]->url, abs_url))
            return c->playlists[i];
    }

    pls = av_mallocz(sizeof(struct playlist));
    if (!pls)
        return NULL;
    pls->pkt = av_packet_alloc();
    if (!pls->pkt) {
        av_free(pls);
        return NULL;
    }
    av_strlcpy(pls->url, abs_url, sizeof(pls->url));
    pls->ts_offset = AV_NOPTS_VALUE;
    pls->seek_timestamp = AV_NOPTS_VALUE;
    pls->discontinuity_offset = 0;
    pls->timeline_time = AV_NOPTS_VALUE;
    pls->subtitle_gate_origin = AV_NOPTS_VALUE;

    pls->is_id3_timestamped = -1;
    pls->id3_mpegts_timestamp = AV_NOPTS_VALUE;
    pls->timed_id3_stream_index = -1;

    dynarray_add(&c->playlists, &c->n_playlists, pls);
    return pls;
}

struct variant_info {
    char bandwidth[20];
    /* variant group ids: */
    char audio[MAX_FIELD_LEN];
    char video[MAX_FIELD_LEN];
    char subtitles[MAX_FIELD_LEN];
};

static struct variant *new_variant(HLSContext *c, struct variant_info *info,
                                   const char *url, const char *base)
{
    struct variant *var;
    struct playlist *pls;

    pls = new_playlist(c, url, base);
    if (!pls)
        return NULL;

    var = av_mallocz(sizeof(struct variant));
    if (!var)
        return NULL;

    if (info) {
        var->bandwidth = atoi(info->bandwidth);
        strcpy(var->audio_group, info->audio);
        strcpy(var->video_group, info->video);
        strcpy(var->subtitles_group, info->subtitles);
    }

    dynarray_add(&c->variants, &c->n_variants, var);
    dynarray_add(&var->playlists, &var->n_playlists, pls);
    return var;
}

static void handle_variant_args(void *context, const char *key,
                                int key_len, char **dest, int *dest_len)
{
    struct variant_info *info = context;
    if (!strncmp(key, "BANDWIDTH=", key_len)) {
        *dest     =        info->bandwidth;
        *dest_len = sizeof(info->bandwidth);
    } else if (!strncmp(key, "AUDIO=", key_len)) {
        *dest     =        info->audio;
        *dest_len = sizeof(info->audio);
    } else if (!strncmp(key, "VIDEO=", key_len)) {
        *dest     =        info->video;
        *dest_len = sizeof(info->video);
    } else if (!strncmp(key, "SUBTITLES=", key_len)) {
        *dest     =        info->subtitles;
        *dest_len = sizeof(info->subtitles);
    }
}

struct key_info {
     char uri[MAX_URL_SIZE];
     char method[16]; /* the longest: SAMPLE-AES-CTR, ISO-23001-7 */
     char iv[35];
     char keyformat[64];
     char keyid[35];
};

static void handle_key_args(void *context, const char *key,
                            int key_len, char **dest, int *dest_len)
{
    struct key_info *info = context;
    if (!strncmp(key, "METHOD=", key_len)) {
        *dest     =        info->method;
        *dest_len = sizeof(info->method);
    } else if (!strncmp(key, "URI=", key_len)) {
        *dest     =        info->uri;
        *dest_len = sizeof(info->uri);
    } else if (!strncmp(key, "IV=", key_len)) {
        *dest     =        info->iv;
        *dest_len = sizeof(info->iv);
    } else if (!strncmp(key, "KEYFORMAT=", key_len)) {
        *dest     =        info->keyformat;
        *dest_len = sizeof(info->keyformat);
    } else if (!strncmp(key, "KEYID=", key_len)) {
        *dest     =        info->keyid;
        *dest_len = sizeof(info->keyid);
    }
}

/**
 * The system ID of the DRM system a KEYFORMAT names, if it is one that uses
 * ISO Common Encryption: urn:uuid:<system ID> (Widevine, PlayReady, ...) or
 * com.microsoft.playready. Not identity, not FairPlay.
 */
static int cenc_keyformat_system_id(const char *keyformat, AVUUID system_id)
{
    static const AVUUID playready = {
        0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86,
        0xab, 0x92, 0xe6, 0x5b, 0xe0, 0x88, 0x5f, 0x95
    };

    if (!av_strcasecmp(keyformat, "com.microsoft.playready")) {
        av_uuid_copy(system_id, playready);
        return 1;
    }
    return av_strncasecmp(keyformat, "urn:uuid:", 9) ? 0 :
           !av_uuid_parse(keyformat + 9, system_id);
}

/**
 * Add what a CENC key tag carries for the key requests: the KEYID, and the
 * data: URI as a pssh box. The URI of a Widevine tag is a whole pssh box,
 * the one of a PlayReady tag the PlayReady object, which is put into a pssh
 * box of its system. Anything else in the URI (a license server URL) is not
 * for the key request.
 */
static int cenc_key_tag_add(const struct key_info *info, const AVUUID system_id,
                            char **pssh, char *kid)
{
    const char *data = info->uri, *comma;
    uint8_t *buf, *box;
    char *b64, *joined;
    int size, box_size;

    if (!kid[0] && !av_strncasecmp(info->keyid, "0x", 2) && strlen(info->keyid) == 34) {
        int i;
        for (i = 2; i < 34 && av_isxdigit(info->keyid[i]); i++)
            kid[i - 2] = av_tolower(info->keyid[i]);
        kid[i == 34 ? 32 : 0] = 0;
    }

    if (av_strncasecmp(data, "data:", 5) || !(comma = strchr(data, ',')) ||
        comma - data < 12 || av_strncasecmp(comma - 7, ";base64", 7))
        return 0;
    data = comma + 1;
    size = AV_BASE64_DECODE_SIZE(strlen(data));
    buf = av_malloc(size + 32);
    if (!buf)
        return AVERROR(ENOMEM);
    size = av_base64_decode(buf + 32, data, size);
    box = buf + 32;
    box_size = size;
    if (size <= 0) {
        av_free(buf);
        return 0;
    }
    if (size < 32 || AV_RB32(box) != size || AV_RL32(box + 4) != MKTAG('p','s','s','h')) {
        /* the data of the system: a version 0 box around it */
        box = buf;
        box_size = size + 32;
        AV_WB32(box, box_size);
        AV_WL32(box + 4, MKTAG('p','s','s','h'));
        AV_WB32(box + 8, 0);
        memcpy(box + 12, system_id, 16);
        AV_WB32(box + 28, size);
    }

    b64 = av_malloc(AV_BASE64_SIZE(box_size));
    if (!b64) {
        av_free(buf);
        return AVERROR(ENOMEM);
    }
    av_base64_encode(b64, AV_BASE64_SIZE(box_size), box, box_size);
    av_free(buf);
    if (*pssh && strstr(*pssh, b64)) {
        av_free(b64);
        return 0;
    }
    joined = *pssh ? av_asprintf("%s,%s", *pssh, b64) : av_strdup(b64);
    av_free(b64);
    if (!joined)
        return AVERROR(ENOMEM);
    av_free(*pssh);
    *pssh = joined;
    return 0;
}

/* The index of the set of CENC key tags in pls->cenc_key_tags, added if new. */
static int cenc_key_tags_index(struct playlist *pls, const char *pssh, const char *kid)
{
    struct cenc_key_tags *tags;

    for (int i = 0; i < pls->n_cenc_key_tags; i++) {
        tags = pls->cenc_key_tags[i];
        if (!strcmp(tags->kid, kid) && !strcmp(tags->pssh ? tags->pssh : "", pssh ? pssh : ""))
            return i;
    }
    tags = av_mallocz(sizeof(*tags));
    if (!tags)
        return AVERROR(ENOMEM);
    if (pssh && !(tags->pssh = av_strdup(pssh))) {
        av_free(tags);
        return AVERROR(ENOMEM);
    }
    av_strlcpy(tags->kid, kid, sizeof(tags->kid));
    if (av_dynarray_add_nofree(&pls->cenc_key_tags, &pls->n_cenc_key_tags, tags) < 0) {
        av_free(tags->pssh);
        av_free(tags);
        return AVERROR(ENOMEM);
    }
    return pls->n_cenc_key_tags - 1;
}

struct init_section_info {
    char uri[MAX_URL_SIZE];
    char byterange[32];
};

static struct segment *new_init_section(struct playlist *pls,
                                        struct init_section_info *info,
                                        const char *url_base)
{
    struct segment *sec;
    char tmp_str[MAX_URL_SIZE], *ptr = tmp_str;

    if (!info->uri[0])
        return NULL;

    sec = av_mallocz(sizeof(*sec));
    if (!sec)
        return NULL;

    if (!av_strncasecmp(info->uri, "data:", 5)) {
        ptr = info->uri;
    } else {
        ff_make_absolute_url(tmp_str, sizeof(tmp_str), url_base, info->uri);
        if (!tmp_str[0]) {
            av_free(sec);
            return NULL;
        }
    }
    sec->url = av_strdup(ptr);
    if (!sec->url) {
        av_free(sec);
        return NULL;
    }

    if (info->byterange[0]) {
        sec->size = strtoll(info->byterange, NULL, 10);
        ptr = strchr(info->byterange, '@');
        if (ptr)
            sec->url_offset = strtoll(ptr+1, NULL, 10);
        if (sec->size < 0 || sec->url_offset < 0 || sec->size > INT64_MAX - sec->url_offset) {
            av_freep(&sec->url);
            av_free(sec);
            return NULL;
        }
    } else {
        /* the entire file is the init section */
        sec->size = -1;
    }

    dynarray_add(&pls->init_sections, &pls->n_init_sections, sec);

    return sec;
}

static void handle_init_section_args(void *context, const char *key,
                                     int key_len, char **dest, int *dest_len)
{
    struct init_section_info *info = context;
    if (!strncmp(key, "URI=", key_len)) {
        *dest     =        info->uri;
        *dest_len = sizeof(info->uri);
    } else if (!strncmp(key, "BYTERANGE=", key_len)) {
        *dest     =        info->byterange;
        *dest_len = sizeof(info->byterange);
    }
}

struct rendition_info {
    char type[16];
    char uri[MAX_URL_SIZE];
    char group_id[MAX_FIELD_LEN];
    char language[MAX_FIELD_LEN];
    char assoc_language[MAX_FIELD_LEN];
    char name[MAX_FIELD_LEN];
    char defaultr[4];
    char forced[4];
    char characteristics[MAX_CHARACTERISTICS_LEN];
};

static struct rendition *new_rendition(HLSContext *c, struct rendition_info *info,
                                      const char *url_base)
{
    struct rendition *rend;
    enum AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
    char *characteristic;
    char *chr_ptr;
    char *saveptr;

    if (!strcmp(info->type, "AUDIO"))
        type = AVMEDIA_TYPE_AUDIO;
    else if (!strcmp(info->type, "VIDEO"))
        type = AVMEDIA_TYPE_VIDEO;
    else if (!strcmp(info->type, "SUBTITLES"))
        type = AVMEDIA_TYPE_SUBTITLE;
    else if (!strcmp(info->type, "CLOSED-CAPTIONS"))
        /* CLOSED-CAPTIONS is ignored since we do not support CEA-608 CC in
         * AVC SEI RBSP anyway */
        return NULL;

    if (type == AVMEDIA_TYPE_UNKNOWN) {
        av_log(c->ctx, AV_LOG_WARNING, "Can't support the type: %s\n", info->type);
        return NULL;
    }

    /* URI is mandatory for subtitles as per spec */
    if (type == AVMEDIA_TYPE_SUBTITLE && !info->uri[0]) {
        av_log(c->ctx, AV_LOG_ERROR, "The URI tag is REQUIRED for subtitle.\n");
        return NULL;
    }

    rend = av_mallocz(sizeof(struct rendition));
    if (!rend)
        return NULL;

    dynarray_add(&c->renditions, &c->n_renditions, rend);

    rend->type = type;
    strcpy(rend->group_id, info->group_id);
    strcpy(rend->language, info->language);
    strcpy(rend->name, info->name);

    /* add the playlist if this is an external rendition */
    if (info->uri[0]) {
        rend->playlist = new_playlist(c, info->uri, url_base);
        if (rend->playlist) {
            if (type == AVMEDIA_TYPE_SUBTITLE) {
                rend->playlist->is_subtitle = 1;
                rend->playlist->is_id3_timestamped = 0;
            }
            dynarray_add(&rend->playlist->renditions,
                         &rend->playlist->n_renditions, rend);
        }
    }

    if (info->assoc_language[0]) {
        size_t langlen = strlen(rend->language);
        if (langlen < sizeof(rend->language) - 3) {
            size_t assoc_len;
            rend->language[langlen] = ',';
            assoc_len = av_strlcpy(rend->language + langlen + 1,
                                   info->assoc_language,
                                   sizeof(rend->language) - langlen - 1);
            if (langlen + assoc_len + 2 > sizeof(rend->language)) // truncation occurred
                av_log(c->ctx, AV_LOG_WARNING, "Truncated rendition language: %s\n",
                       info->assoc_language);
        }
    }

    if (!strcmp(info->defaultr, "YES"))
        rend->disposition |= AV_DISPOSITION_DEFAULT;
    if (!strcmp(info->forced, "YES"))
        rend->disposition |= AV_DISPOSITION_FORCED;

    chr_ptr = info->characteristics;
    while ((characteristic = av_strtok(chr_ptr, ",", &saveptr))) {
        if (!strcmp(characteristic, "public.accessibility.describes-music-and-sound"))
            rend->disposition |= AV_DISPOSITION_HEARING_IMPAIRED;
        else if (!strcmp(characteristic, "public.accessibility.describes-video"))
            rend->disposition |= AV_DISPOSITION_VISUAL_IMPAIRED;

        chr_ptr = NULL;
    }

    return rend;
}

static void handle_rendition_args(void *vinfo, const char *key,
                                  int key_len, char **dest, int *dest_len)
{
    struct rendition_info *info = vinfo;

    if (!strncmp(key, "TYPE=", key_len)) {
        *dest     =        info->type;
        *dest_len = sizeof(info->type);
    } else if (!strncmp(key, "URI=", key_len)) {
        *dest     =        info->uri;
        *dest_len = sizeof(info->uri);
    } else if (!strncmp(key, "GROUP-ID=", key_len)) {
        *dest     =        info->group_id;
        *dest_len = sizeof(info->group_id);
    } else if (!strncmp(key, "LANGUAGE=", key_len)) {
        *dest     =        info->language;
        *dest_len = sizeof(info->language);
    } else if (!strncmp(key, "ASSOC-LANGUAGE=", key_len)) {
        *dest     =        info->assoc_language;
        *dest_len = sizeof(info->assoc_language);
    } else if (!strncmp(key, "NAME=", key_len)) {
        *dest     =        info->name;
        *dest_len = sizeof(info->name);
    } else if (!strncmp(key, "DEFAULT=", key_len)) {
        *dest     =        info->defaultr;
        *dest_len = sizeof(info->defaultr);
    } else if (!strncmp(key, "FORCED=", key_len)) {
        *dest     =        info->forced;
        *dest_len = sizeof(info->forced);
    } else if (!strncmp(key, "CHARACTERISTICS=", key_len)) {
        *dest     =        info->characteristics;
        *dest_len = sizeof(info->characteristics);
    }
    /*
     * ignored:
     * - AUTOSELECT: client may autoselect based on e.g. system language
     * - INSTREAM-ID: EIA-608 closed caption number ("CC1".."CC4")
     */
}

/* used by parse_playlist to allocate a new variant+playlist when the
 * playlist is detected to be a Media Playlist (not Master Playlist)
 * and we have no parent Master Playlist (parsing of which would have
 * allocated the variant and playlist already)
 * *pls == NULL  => Master Playlist or parentless Media Playlist
 * *pls != NULL => parented Media Playlist, playlist+variant allocated */
static int ensure_playlist(HLSContext *c, struct playlist **pls, const char *url)
{
    if (*pls)
        return 0;
    if (!new_variant(c, NULL, url, NULL))
        return AVERROR(ENOMEM);
    *pls = c->playlists[c->n_playlists - 1];
    return 0;
}

static int open_url_keepalive(AVFormatContext *s, AVIOContext **pb,
                              const char *url, AVDictionary **options)
{
#if !CONFIG_HTTP_PROTOCOL
    return AVERROR_PROTOCOL_NOT_FOUND;
#else
    int ret;
    URLContext *uc = ffio_geturlcontext(*pb);
    av_assert0(uc);
    (*pb)->eof_reached = 0;
    ret = ff_http_do_new_request2(uc, url, options);
    if (ret < 0) {
        ff_format_io_close(s, pb);
    }
    return ret;
#endif
}

static int open_url(AVFormatContext *s, AVIOContext **pb, const char *url,
                    AVDictionary **opts, AVDictionary *opts2, int *is_http_out)
{
    HLSContext *c = s->priv_data;
    AVDictionary *tmp = NULL;
    const char *proto_name = NULL;
    int ret;
    int is_http = 0;

    if (av_strstart(url, "crypto", NULL)) {
        if (url[6] == '+' || url[6] == ':')
            proto_name = avio_find_protocol_name(url + 7);
    } else if (av_strstart(url, "data", NULL)) {
        if (url[4] == '+' || url[4] == ':')
            proto_name = avio_find_protocol_name(url + 5);
    }

    if (!proto_name)
        proto_name = avio_find_protocol_name(url);

    if (!proto_name)
        return AVERROR_INVALIDDATA;

    // only http(s) & file are allowed
    if (av_strstart(proto_name, "file", NULL)) {
        if (strcmp(c->allowed_extensions, "ALL") && !av_match_ext(url, c->allowed_extensions)) {
            av_log(s, AV_LOG_ERROR,
                "Filename extension of \'%s\' is not a common multimedia extension, blocked for security reasons.\n"
                "If you wish to override this adjust allowed_extensions, you can set it to \'ALL\' to allow all\n",
                url);
            return AVERROR_INVALIDDATA;
        }
    } else if (av_strstart(proto_name, "http", NULL)) {
        is_http = 1;
    } else if (av_strstart(proto_name, "data", NULL)) {
        ;
    } else
        return AVERROR_INVALIDDATA;

    if (!strncmp(proto_name, url, strlen(proto_name)) && url[strlen(proto_name)] == ':')
        ;
    else if (av_strstart(url, "crypto", NULL) && !strncmp(proto_name, url + 7, strlen(proto_name)) && url[7 + strlen(proto_name)] == ':')
        ;
    else if (av_strstart(url, "data", NULL) && !strncmp(proto_name, url + 5, strlen(proto_name)) && url[5 + strlen(proto_name)] == ':')
        ;
    else if (strcmp(proto_name, "file") || !strncmp(url, "file,", 5))
        return AVERROR_INVALIDDATA;

    av_dict_copy(&tmp, *opts, 0);
    av_dict_copy(&tmp, opts2, 0);

    if (is_http && c->http_persistent && *pb) {
        ret = open_url_keepalive(c->ctx, pb, url, &tmp);
        if (ret == AVERROR_EXIT) {
            av_dict_free(&tmp);
            return ret;
        } else if (ret < 0) {
            if (ret != AVERROR_EOF)
                av_log(s, AV_LOG_WARNING,
                    "keepalive request failed for '%s' with error: '%s' when opening url, retrying with new connection\n",
                    url, av_err2str(ret));
            av_dict_copy(&tmp, *opts, 0);
            av_dict_copy(&tmp, opts2, 0);
            ret = s->io_open(s, pb, url, AVIO_FLAG_READ, &tmp);
        }
    } else {
        ret = s->io_open(s, pb, url, AVIO_FLAG_READ, &tmp);
    }
    if (ret >= 0) {
        // update cookies on http response with setcookies.
        char *new_cookies = NULL;

        if (!(s->flags & AVFMT_FLAG_CUSTOM_IO))
            av_opt_get(*pb, "cookies", AV_OPT_SEARCH_CHILDREN, (uint8_t**)&new_cookies);

        if (new_cookies)
            av_dict_set(opts, "cookies", new_cookies, AV_DICT_DONT_STRDUP_VAL);
    }

    av_dict_free(&tmp);

    if (is_http_out)
        *is_http_out = is_http;

    return ret;
}

static int test_segment(AVFormatContext *s, const AVInputFormat *in_fmt, struct playlist *pls, struct segment *seg)
{
    HLSContext *c = s->priv_data;
    int matchA = 3;
    int matchF = 0;

    if (!c->extension_picky)
        return 0;

    if (strcmp(c->allowed_segment_extensions, "ALL"))
        matchA =      av_match_ext    (seg->url, c->allowed_segment_extensions)
                 + 2*(ff_match_url_ext(seg->url, c->allowed_segment_extensions) > 0);

    if (!matchA) {
        av_log(s, AV_LOG_ERROR, "URL %s is not in allowed_segment_extensions, consider updating hls.c and submitting a patch to ffmpeg-devel, if this should be added\n", seg->url);
        return AVERROR_INVALIDDATA;
    }

    if (in_fmt) {
        if (in_fmt->extensions) {
            matchF =      av_match_ext(    seg->url, in_fmt->extensions)
                     + 2*(ff_match_url_ext(seg->url, in_fmt->extensions) > 0);
            // Youtube uses aac files with .ts extension
            if(av_match_name("mp4", in_fmt->name) || av_match_name("aac", in_fmt->name)) {
                matchF |=      av_match_ext(    seg->url, "ts,m2t,m2ts,mts,mpg,m4s,mpeg,mpegts,cmfv,cmfa")
                          + 2*(ff_match_url_ext(seg->url, "ts,m2t,m2ts,mts,mpg,m4s,mpeg,mpegts,cmfv,cmfa") > 0);
            }
        } else if (!strcmp(in_fmt->name, "mpegts")) {
            const char *str = "ts,m2t,m2ts,mts,mpg,m4s,mpeg,mpegts"
                              ",html" // https://flash1.bogulus.cfd/
                            ;
            matchF =      av_match_ext(    seg->url, str)
                     + 2*(ff_match_url_ext(seg->url, str) > 0);
        } else if (!strcmp(in_fmt->name, "webvtt")) {
            matchF =      av_match_ext(    seg->url, "vtt,webvtt")
                     + 2*(ff_match_url_ext(seg->url, "vtt,webvtt") > 0);
        }

        if (!(matchA & matchF)) {
            av_log(s, AV_LOG_ERROR, "detected format %s extension %s mismatches allowed extensions in url %s\n", in_fmt->name, in_fmt->extensions ? in_fmt->extensions : "none", seg->url);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

/* After the reload of a live playlist: keep the discontinuity sequence
 * numbers and the timeline anchor consistent with the previous segment
 * list. */
static void reload_discontinuity_state(HLSContext *c, struct playlist *pls,
                                       struct segment **prev_segments,
                                       int prev_n_segments,
                                       int64_t prev_start_seq_no,
                                       int sequence_seen)
{
    /* index of the new first segment in the previous list */
    int64_t overlap = pls->start_seq_no - prev_start_seq_no;
    /* a segment both lists contain: prev_segments[pi] is pls->segments[ni] */
    int64_t pi = -1, ni = -1, delta = 0, i, j;
    /* Without #EXT-X-DISCONTINUITY-SEQUENCE the numbering of every parse
     * starts at 0 and changes whenever a discontinuity drops out of the
     * window: continue the numbering of the previous list instead. Only for
     * playlists with discontinuities, the numbers of the others stay 0. */
    int renumber = pls->has_discontinuity && !sequence_seen;

    if (!pls->n_segments || !prev_n_segments)
        return;

    if (overlap >= 0 && overlap < prev_n_segments) {
        pi = overlap;
        ni = 0;
    } else if (overlap < 0 && -overlap < pls->n_segments) {
        /* the media sequence went back (e.g. a lagging CDN edge), the
         * lists still share segments */
        pi = 0;
        ni = -overlap;
    } else if (overlap < 0 && pls->has_discontinuity) {
        /* the media sequence went back out of the previous window: the same
         * content renumbered if a segment URL of the previous list is still
         * there, else a restarted sequence */
        for (j = 0; pi < 0 && j < pls->n_segments; j++)
            for (i = 0; i < prev_n_segments; i++)
                if (pls->segments[j]->url_offset == prev_segments[i]->url_offset &&
                    !strcmp(pls->segments[j]->url, prev_segments[i]->url)) {
                    pi = i;
                    ni = j;
                    break;
                }
    }

    if (pi < 0) {
        /* No segment in common: a window after a long time without reload
         * or a restarted media sequence. A new local region, so that no
         * stale region number or offset matches, and no anchor: the time
         * in between is unknown. */
        if (renumber)
            delta = av_sat_sub64(av_sat_add64(prev_segments[prev_n_segments - 1]->discontinuity_sequence, 1),
                                 pls->segments[0]->discontinuity_sequence);
        if (pls->timeline_time != AV_NOPTS_VALUE)
            av_log(c->ctx, AV_LOG_DEBUG, "Timeline anchor of playlist %d dropped, the reloaded "
                   "list shares no segment with the previous one\n", pls->index);
        pls->timeline_time = AV_NOPTS_VALUE;
    } else {
        if (renumber)
            delta = av_sat_sub64(prev_segments[pi]->discontinuity_sequence,
                                 pls->segments[ni]->discontinuity_sequence);

        /* Keep the anchor on its segment (renumbered if the match was by
         * URL) and move it forward to the new first segment only when its
         * segment dropped out of the window, by the durations in between. */
        if (pls->timeline_time != AV_NOPTS_VALUE) {
            int64_t shift = (prev_start_seq_no + pi) - (pls->start_seq_no + ni);
            int64_t from  = av_clip64(pls->timeline_seq_no - prev_start_seq_no, 0, prev_n_segments);
            int64_t time  = pls->timeline_time;

            pls->timeline_seq_no -= shift;
            if (pls->timeline_seq_no < pls->start_seq_no) {
                for (i = from; i < pi - ni; i++)
                    time = av_sat_add64(time, prev_segments[i]->duration);
                av_log(c->ctx, AV_LOG_DEBUG, "Timeline anchor of playlist %d moved from segment %"PRId64
                       " (%"PRId64") to %"PRId64" (%"PRId64")\n", pls->index, pls->timeline_seq_no,
                       pls->timeline_time, pls->start_seq_no, time);
                pls->timeline_seq_no = pls->start_seq_no;
                pls->timeline_time   = time;
            }
        }
    }

    for (i = 0; delta && i < pls->n_segments; i++)
        pls->segments[i]->discontinuity_sequence =
            av_sat_add64(pls->segments[i]->discontinuity_sequence, delta);
}

static int parse_playlist(HLSContext *c, const char *url,
                          struct playlist *pls, AVIOContext *in)
{
    int ret = 0, is_segment = 0, is_variant = 0;
    int64_t duration = 0;
    enum KeyType key_type = KEY_NONE;
    uint8_t iv[16] = "";
    int has_iv = 0;
    char key[MAX_URL_SIZE] = "";
    char line[MAX_URL_SIZE];
    const char *ptr;
    int close_in = 0;
    int64_t seg_offset = 0;
    int64_t seg_size = -1;
    uint8_t *new_url = NULL;
    struct variant_info variant_info;
    char tmp_str[MAX_URL_SIZE];
    struct segment *cur_init_section = NULL;
    int is_http = av_strstart(url, "http", NULL);
    struct segment **prev_segments = NULL;
    int prev_n_segments = 0;
    int64_t prev_start_seq_no = -1;
    int64_t discontinuity_sequence = 0;
    int discontinuity = 0;
    int discontinuity_sequence_seen = 0;
    int segment_seen = 0;
    /* The KEY tags since the last segment are one set, one tag per DRM
     * system. What the CENC tags of the set carry for the key requests: */
    int key_tags_open = 0, cenc_in_set = 0;
    char *cenc_pssh = NULL;
    char cenc_kid[33] = "";

    if (is_http && !in && c->http_persistent && c->playlist_pb) {
        in = c->playlist_pb;
        ret = open_url_keepalive(c->ctx, &c->playlist_pb, url, NULL);
        if (ret == AVERROR_EXIT) {
            return ret;
        } else if (ret < 0) {
            if (ret != AVERROR_EOF)
                av_log(c->ctx, AV_LOG_WARNING,
                    "keepalive request failed for '%s' with error: '%s' when parsing playlist\n",
                    url, av_err2str(ret));
            in = NULL;
        }
    }

    if (!in) {
        AVDictionary *opts = NULL;
        av_dict_copy(&opts, c->avio_opts, 0);

        if (c->http_persistent)
            av_dict_set(&opts, "multiple_requests", "1", 0);

        // don't use byterange when loading playlist - some servers don't like that and return HTTP 416
        av_dict_set(&opts, "seekable", "0", 0);

        ret = open_url(c->ctx, &in, url, &opts, NULL, NULL);
        av_dict_free(&opts);
        if (ret < 0)
            return ret;

        if (is_http && c->http_persistent)
            c->playlist_pb = in;
        else
            close_in = 1;
    }

    if (av_opt_get(in, "location", AV_OPT_SEARCH_CHILDREN, &new_url) >= 0)
        url = new_url;

    ff_get_chomp_line(in, line, sizeof(line));
    if (strcmp(line, "#EXTM3U")) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    if (pls) {
        prev_start_seq_no = pls->start_seq_no;
        prev_segments = pls->segments;
        prev_n_segments = pls->n_segments;
        pls->segments = NULL;
        pls->n_segments = 0;

        pls->finished = 0;
        pls->type = PLS_TYPE_UNSPECIFIED;
        /* a reloaded playlist may serve the key again */
        pls->key_fail_count = 0;
    }
    while (!avio_feof(in)) {
        ff_get_chomp_line(in, line, sizeof(line));
        if (av_strstart(line, "#EXT-X-STREAM-INF:", &ptr)) {
            if (pls) {
                av_log(c->ctx, AV_LOG_ERROR,
                       "Master Playlist tag found in a Media Playlist\n");
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            is_variant = 1;
            memset(&variant_info, 0, sizeof(variant_info));
            ff_parse_key_value(ptr, handle_variant_args, &variant_info);
        } else if (av_strstart(line, "#EXT-X-KEY:", &ptr)) {
            struct key_info info = {{0}};
            AVUUID system_id = { 0 };
            int has_system, is_cenc;
            ff_parse_key_value(ptr, handle_key_args, &info);
            if (!key_tags_open) {
                av_freep(&cenc_pssh);
                cenc_kid[0] = 0;
                cenc_in_set = 0;
                key_tags_open = 1;
            }
            has_system = cenc_keyformat_system_id(info.keyformat, system_id);
            /* SAMPLE-AES with a Widevine or PlayReady KEYFORMAT is cbcs of
             * fMP4 segments, which the mov demuxer decrypts as well */
            is_cenc = !strcmp(info.method, "SAMPLE-AES-CTR") ||
                      !strcmp(info.method, "ISO-23001-7") ||
                      (!strcmp(info.method, "SAMPLE-AES") && has_system);
            /* A tag of another DRM system next to a CENC tag of the same
             * segments (FairPlay with skd://) does not replace it. */
            if (!is_cenc && cenc_in_set && info.keyformat[0] &&
                strcmp(info.keyformat, "identity"))
                continue;
            if (!strcmp(info.method, "NONE")) {
                av_freep(&cenc_pssh);
                cenc_kid[0] = 0;
                cenc_in_set = 0;
            }
            key_type = KEY_NONE;
            has_iv = 0;
            if (!strcmp(info.method, "AES-128"))
                key_type = KEY_AES_128;
            if (!strcmp(info.method, "SAMPLE-AES") && !is_cenc)
                key_type = KEY_SAMPLE_AES;
            if (is_cenc) {
                /* ISO Common Encryption (cenc) of fMP4 segments: the KEY
                 * tag names a DRM system, there is no key to load here. The
                 * mov demuxer of the segments decrypts them with the key
                 * passed in seg_format_options, or requests it with
                 * cenc_decryption_key_url, which gets the pssh boxes and
                 * the KEYID of the tags. */
                cenc_in_set = 1;
                if (c->cenc_decryption_key_url && *c->cenc_decryption_key_url) {
                    ret = cenc_key_tag_add(&info, system_id, &cenc_pssh, cenc_kid);
                    if (ret < 0)
                        goto fail;
                }
                if (!c->cenc_logged) {
                    int has_key = av_dict_get(c->seg_format_opts, "decryption_key", NULL, 0) ||
                                  av_dict_get(c->seg_format_opts, "decryption_keys", NULL, 0) ||
                                  (c->cenc_decryption_key_url && *c->cenc_decryption_key_url);
                    av_log(c->ctx, has_key ? AV_LOG_VERBOSE : AV_LOG_WARNING,
                           "Segments are encrypted with %s (CENC)%s\n", info.method,
                           has_key ? "" : ", no decryption_key or decryption_keys in seg_format_options");
                    c->cenc_logged = 1;
                }
            }
            if (!av_strncasecmp(info.iv, "0x", 2)) {
                ff_hex_to_data(iv, info.iv + 2);
                has_iv = 1;
            }
            av_strlcpy(key, info.uri, sizeof(key));
        } else if (av_strstart(line, "#EXT-X-MEDIA:", &ptr)) {
            struct rendition_info info = {{0}};
            if (pls) {
                av_log(c->ctx, AV_LOG_ERROR,
                       "Master Playlist tag found in a Media Playlist\n");
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            ff_parse_key_value(ptr, handle_rendition_args, &info);
            new_rendition(c, &info, url);
        } else if (av_strstart(line, "#EXT-X-TARGETDURATION:", &ptr)) {
            int64_t t;
            ret = ensure_playlist(c, &pls, url);
            if (ret < 0)
                goto fail;
            t = strtoll(ptr, NULL, 10);
            if (t < 0 || t >= INT64_MAX / AV_TIME_BASE) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            pls->target_duration = t * AV_TIME_BASE;
        } else if (av_strstart(line, "#EXT-X-MEDIA-SEQUENCE:", &ptr)) {
            uint64_t seq_no;
            ret = ensure_playlist(c, &pls, url);
            if (ret < 0)
                goto fail;
            seq_no = strtoull(ptr, NULL, 10);
            if (seq_no > INT64_MAX/2) {
                av_log(c->ctx, AV_LOG_DEBUG, "MEDIA-SEQUENCE higher than "
                        "INT64_MAX/2, mask out the highest bit\n");
                seq_no &= INT64_MAX/2;
            }
            pls->start_seq_no = seq_no;
        } else if (av_strstart(line, "#EXT-X-DISCONTINUITY-SEQUENCE:", &ptr)) {
            char *end;
            uint64_t seq_no;

            ret = ensure_playlist(c, &pls, url);
            if (ret < 0)
                goto fail;
            seq_no = strtoull(ptr, &end, 10);
            if (segment_seen || discontinuity || discontinuity_sequence_seen ||
                end == ptr || *end || seq_no > INT64_MAX) {
                av_log(c->ctx, AV_LOG_WARNING,
                       "Ignoring misplaced or invalid #EXT-X-DISCONTINUITY-SEQUENCE:%s\n", ptr);
                continue;
            }
            discontinuity_sequence = seq_no;
            discontinuity_sequence_seen = 1;
        } else if (av_strstart(line, "#EXT-X-PLAYLIST-TYPE:", &ptr)) {
            ret = ensure_playlist(c, &pls, url);
            if (ret < 0)
                goto fail;
            if (!strcmp(ptr, "EVENT"))
                pls->type = PLS_TYPE_EVENT;
            else if (!strcmp(ptr, "VOD"))
                pls->type = PLS_TYPE_VOD;
        } else if (av_strstart(line, "#EXT-X-CENC-DECRYPTION-KEY-URL:", &ptr)) {
            /* takes priority over the cenc_decryption_key_url option */
            if (*ptr) {
                size_t base_len = strlen(url);
                size_t val_len = strlen(ptr);
                char *key_url;

                if (base_len >= INT_MAX || val_len >= INT_MAX - base_len - 1) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                key_url = av_malloc(base_len + val_len + 2);
                if (!key_url) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                ret = ff_make_absolute_url(key_url, base_len + val_len + 2, url, ptr);
                if (ret < 0) {
                    av_log(c->ctx, AV_LOG_ERROR, "Invalid playlist key URL '%s'\n", ptr);
                    av_free(key_url);
                    goto fail;
                }
                av_free(c->cenc_decryption_key_url);
                c->cenc_decryption_key_url = key_url;
            }
        } else if (av_strstart(line, "#EXT-X-MAP:", &ptr)) {
            struct init_section_info info = {{0}};
            ret = ensure_playlist(c, &pls, url);
            if (ret < 0)
                goto fail;
            ff_parse_key_value(ptr, handle_init_section_args, &info);
            cur_init_section = new_init_section(pls, &info, url);
            if (!cur_init_section) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            cur_init_section->key_type = key_type;
            if (has_iv) {
                memcpy(cur_init_section->iv, iv, sizeof(iv));
            } else {
                int64_t seq = pls->start_seq_no + pls->n_segments;
                memset(cur_init_section->iv, 0, sizeof(cur_init_section->iv));
                AV_WB64(cur_init_section->iv + 8, seq);
            }

            if (key_type != KEY_NONE) {
                ff_make_absolute_url(tmp_str, sizeof(tmp_str), url, key);
                if (!tmp_str[0]) {
                    av_free(cur_init_section);
                    ret = AVERROR_INVALIDDATA;
                    goto fail;
                }
                cur_init_section->key = av_strdup(tmp_str);
                if (!cur_init_section->key) {
                    av_free(cur_init_section);
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
            } else {
                cur_init_section->key = NULL;
            }

        } else if (av_strstart(line, "#EXT-X-START:", &ptr)) {
            const char *time_offset_value = NULL;
            ret = ensure_playlist(c, &pls, url);
            if (ret < 0) {
                goto fail;
            }
            if (av_strstart(ptr, "TIME-OFFSET=", &time_offset_value)) {
                double offset = strtod(time_offset_value, NULL) * AV_TIME_BASE;
                if (offset >= -0x1p63 && offset < 0x1p63) {
                    pls->start_time_offset = offset;
                    pls->time_offset_flag = 1;
                } else {
                    av_log(c->ctx, AV_LOG_WARNING, "TIME-OFFSET value is"
                                                    "invalid, it will be ignored");
                }
            } else {
                av_log(c->ctx, AV_LOG_WARNING, "#EXT-X-START value is"
                                                "invalid, it will be ignored");
                continue;
            }
        } else if (!strcmp(line, "#EXT-X-DISCONTINUITY")) {
            ret = ensure_playlist(c, &pls, url);
            if (ret < 0)
                goto fail;
            if (discontinuity_sequence == INT64_MAX) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            discontinuity_sequence++;
            discontinuity = 1;
            pls->has_discontinuity = 1;
        } else if (av_strstart(line, "#EXT-X-ENDLIST", &ptr)) {
            if (pls)
                pls->finished = 1;
        } else if (av_strstart(line, "#EXTINF:", &ptr)) {
            double d = atof(ptr) * AV_TIME_BASE;
            if (d < 0 || d > INT64_MAX || isnan(d)) {
                av_log(c->ctx, AV_LOG_WARNING, "EXTINF %f unsupported\n", d / AV_TIME_BASE);
                d = 0;
            }
            duration = d;
            is_segment = 1;
        } else if (av_strstart(line, "#EXT-X-BYTERANGE:", &ptr)) {
            seg_size = strtoll(ptr, NULL, 10);
            ptr = strchr(ptr, '@');
            if (ptr)
                seg_offset = strtoll(ptr+1, NULL, 10);
            if (seg_size < 0 || seg_offset > INT64_MAX - seg_size) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
        } else if (av_strstart(line, "#", NULL)) {
            av_log(c->ctx, AV_LOG_VERBOSE, "Skip ('%s')\n", line);
            continue;
        } else if (line[0]) {
            if (is_variant) {
                if (!new_variant(c, &variant_info, line, url)) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                is_variant = 0;
            }
            if (is_segment) {
                struct segment *seg;
                int cenc_tags = -1;
                ret = ensure_playlist(c, &pls, url);
                if (ret < 0)
                    goto fail;
                key_tags_open = 0;
                if (cenc_pssh || cenc_kid[0]) {
                    cenc_tags = cenc_key_tags_index(pls, cenc_pssh, cenc_kid);
                    if (cenc_tags < 0) {
                        ret = cenc_tags;
                        goto fail;
                    }
                }
                seg = av_malloc(sizeof(struct segment));
                if (!seg) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                seg->cenc_key_tags = cenc_tags;
                if (has_iv) {
                    memcpy(seg->iv, iv, sizeof(iv));
                } else {
                    uint64_t seq = pls->start_seq_no + (uint64_t)pls->n_segments;
                    memset(seg->iv, 0, sizeof(seg->iv));
                    AV_WB64(seg->iv + 8, seq);
                }

                if (key_type != KEY_NONE) {
                    ff_make_absolute_url(tmp_str, sizeof(tmp_str), url, key);
                    if (!tmp_str[0]) {
                        ret = AVERROR_INVALIDDATA;
                        av_free(seg);
                        goto fail;
                    }
                    seg->key = av_strdup(tmp_str);
                    if (!seg->key) {
                        av_free(seg);
                        ret = AVERROR(ENOMEM);
                        goto fail;
                    }
                } else {
                    seg->key = NULL;
                }

                ff_make_absolute_url(tmp_str, sizeof(tmp_str), url, line);
                if (!tmp_str[0]) {
                    ret = AVERROR_INVALIDDATA;
                    if (seg->key)
                        av_free(seg->key);
                    av_free(seg);
                    goto fail;
                }
                seg->url = av_strdup(tmp_str);
                if (!seg->url) {
                    av_free(seg->key);
                    av_free(seg);
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }

                ret = test_segment(c->ctx, pls->ctx ? pls->ctx->iformat : NULL, pls, seg);
                if (ret < 0) {
                    av_free(seg->url);
                    av_free(seg->key);
                    av_free(seg);
                    goto fail;
                }

                if (duration < 0.001 * AV_TIME_BASE) {
                    av_log(c->ctx, AV_LOG_WARNING, "Cannot get correct #EXTINF value of segment %s,"
                                    " set to default value to 1ms.\n", seg->url);
                    duration = 0.001 * AV_TIME_BASE;
                }
                seg->duration = duration;
                seg->discontinuity_sequence = discontinuity_sequence;
                seg->discontinuity = discontinuity;
                seg->key_type = key_type;
                dynarray_add(&pls->segments, &pls->n_segments, seg);
                is_segment = 0;
                discontinuity = 0;
                segment_seen = 1;

                seg->size = seg_size;
                if (seg_size >= 0) {
                    seg->url_offset = seg_offset;
                    seg_offset += seg_size;
                    seg_size = -1;
                } else {
                    seg->url_offset = 0;
                    seg_offset = 0;
                }

                seg->init_section = cur_init_section;
            }
        }
    }
    if (prev_segments) {
        if (pls->is_subtitle && pls->subtitle_gate_origin != AV_NOPTS_VALUE) {
            /* the window moved on: its first segment starts later */
            int64_t diff = pls->start_seq_no - prev_start_seq_no;
            if (diff < 0 || diff > prev_n_segments)
                pls->subtitle_gate_origin = AV_NOPTS_VALUE;
            else
                for (int64_t k = 0; k < diff; k++)
                    pls->subtitle_gate_origin = av_sat_add64(pls->subtitle_gate_origin,
                                                             prev_segments[k]->duration);
        }
        if (pls->start_seq_no > prev_start_seq_no && c->first_timestamp != AV_NOPTS_VALUE &&
            c->first_timestamp_pls == pls) {
            int64_t prev_timestamp = c->first_timestamp;
            int i;
            int64_t diff = pls->start_seq_no - prev_start_seq_no;
            for (i = 0; i < prev_n_segments && i < diff; i++) {
                c->first_timestamp += prev_segments[i]->duration;
            }
            av_log(c->ctx, AV_LOG_DEBUG, "Media sequence change (%"PRId64" -> %"PRId64")"
                   " reflected in first_timestamp: %"PRId64" -> %"PRId64"\n",
                   prev_start_seq_no, pls->start_seq_no,
                   prev_timestamp, c->first_timestamp);
        } else if (pls->start_seq_no < prev_start_seq_no) {
            av_log(c->ctx, AV_LOG_WARNING, "Media sequence changed unexpectedly: %"PRId64" -> %"PRId64"\n",
                   prev_start_seq_no, pls->start_seq_no);
        }
        reload_discontinuity_state(c, pls, prev_segments, prev_n_segments,
                                   prev_start_seq_no, discontinuity_sequence_seen);
        free_segment_dynarray(prev_segments, prev_n_segments);
        av_freep(&prev_segments);
    }
    if (pls) {
        pls->discontinuity_sequence_seen = discontinuity_sequence_seen;
        pls->last_load_time = av_gettime_relative();
    }

fail:
    av_free(new_url);
    av_free(cenc_pssh);
    if (close_in)
        ff_format_io_close(c->ctx, &in);
    c->ctx->ctx_flags = c->ctx->ctx_flags & ~(unsigned)AVFMTCTX_UNSEEKABLE;
    if (!c->n_variants || !c->variants[c->selected_variant]->n_playlists ||
        !(c->variants[c->selected_variant]->playlists[0]->finished ||
          c->variants[c->selected_variant]->playlists[0]->type == PLS_TYPE_EVENT))
        c->ctx->ctx_flags |= AVFMTCTX_UNSEEKABLE;

    if (c->n_variants && c->variants[c->selected_variant]->n_playlists &&
        c->variants[c->selected_variant]->playlists[0]->type == PLS_TYPE_EVENT &&
        !c->variants[c->selected_variant]->playlists[0]->finished) {
        struct playlist *p = c->variants[c->selected_variant]->playlists[0];
        int64_t duration = 0;
        for (int i = 0; i < p->n_segments; i++)
            duration += p->segments[i]->duration;
        c->ctx->duration = duration;
    }

    return ret;
}

static struct segment *segment_at(struct playlist *pls, int64_t seq_no)
{
    int64_t n = seq_no - pls->start_seq_no;
    if (n < 0 || n >= pls->n_segments)
        return NULL;
    return pls->segments[n];
}

static struct segment *current_segment(struct playlist *pls)
{
    return segment_at(pls, pls->cur_seq_no);
}

/* The offset of region sequence of pls: shared between the playlists only
 * when the numbers come from #EXT-X-DISCONTINUITY-SEQUENCE. */
static int64_t disc_offset_lookup(HLSContext *c, const struct playlist *pls,
                                  int64_t sequence)
{
    const struct playlist *owner = pls->discontinuity_sequence_seen ? NULL : pls;

    for (int i = 0; i < c->n_disc_offsets; i++)
        if (c->disc_offsets[i].sequence == sequence &&
            c->disc_offsets[i].owner == owner)
            return c->disc_offsets[i].offset;
    return AV_NOPTS_VALUE;
}

/* The first offset stored for a region stays. */
static void disc_offset_store(HLSContext *c, const struct playlist *pls,
                              int64_t sequence, int64_t offset)
{
    int i;

    if (offset == AV_NOPTS_VALUE || pls->is_subtitle ||
        disc_offset_lookup(c, pls, sequence) != AV_NOPTS_VALUE)
        return;
    i = c->disc_offsets_next;
    c->disc_offsets_next = (i + 1) % HLS_DISC_OFFSETS;
    c->n_disc_offsets = FFMIN(c->n_disc_offsets + 1, HLS_DISC_OFFSETS);
    c->disc_offsets[i].sequence = sequence;
    c->disc_offsets[i].offset   = offset;
    c->disc_offsets[i].owner    = pls->discontinuity_sequence_seen ? NULL : pls;
}

/* The segment demuxer of pls (re)starts on cur_seq_no (start, seek, switch
 * in, new subtitle region): take over the region of that segment. With
 * lookup, a playlist with discontinuities keeps its offset if the region
 * stays the same and else takes the stored one (AV_NOPTS_VALUE: computed
 * from the first packet); without lookup (start of the demuxer) the offset
 * stays as it is, 0 at the start. */
static void discontinuity_select(HLSContext *c, struct playlist *pls, int lookup)
{
    struct segment *seg = current_segment(pls);

    pls->discontinuity_pending      = 0;
    pls->discontinuity_start_seq_no = pls->cur_seq_no;
    pls->discontinuity_region_start = 1;
    if (!seg)
        return;
    if (lookup && pls->has_discontinuity &&
        (seg->discontinuity_sequence != pls->active_discontinuity_sequence ||
         pls->discontinuity_offset == AV_NOPTS_VALUE)) {
        disc_offset_store(c, pls, pls->active_discontinuity_sequence,
                          pls->discontinuity_offset);
        pls->discontinuity_offset = disc_offset_lookup(c, pls, seg->discontinuity_sequence);
    }
    pls->active_discontinuity_sequence = seg->discontinuity_sequence;
}

/* Does seg start a new discontinuity region for the segment demuxer? Never
 * for a playlist without #EXT-X-DISCONTINUITY, its region is only tracked
 * (a tag showing up later then compares against the right number). */
static int discontinuity_changed(struct playlist *pls, const struct segment *seg)
{
    if (!pls->has_discontinuity) {
        pls->active_discontinuity_sequence = seg->discontinuity_sequence;
        return 0;
    }
    return seg->discontinuity_sequence != pls->active_discontinuity_sequence;
}

static struct segment *next_segment(struct playlist *pls)
{
    int64_t n = pls->cur_seq_no - pls->start_seq_no + 1;
    if (n >= pls->n_segments)
        return NULL;
    return pls->segments[n];
}

/* True if 'next' is an unencrypted byte range that directly follows 'cur' in
 * the same resource and ends within the range requested up to 'end', so that
 * it is read on from the input of 'cur' (i.e. consecutive EXT-X-BYTERANGE
 * segments). A segment with other CENC key tags is opened on its own, so
 * that cenc_key_tags_apply() passes its tags before it is read. */
static int segment_continues(const struct segment *cur,
                             const struct segment *next, int64_t end)
{
    return cur && next &&
           cur->size >= 0 && next->size >= 0 &&
           next->url_offset == cur->url_offset + cur->size &&
           next->url_offset + next->size <= end &&
           cur->key_type == KEY_NONE && next->key_type == KEY_NONE &&
           next->init_section == cur->init_section &&
           next->cenc_key_tags == cur->cenc_key_tags &&
           !next->discontinuity &&
           !strcmp(next->url, cur->url);
}

static int64_t range_end(struct playlist *pls, const struct segment *seg)
{
    int64_t n = pls->cur_seq_no - pls->start_seq_no + 1;

    if (seg == next_segment(pls))
        n++;
    else if (seg != current_segment(pls))
        return seg->url_offset + seg->size;
    for (; n < pls->n_segments &&
           segment_continues(seg, pls->segments[n], INT64_MAX); n++)
        seg = pls->segments[n];
    return seg->url_offset + seg->size;
}

static int read_from_input(struct playlist *pls, struct segment *seg,
                           uint8_t *buf, int buf_size)
{
    int ret;

     /* limit read if the segment was only a part of a file */
    if (seg->size >= 0)
        buf_size = FFMIN(buf_size, seg->size - pls->cur_seg_offset);

    ret = avio_read(pls->input, buf, buf_size);
    if (ret > 0)
        pls->cur_seg_offset += ret;

    return ret;
}

static int read_from_url(struct playlist *pls, struct segment *seg,
                         uint8_t *buf, int buf_size)
{
    /* what skip_image_head() read of the segment comes first */
    if (pls->head_pos < pls->head_len) {
        int len = FFMIN(buf_size, pls->head_len - pls->head_pos);
        memcpy(buf, pls->head_buf + pls->head_pos, len);
        pls->head_pos += len;
        return len;
    }

    return read_from_input(pls, seg, buf, buf_size);
}

static void drop_segment_head(struct playlist *pls)
{
    pls->head_len   = 0;
    pls->head_pos   = 0;
    pls->head_check = 0;
}

/*
 * Some CDNs serve MPEG-TS segments disguised as pictures: a small PNG, JPEG,
 * GIF or WebP image in front of the TS packets of every segment, served from
 * an image host. Probed, the first segment is taken for the picture
 * (png_pipe, webp_pipe, ...); inside the stream, the picture is garbage the
 * mpegts demuxer resyncs over, and may take for packets.
 *
 * A segment that starts with one of these signatures and holds a run of
 * IMAGE_HEAD_PACKETS TS packets that starts within its first
 * IMAGE_HEAD_SEARCH bytes is read from the first packet of that run.
 * Anything else, plain TS, fMP4, packed audio, an image without TS packets
 * behind it, is passed on unchanged; only the first 12 bytes are looked at
 * then. The check runs on what the segment demuxer gets, the decrypted
 * data of an AES-128 segment.
 *
 * Detection and limits as in hlsdl 0.34 (strip_disguise_prefix(), commit
 * 12ba4d1 of Masta2002/e2iplayer-deps), which does the same on the data it
 * writes.
 */
#define IMAGE_HEAD_SIGNATURE 12
#define IMAGE_HEAD_SEARCH    (64 * 1024)
#define IMAGE_HEAD_TS_PACKET 188
/* more packets in a row than a probe needs: this cuts data off, and image
 * data may hold 0x47 bytes 188 apart by chance */
#define IMAGE_HEAD_PACKETS   5

static const char *image_head_type(const uint8_t *d)
{
    if (!memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return "PNG";
    if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return "JPEG";
    if (!memcmp(d, "GIF8", 4))
        return "GIF";
    if (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return "WebP";
    return NULL;
}

static int grow_segment_head(struct playlist *pls, int size)
{
    uint8_t *buf = av_fast_realloc(pls->head_buf, &pls->head_buf_size, size);
    if (!buf)
        return AVERROR(ENOMEM);
    pls->head_buf = buf;
    return 0;
}

static int skip_image_head(struct playlist *pls, struct segment *seg)
{
    const int size = IMAGE_HEAD_SEARCH + IMAGE_HEAD_PACKETS * IMAGE_HEAD_TS_PACKET;
    const char *type = NULL;
    int len = 0, ret = 0, start, k;

    drop_segment_head(pls);

    if ((ret = grow_segment_head(pls, IMAGE_HEAD_SIGNATURE)) < 0)
        return ret;
    while (len < IMAGE_HEAD_SIGNATURE &&
           (ret = read_from_input(pls, seg, pls->head_buf + len, IMAGE_HEAD_SIGNATURE - len)) > 0)
        len += ret;
    pls->head_len = len;
    if (len < IMAGE_HEAD_SIGNATURE || !(type = image_head_type(pls->head_buf)))
        return 0;

    if ((ret = grow_segment_head(pls, size)) < 0)
        return ret;
    while (len < size &&
           (ret = read_from_input(pls, seg, pls->head_buf + len, size - len)) > 0)
        len += ret;
    pls->head_len = len;

    for (start = 1; start + (IMAGE_HEAD_PACKETS - 1) * IMAGE_HEAD_TS_PACKET < len; start++) {
        for (k = 0; k < IMAGE_HEAD_PACKETS; k++)
            if (pls->head_buf[start + k * IMAGE_HEAD_TS_PACKET] != 0x47)
                break;
        if (k == IMAGE_HEAD_PACKETS)
            break;
    }
    if (start + (IMAGE_HEAD_PACKETS - 1) * IMAGE_HEAD_TS_PACKET >= len) {
        av_log(pls->parent, AV_LOG_VERBOSE,
               "Segment %s starts with a %s image without MPEG-TS packets behind it\n",
               seg->url, type);
        return 0;
    }

    pls->head_pos = start;
    av_log(pls->parent, pls->head_logged ? AV_LOG_DEBUG : AV_LOG_INFO,
           "Skipped a %s image of %d bytes in front of the MPEG-TS data of segment %s\n",
           type, start, seg->url);
    pls->head_logged = 1;
    return 0;
}

/* Parse the raw ID3 data and pass contents to caller */
static void parse_id3(AVFormatContext *s, AVIOContext *pb,
                      AVDictionary **metadata, int64_t *dts, HLSAudioSetupInfo *audio_setup_info,
                      ID3v2ExtraMetaAPIC **apic, ID3v2ExtraMeta **extra_meta)
{
    static const char id3_priv_owner_ts[] = "com.apple.streaming.transportStreamTimestamp";
    static const char id3_priv_owner_audio_setup[] = "com.apple.streaming.audioDescription";
    ID3v2ExtraMeta *meta;

    ff_id3v2_read_dict(pb, metadata, ID3v2_DEFAULT_MAGIC, extra_meta);
    for (meta = *extra_meta; meta; meta = meta->next) {
        if (!strcmp(meta->tag, "PRIV")) {
            ID3v2ExtraMetaPRIV *priv = &meta->data.priv;
            if (priv->datasize == 8 && !av_strncasecmp(priv->owner, id3_priv_owner_ts, 44)) {
                /* 33-bit MPEG timestamp */
                int64_t ts = AV_RB64(priv->data);
                av_log(s, AV_LOG_DEBUG, "HLS ID3 audio timestamp %"PRId64"\n", ts);
                if ((ts & ~((1ULL << 33) - 1)) == 0)
                    *dts = ts;
                else
                    av_log(s, AV_LOG_ERROR, "Invalid HLS ID3 audio timestamp %"PRId64"\n", ts);
            } else if (priv->datasize >= 8 && !av_strncasecmp(priv->owner, id3_priv_owner_audio_setup, 36)) {
                ff_hls_senc_read_audio_setup_info(audio_setup_info, priv->data, priv->datasize);
            }
        } else if (!strcmp(meta->tag, "APIC") && apic)
            *apic = &meta->data.apic;
    }
}

/* Check if the ID3 metadata contents have changed */
static int id3_has_changed_values(struct playlist *pls, AVDictionary *metadata,
                                  ID3v2ExtraMetaAPIC *apic)
{
    const AVDictionaryEntry *entry = NULL;
    const AVDictionaryEntry *oldentry;
    /* check that no keys have changed values */
    while ((entry = av_dict_iterate(metadata, entry))) {
        oldentry = av_dict_get(pls->id3_initial, entry->key, NULL, AV_DICT_MATCH_CASE);
        if (!oldentry || strcmp(oldentry->value, entry->value) != 0)
            return 1;
    }

    /* check if apic appeared */
    if (apic && (pls->ctx->nb_streams != 2 || !pls->ctx->streams[1]->attached_pic.data))
        return 1;

    if (apic) {
        int size = pls->ctx->streams[1]->attached_pic.size;
        if (size != apic->buf->size - AV_INPUT_BUFFER_PADDING_SIZE)
            return 1;

        if (memcmp(apic->buf->data, pls->ctx->streams[1]->attached_pic.data, size) != 0)
            return 1;
    }

    return 0;
}

/* Parse ID3 data and handle the found data */
static void handle_id3(AVIOContext *pb, struct playlist *pls)
{
    AVDictionary *metadata = NULL;
    ID3v2ExtraMetaAPIC *apic = NULL;
    ID3v2ExtraMeta *extra_meta = NULL;
    int64_t timestamp = AV_NOPTS_VALUE;

    parse_id3(pls->ctx, pb, &metadata, &timestamp, &pls->audio_setup_info, &apic, &extra_meta);

    if (timestamp != AV_NOPTS_VALUE) {
        pls->id3_mpegts_timestamp = timestamp;
        pls->id3_offset = 0;
    }

    if (!pls->id3_found) {
        /* initial ID3 tags */
        av_assert0(!pls->id3_deferred_extra);
        pls->id3_found = 1;

        /* get picture attachment and set text metadata */
        if (pls->ctx->nb_streams)
            ff_id3v2_parse_apic(pls->ctx, extra_meta);
        else
            /* demuxer not yet opened, defer picture attachment */
            pls->id3_deferred_extra = extra_meta;

        ff_id3v2_parse_priv_dict(&metadata, extra_meta);
        av_dict_copy(&pls->ctx->metadata, metadata, 0);
        pls->id3_initial = metadata;

    } else {
        if (!pls->id3_changed && id3_has_changed_values(pls, metadata, apic)) {
            avpriv_report_missing_feature(pls->parent, "Changing ID3 metadata in HLS audio elementary stream");
            pls->id3_changed = 1;
        }
        av_dict_free(&metadata);
    }

    if (!pls->id3_deferred_extra)
        ff_id3v2_free_extra_meta(&extra_meta);
}

static void intercept_id3(struct playlist *pls, uint8_t *buf,
                         int buf_size, int *len)
{
    /* intercept id3 tags, we do not want to pass them to the raw
     * demuxer on all segment switches */
    int bytes;
    int id3_buf_pos = 0;
    int fill_buf = 0;
    struct segment *seg = current_segment(pls);

    /* gather all the id3 tags */
    while (1) {
        /* see if we can retrieve enough data for ID3 header */
        if (*len < ID3v2_HEADER_SIZE && buf_size >= ID3v2_HEADER_SIZE) {
            bytes = read_from_url(pls, seg, buf + *len, ID3v2_HEADER_SIZE - *len);
            if (bytes > 0) {

                if (bytes == ID3v2_HEADER_SIZE - *len)
                    /* no EOF yet, so fill the caller buffer again after
                     * we have stripped the ID3 tags */
                    fill_buf = 1;

                *len += bytes;

            } else if (*len <= 0) {
                /* error/EOF */
                *len = bytes;
                fill_buf = 0;
            }
        }

        if (*len < ID3v2_HEADER_SIZE)
            break;

        if (ff_id3v2_match(buf, ID3v2_DEFAULT_MAGIC)) {
            int64_t maxsize = seg->size >= 0 ? seg->size : 1024*1024;
            int taglen = ff_id3v2_tag_len(buf);
            int tag_got_bytes = FFMIN(taglen, *len);
            int remaining = taglen - tag_got_bytes;

            if (taglen > maxsize) {
                av_log(pls->parent, AV_LOG_ERROR, "Too large HLS ID3 tag (%d > %"PRId64" bytes)\n",
                       taglen, maxsize);
                break;
            }

            /*
             * Copy the id3 tag to our temporary id3 buffer.
             * We could read a small id3 tag directly without memcpy, but
             * we would still need to copy the large tags, and handling
             * both of those cases together with the possibility for multiple
             * tags would make the handling a bit complex.
             */
            pls->id3_buf = av_fast_realloc(pls->id3_buf, &pls->id3_buf_size, id3_buf_pos + taglen);
            if (!pls->id3_buf)
                break;
            memcpy(pls->id3_buf + id3_buf_pos, buf, tag_got_bytes);
            id3_buf_pos += tag_got_bytes;

            /* strip the intercepted bytes */
            *len -= tag_got_bytes;
            memmove(buf, buf + tag_got_bytes, *len);
            av_log(pls->parent, AV_LOG_DEBUG, "Stripped %d HLS ID3 bytes\n", tag_got_bytes);

            if (remaining > 0) {
                /* read the rest of the tag in */
                if (read_from_url(pls, seg, pls->id3_buf + id3_buf_pos, remaining) != remaining)
                    break;
                id3_buf_pos += remaining;
                av_log(pls->parent, AV_LOG_DEBUG, "Stripped additional %d HLS ID3 bytes\n", remaining);
            }

        } else {
            /* no more ID3 tags */
            break;
        }
    }

    /* re-fill buffer for the caller unless EOF */
    if (*len >= 0 && (fill_buf || *len == 0)) {
        bytes = read_from_url(pls, seg, buf + *len, buf_size - *len);

        /* ignore error if we already had some data */
        if (bytes >= 0)
            *len += bytes;
        else if (*len == 0)
            *len = bytes;
    }

    if (pls->id3_buf) {
        /* Now parse all the ID3 tags */
        FFIOContext id3ioctx;
        ffio_init_read_context(&id3ioctx, pls->id3_buf, id3_buf_pos);
        handle_id3(&id3ioctx.pub, pls);
    }

    if (pls->is_id3_timestamped == -1)
        pls->is_id3_timestamped = (pls->id3_mpegts_timestamp != AV_NOPTS_VALUE);
}

/* An answer that will not change: a 4xx other than 429, a missing file, a
 * key of the wrong size. */
static int key_fail_is_final(int ret)
{
    return ret == AVERROR_HTTP_BAD_REQUEST || ret == AVERROR_HTTP_UNAUTHORIZED ||
           ret == AVERROR_HTTP_FORBIDDEN   || ret == AVERROR_HTTP_NOT_FOUND    ||
           ret == AVERROR_HTTP_OTHER_4XX   || ret == AVERROR(ENOENT)           ||
           ret == AVERROR_INVALIDDATA;
}

/* Whether the key URL of pls->key_fail_url is not requested again. An answer
 * that will not change (a 4xx other than 429, a missing file, a key of the
 * wrong size) counts at once, other errors (5xx, timeouts) after as many
 * failures in a row as one segment is tried, at least three. Without this
 * every try of every segment requests the dead key again: seg_max_retry + 1
 * requests per segment, a burst over the whole playlist. */
static int key_failed_for_good(HLSContext *c, struct playlist *pls)
{
    if (!pls->key_fail_count)
        return 0;
    if (key_fail_is_final(pls->key_fail_ret))
        return 1;
    return pls->key_fail_count >= FFMAX(c->seg_max_retry + 1, 3);
}

/* Whether the key of seg is not requested now: it failed for good, and after
 * an error that may pass the segment is before the next one to try. The
 * segments in between fail at once and are skipped, without a wait, so the
 * gap is counted in segments, not in time: 1 segment after the failure that
 * reached the limit, doubled after every further failure up to 64. */
static int key_request_skipped(HLSContext *c, struct playlist *pls, struct segment *seg)
{
    if (!pls->key_fail_count || strcmp(pls->key_fail_url, seg->key) ||
        !key_failed_for_good(c, pls))
        return 0;
    return key_fail_is_final(pls->key_fail_ret) ||
           pls->cur_seq_no < pls->key_retry_seq_no;
}

static int key_failed(HLSContext *c, struct playlist *pls, struct segment *seg, int ret)
{
    /* an interrupted request says nothing about the key */
    if (ret == AVERROR_EXIT)
        return ret;
    if (!pls->key_fail_count || strcmp(pls->key_fail_url, seg->key)) {
        av_strlcpy(pls->key_fail_url, seg->key, sizeof(pls->key_fail_url));
        pls->key_fail_count = 0;
    }
    pls->key_fail_count++;
    pls->key_fail_ret = ret;
    if (key_fail_is_final(ret)) {
        av_log(pls->parent, AV_LOG_WARNING,
               "Key file %s is not requested again until the playlist is reloaded\n",
               seg->key);
    } else if (key_failed_for_good(c, pls)) {
        int over = pls->key_fail_count - FFMAX(c->seg_max_retry + 1, 3);
        pls->key_retry_seq_no = pls->cur_seq_no + (INT64_C(1) << FFMIN(over, 6));
        av_log(pls->parent, AV_LOG_WARNING,
               "Key file %s is not requested again before segment %"PRId64"\n",
               seg->key, pls->key_retry_seq_no);
    }
    return ret;
}

static int read_key(HLSContext *c, struct playlist *pls, struct segment *seg)
{
    AVIOContext *pb = NULL;
    char *key_url = NULL;
    int ret;

    if (!strncmp(seg->key, "key://", 6)) {
        /* the key is given directly as hex string */
        uint8_t key[sizeof(pls->key)];
        if (strlen(seg->key + 6) != sizeof(key) * 2 ||
            ff_hex_to_data(key, seg->key + 6) != sizeof(key)) {
            av_log(pls->parent, AV_LOG_ERROR,
                   "Failed to parse key:// key - not %zu hex digits\n", sizeof(key) * 2);
            return AVERROR_INVALIDDATA;
        }
        memcpy(pls->key, key, sizeof(key));
        av_strlcpy(pls->key_url, seg->key, sizeof(pls->key_url));
        return 0;
    }

    if (!strncmp(seg->key, "keys://", 7)) {
        /* "keys://" contains multiple kid=key pairs, they are passed to the
         * mov demuxer as decryption_keys in hls_read_header() */
        av_strlcpy(pls->key_url, seg->key, sizeof(pls->key_url));
        return 0;
    }

    if (key_request_skipped(c, pls, seg))
        return pls->key_fail_ret;

    if (c->key_uri_replace_old && c->key_uri_replace_new &&
        c->key_uri_replace_old[0]) {
        key_url = av_strireplace(seg->key, c->key_uri_replace_old, c->key_uri_replace_new);
        if (!key_url)
            return AVERROR(ENOMEM);
    } else {
        key_url = seg->key;
    }

    ret = open_url(pls->parent, &pb, key_url, &c->avio_opts, NULL, NULL);
    if (key_url != seg->key)
        av_free(key_url);
    if (ret < 0) {
        av_log(pls->parent, AV_LOG_ERROR, "Unable to open key file %s, %s\n",
               seg->key, av_err2str(ret));
        return key_failed(c, pls, seg, ret);
    }

    ret = avio_read(pb, pls->key, sizeof(pls->key));
    ff_format_io_close(pls->parent, &pb);
    if (ret != sizeof(pls->key)) {
        if (ret < 0) {
            av_log(pls->parent, AV_LOG_ERROR, "Unable to read key file %s, %s\n",
                   seg->key, av_err2str(ret));
        } else {
            av_log(pls->parent, AV_LOG_ERROR, "Unable to read key file %s, read bytes %d != %zu\n",
                   seg->key, ret, sizeof(pls->key));
            ret = AVERROR_INVALIDDATA;
        }

        return key_failed(c, pls, seg, ret);
    }

    pls->key_fail_count = 0;
    av_strlcpy(pls->key_url, seg->key, sizeof(pls->key_url));

    return 0;
}

static int open_input(HLSContext *c, struct playlist *pls, struct segment *seg,
                      AVIOContext **in, int64_t *end)
{
    AVDictionary *opts = NULL;
    int ret;
    int is_http = 0;

    if (c->http_persistent)
        av_dict_set(&opts, "multiple_requests", "1", 0);

    *end = -1;
    if (seg->size >= 0) {
        /* Restrict the request to the wanted byte range, extended so that one
         * request serves the whole run of segments that continue it. */
        *end = range_end(pls, seg);
        av_dict_set_int(&opts, "offset", seg->url_offset, 0);
        av_dict_set_int(&opts, "end_offset", *end, 0);
    }

    if (pls->is_subtitle) {
        // don't try to load subtitles using byterange - some servers will respond with HTTP 416
        av_dict_set(&opts, "seekable", "0", 0);
    }

    av_log(pls->parent, AV_LOG_VERBOSE, "HLS request for url '%s', offset %"PRId64", playlist %d\n",
           seg->url, seg->url_offset, pls->index);

    if (seg->key_type == KEY_AES_128 || seg->key_type == KEY_SAMPLE_AES) {
        if (strcmp(seg->key, pls->key_url)) {
            ret = read_key(c, pls, seg);
            if (ret < 0)
                goto cleanup;
        }
    }

    if (seg->key_type == KEY_AES_128) {
        char iv[33], key[33], url[MAX_URL_SIZE];
        ff_data_to_hex(iv, seg->iv, sizeof(seg->iv), 0);
        ff_data_to_hex(key, pls->key, sizeof(pls->key), 0);
        if (strstr(seg->url, "://"))
            snprintf(url, sizeof(url), "crypto+%s", seg->url);
        else
            snprintf(url, sizeof(url), "crypto:%s", seg->url);

        av_dict_set(&opts, "key", key, 0);
        av_dict_set(&opts, "iv", iv, 0);

        ret = open_url(pls->parent, in, url, &c->avio_opts, opts, &is_http);
        if (ret < 0) {
            goto cleanup;
        }
        ret = 0;
    } else {
        ret = open_url(pls->parent, in, seg->url, &c->avio_opts, opts, &is_http);
    }

    /* Seek to the requested position. If this was a HTTP request, the offset
     * should already be where want it to, but this allows e.g. local testing
     * without a HTTP server.
     *
     * This is not done for HTTP at all as avio_seek() does internal bookkeeping
     * of file offset which is out-of-sync with the actual offset when "offset"
     * AVOption is used with http protocol, causing the seek to not be a no-op
     * as would be expected. Wrong offset received from the server will not be
     * noticed without the call, though.
     */
    if (ret == 0 && !is_http && seg->url_offset) {
        int64_t seekret = avio_seek(*in, seg->url_offset, SEEK_SET);
        if (seekret < 0) {
            av_log(pls->parent, AV_LOG_ERROR, "Unable to seek to offset %"PRId64" of HLS segment '%s'\n", seg->url_offset, seg->url);
            ret = seekret;
            ff_format_io_close(pls->parent, in);
        }
    }

cleanup:
    av_dict_free(&opts);
    pls->cur_seg_offset = 0;
    return ret;
}

/**
 * Pass the pssh boxes and the KEYID of the CENC key tags of seg to the mov
 * demuxer of the playlist (decryption_pssh, decryption_kid) when they differ
 * from the ones it has, so that a key it requests for the samples of seg
 * gets the ones of seg (key rotation in the playlist). The segment is
 * opened before the demuxer reads a sample of it.
 */
static void cenc_key_tags_apply(HLSContext *c, struct playlist *pls, const struct segment *seg)
{
    const struct cenc_key_tags *tags;
    int idx;

    if (!seg || !c->cenc_decryption_key_url || !*c->cenc_decryption_key_url ||
        !pls->ctx || !pls->ctx->iformat || !pls->ctx->priv_data ||
        !strstr(pls->ctx->iformat->name, "mov"))
        return;
    idx = seg->cenc_key_tags + 1;
    if (idx == pls->cenc_key_tags_applied)
        return;
    tags = idx ? pls->cenc_key_tags[idx - 1] : NULL;
    av_opt_set(pls->ctx->priv_data, "decryption_pssh", tags && tags->pssh ? tags->pssh : "", 0);
    av_opt_set(pls->ctx->priv_data, "decryption_kid", tags ? tags->kid : "", 0);
    pls->cenc_key_tags_applied = idx;
}

static int update_init_section(struct playlist *pls, struct segment *seg)
{
    static const int max_init_section_size = 1024*1024;
    HLSContext *c = pls->parent->priv_data;
    int64_t sec_size;
    int64_t urlsize;
    int ret;

    if (seg->init_section == pls->cur_init_section)
        return 0;

    pls->cur_init_section = NULL;

    if (!seg->init_section)
        return 0;

    ret = open_input(c, pls, seg->init_section, &pls->input, &pls->input_end);
    if (ret < 0) {
        av_log(pls->parent, AV_LOG_WARNING,
               "Failed to open an initialization section in playlist %d\n",
               pls->index);
        return ret;
    }

    if (seg->init_section->size >= 0)
        sec_size = seg->init_section->size;
    else if ((urlsize = avio_size(pls->input)) >= 0)
        sec_size = urlsize;
    else
        sec_size = max_init_section_size;

    av_log(pls->parent, AV_LOG_DEBUG,
           "Downloading an initialization section of size %"PRId64"\n",
           sec_size);

    sec_size = FFMIN(sec_size, max_init_section_size);

    av_fast_malloc(&pls->init_sec_buf, &pls->init_sec_buf_size, sec_size);

    ret = read_from_url(pls, seg->init_section, pls->init_sec_buf,
                        pls->init_sec_buf_size);
    ff_format_io_close(pls->parent, &pls->input);

    if (ret < 0)
        return ret;

    pls->cur_init_section = seg->init_section;
    pls->init_sec_data_len = ret;
    pls->init_sec_buf_read_offset = 0;

    /* spec says audio elementary streams do not have media initialization
     * sections, so there should be no ID3 timestamps */
    pls->is_id3_timestamped = 0;

    return 0;
}

static int64_t default_reload_interval(struct playlist *pls)
{
    return pls->n_segments > 0 ?
                          pls->segments[pls->n_segments - 1]->duration :
                          pls->target_duration;
}

static int playlist_needed(struct playlist *pls)
{
    AVFormatContext *s = pls->parent;
    int i, j;
    int stream_needed = 0;
    int first_st;

    /* If there is no context or streams yet, the playlist is needed */
    if ((!pls->ctx || !pls->n_associated_streams) && !pls->is_subtitle)
        return 1;

    /* check if any of the streams in the playlist are needed */
    for (i = 0; i < pls->n_associated_streams; i++) {
        if (pls->associated_streams[i].stream->discard < AVDISCARD_ALL) {
            stream_needed = 1;
            break;
        }
    }

    /* If all streams in the playlist were discarded, the playlist is not
     * needed (regardless of whether whole programs are discarded or not). */
    if (!stream_needed)
        return 0;

    /* Otherwise, check if all the programs (variants) this playlist is in are
     * discarded. Since all streams in the playlist are part of the same programs
     * we can just check the programs of the first stream. */

    first_st = pls->associated_streams[0].stream->index;

    for (i = 0; i < s->nb_programs; i++) {
        AVProgram *program = s->programs[i];
        if (program->discard < AVDISCARD_ALL) {
            for (j = 0; j < program->nb_stream_indexes; j++) {
                if (program->stream_index[j] == first_st) {
                    /* playlist is in an undiscarded program */
                    return 1;
                }
            }
        }
    }

    /* some streams were not discarded but all the programs were */
    return 0;
}

static int reload_playlist(struct playlist *v, HLSContext *c)
{
    int ret = 0;
    int reload_count = 0;

    v->needed = playlist_needed(v);

    if (!v->needed)
        return AVERROR_EOF;

    if (!v->input || (c->http_persistent && v->input_read_done)) {
        int64_t reload_interval;

        /* Check that the playlist is still needed before opening a new
         * segment. */
        v->needed = playlist_needed(v);

        if (!v->needed) {
            av_log(v->parent, AV_LOG_INFO, "No longer receiving playlist %d ('%s')\n",
                   v->index, v->url);
            return AVERROR_EOF;
        }

        /* If this is a live stream and the reload interval has elapsed since
         * the last playlist reload, reload the playlists now. */
        reload_interval = default_reload_interval(v);

reload:
        reload_count++;
        if (reload_count > c->max_reload)
            return AVERROR_EOF;
        if (!v->finished &&
            av_gettime_relative() - v->last_load_time >= reload_interval) {
            if ((ret = parse_playlist(c, v->url, v, NULL)) < 0) {
                if (ret != AVERROR_EXIT)
                    av_log(v->parent, AV_LOG_WARNING, "Failed to reload playlist %d\n",
                           v->index);
                return ret;
            }
            /* If we need to reload the playlist again below (if
             * there's still no more segments), switch to a reload
             * interval of half the target duration. */
            reload_interval = v->target_duration / 2;
        }
        if (v->cur_seq_no < v->start_seq_no) {
            av_log(v->parent, AV_LOG_WARNING,
                   "skipping %"PRId64" segments ahead, expired from playlists\n",
                   v->start_seq_no - v->cur_seq_no);
            v->cur_seq_no = v->start_seq_no;
        }
        /* Media sequence wrap after server side ad insertion: going back
         * from the ad to the programme, some servers restart the media
         * sequence (e.g. from 325 to 31). cur_seq_no is then far AHEAD of the
         * new playlist window, not behind it, so the branch above does not
         * catch it, and the segment would be waited for up to max_reload
         * times although it never comes. Regular live HLS never gets here:
         * cur_seq_no is at most one segment past the window.
         *
         * The target is not start_seq_no (the oldest segment of the new
         * window) but one segment before the live edge
         * (start_seq_no + n_segments - 2). Right at the edge (n_segments - 1)
         * the picture stalled for a few seconds after every resync, the
         * decoder had no data in reserve; n_segments - 3, after FFmpeg's
         * live_start_index default for a cold start, jumped back three
         * segments (15 s with 5 s segments) for no reason, as the player
         * still has data buffered.
         *
         * The +6 segments of tolerance keep a lagging CDN edge (a playlist a
         * segment or two behind) from triggering it; a real wrap is far
         * outside the window anyway. Tested live on 2026-10-25. */
        else if (v->n_segments > 0 &&
                 v->cur_seq_no > v->start_seq_no + v->n_segments + 6) {
            int64_t target_seq_no = v->start_seq_no + FFMAX(v->n_segments - 2, 0);
            av_log(v->parent, AV_LOG_WARNING,
                   "cur_seq_no %"PRId64" is far beyond playlist window [%"PRId64" - %"PRId64"], resyncing to live edge (seq %"PRId64")\n",
                   v->cur_seq_no, v->start_seq_no, v->start_seq_no + v->n_segments - 1, target_seq_no);
            v->cur_seq_no = target_seq_no;
            v->last_seq_no = target_seq_no - 1;
            /* the prefetched next segment is one of the old sequence */
            ff_format_io_close(v->parent, &v->input_next);
            v->input_next_end = -1;
            v->input_next_requested = 0;
        }
        if (v->cur_seq_no > v->last_seq_no) {
            v->last_seq_no = v->cur_seq_no;
            v->m3u8_hold_counters = 0;
        } else if (v->last_seq_no == v->cur_seq_no) {
            v->m3u8_hold_counters++;
            if (v->m3u8_hold_counters >= c->m3u8_hold_counters) {
                return AVERROR_EOF;
            }
        } else {
            av_log(v->parent, AV_LOG_WARNING, "The m3u8 list sequence may have been wrapped.\n");
        }
        if (v->cur_seq_no >= v->start_seq_no + v->n_segments) {
            if (v->finished || v->is_subtitle)
                return AVERROR_EOF;
            while (av_gettime_relative() - v->last_load_time < reload_interval) {
                if (ff_check_interrupt(c->interrupt_callback))
                    return AVERROR_EXIT;
                av_usleep(100*1000);
            }
            /* Enough time has elapsed since the last reload */
            goto reload;
        }

    }
    return ret;
}

/* The segment at cur_seq_no starts a new discontinuity region. Close the
 * segment connections and let hls_read_packet() restart the segment
 * demuxer on it; the next read opens the segment afresh. Only reached for
 * playlists with discontinuities, see discontinuity_changed(). */
static int discontinuity_restart(HLSContext *c, struct playlist *v, struct segment *seg)
{
    ff_format_io_close(v->parent, &v->input);
    v->input_read_done = 0;
    v->input_end = -1;
    ff_format_io_close(v->parent, &v->input_next);
    v->input_next_end = -1;
    v->input_next_requested = 0;
    drop_segment_head(v);
    /* a seek back into the region left reuses its offset */
    disc_offset_store(c, v, v->active_discontinuity_sequence, v->discontinuity_offset);
    v->active_discontinuity_sequence = seg->discontinuity_sequence;
    v->discontinuity_pending = 1;
    return AVERROR_EOF;
}

static int read_data_continuous(void *opaque, uint8_t *buf, int buf_size)
{
    struct playlist *v = opaque;
    HLSContext *c = v->parent->priv_data;
    int ret;
    int just_opened = 0;
    int segment_retries = 0;
    struct segment *seg;

    /* A new discontinuity region waits for hls_read_packet() to restart the
     * segment demuxer, which happens when that demuxer has returned its
     * last packet and the end of its input. Until then there is no data of
     * the new region: avio asks again after an end of file (avio_feof()),
     * and the data handed out then would be parsed by the demuxer that was
     * to be restarted, with the timestamp offset of the region before. */
    if (v->discontinuity_pending)
        return AVERROR_EOF;

    if (c->http_persistent && v->input_read_done) {
        ret = reload_playlist(v, c);
        if (ret < 0)
            return ret;
    }

    v->input_read_done = 0;

restart:
    ret = reload_playlist(v, c);
    if (ret < 0)
        return ret;

    seg = current_segment(v);

    if (!v->input || v->input_read_done) {
        if (discontinuity_changed(v, seg))
            return discontinuity_restart(c, v, seg);

        /* load/update Media Initialization Section, if any */
        ret = update_init_section(v, seg);
        if (ret)
            return ret;

        cenc_key_tags_apply(c, v, seg);

        if (c->http_multiple == 1 && v->input_next_requested) {
            FFSWAP(AVIOContext *, v->input, v->input_next);
            FFSWAP(int64_t, v->input_end, v->input_next_end);
            v->cur_seg_offset = 0;
            v->input_next_requested = 0;
            ret = 0;
        } else {
            ret = open_input(c, v, seg, &v->input, &v->input_end);
        }
        if (ret < 0) {
            if (ff_check_interrupt(c->interrupt_callback))
                return AVERROR_EXIT;
            av_log(v->parent, AV_LOG_WARNING, "Failed to open segment %"PRId64" of playlist %d\n",
                   v->cur_seq_no,
                   v->index);
            if (segment_retries >= c->seg_max_retry) {
                av_log(v->parent, AV_LOG_WARNING, "Segment %"PRId64" of playlist %d failed too many times, skipping\n",
                       v->cur_seq_no,
                       v->index);
                v->cur_seq_no++;
                segment_retries = 0;
            } else {
                segment_retries++;
            }
            goto restart;
        }
        segment_retries = 0;
        just_opened = 1;
        v->input_read_done = 0;
        drop_segment_head(v);
        v->head_check = 1;
    }

    if (c->http_multiple == -1) {
        uint8_t *http_version_opt = NULL;
        int r = av_opt_get(v->input, "http_version", AV_OPT_SEARCH_CHILDREN, &http_version_opt);
        if (r >= 0) {
            c->http_multiple = (!strncmp((const char *)http_version_opt, "1.1", 3) || !strncmp((const char *)http_version_opt, "2.0", 3));
            av_freep(&http_version_opt);
        }
    }

    seg = next_segment(v);
    if (c->http_multiple == 1 && !v->input_next_requested &&
        seg && !seg->discontinuity &&
        seg->key_type == KEY_NONE && av_strstart(seg->url, "http", NULL) &&
        !segment_continues(current_segment(v), seg, v->input_end)) {
        ret = open_input(c, v, seg, &v->input_next, &v->input_next_end);
        if (ret < 0) {
            if (ff_check_interrupt(c->interrupt_callback))
                return AVERROR_EXIT;
            av_log(v->parent, AV_LOG_WARNING, "Failed to open segment %"PRId64" of playlist %d\n",
                   v->cur_seq_no + 1,
                   v->index);
        } else {
            v->input_next_requested = 1;
        }
    }

    if (v->init_sec_buf_read_offset < v->init_sec_data_len) {
        /* Push init section out first before first actual segment */
        int copy_size = FFMIN(v->init_sec_data_len - v->init_sec_buf_read_offset, buf_size);
        memcpy(buf, v->init_sec_buf, copy_size);
        v->init_sec_buf_read_offset += copy_size;
        return copy_size;
    }

    seg = current_segment(v);
    if (v->head_check) {
        v->head_check = 0;
        if (c->skip_image_head && !v->is_subtitle) {
            ret = skip_image_head(v, seg);
            if (ret < 0)
                return ret;
        }
    }
    ret = read_from_url(v, seg, buf, buf_size);
    if (ret > 0) {
        if (just_opened && v->is_id3_timestamped != 0) {
            /* Intercept ID3 tags here, elementary audio streams are required
             * to convey timestamps using them in the beginning of each segment. */
            intercept_id3(v, buf, buf_size, &ret);
        }

        return ret;
    }
    if (ret == 0 && segment_continues(seg, next_segment(v), v->input_end)) {
        /* The next segment follows within the requested range and is read
         * on from the same input. */
        v->cur_seq_no++;
        c->cur_seq_no = v->cur_seq_no;
        v->cur_seg_offset = 0;
        just_opened = 1;
        drop_segment_head(v);
        v->head_check = 1;
        goto restart;
    }
    /* Keep the connection for the next request only if its reply was read
     * to the end. */
    if (c->http_persistent &&
        seg->key_type == KEY_NONE && av_strstart(seg->url, "http", NULL) &&
        (seg->size >= 0 ? ret == 0 && seg->url_offset + seg->size == v->input_end
                        : ret == AVERROR_EOF)) {
        v->input_read_done = 1;
    } else {
        ff_format_io_close(v->parent, &v->input);
    }
    v->cur_seq_no++;

    c->cur_seq_no = v->cur_seq_no;

    seg = current_segment(v);
    if (seg && discontinuity_changed(v, seg))
        return discontinuity_restart(c, v, seg);

    goto restart;
}

static int read_data_subtitle_segment(void *opaque, uint8_t *buf, int buf_size)
{
    struct playlist *v = opaque;
    HLSContext *c = v->parent->priv_data;
    int ret;
    struct segment *seg;

    if (!v->needed || v->cur_seq_no - v->start_seq_no >= v->n_segments) {
        return AVERROR_EOF;
    } else {
        seg = current_segment(v);
    }

    if (!v->input) {
        ret = open_input(c, v, seg, &v->input, &v->input_end);
        if (ret < 0) {
            if (ff_check_interrupt(c->interrupt_callback))
                return AVERROR_EXIT;
            av_log(v->parent, AV_LOG_WARNING, "Failed to open segment of playlist %d\n",
                   v->index);
            return ret;
        }
    }

    return read_from_url(v, seg, buf, buf_size);
}

/* Start of segment seq_no on the presentation timeline (AV_TIME_BASE): the
 * timeline anchor of the playlist plus the EXTINF durations in between.
 * Without an anchor (no packet read from the playlist yet, or a reload that
 * shared no segment with the previous list) the first segment of the
 * playlist is taken to start at c->first_timestamp, as
 * find_timestamp_in_playlist() always assumed. Only used for playlists
 * with discontinuities. */
static int64_t segment_start_time(HLSContext *c, struct playlist *pls,
                                  int64_t seq_no)
{
    int64_t time, anchor, i;

    if (pls->timeline_time != AV_NOPTS_VALUE) {
        time   = pls->timeline_time;
        anchor = av_clip64(pls->timeline_seq_no - pls->start_seq_no, 0, pls->n_segments);
    } else {
        time   = c->first_timestamp == AV_NOPTS_VALUE ? 0 : c->first_timestamp;
        anchor = 0;
    }
    seq_no = av_clip64(seq_no - pls->start_seq_no, 0, pls->n_segments);
    for (i = anchor; i < seq_no; i++)
        time = av_sat_add64(time, pls->segments[i]->duration);
    for (i = seq_no; i < anchor; i++)
        time = av_sat_sub64(time, pls->segments[i]->duration);

    return time;
}

static int nested_io_open(AVFormatContext *s, AVIOContext **pb, const char *url,
                          int flags, AVDictionary **opts)
{
    av_log(s, AV_LOG_ERROR,
           "A HLS playlist item '%s' referred to an external file '%s'. "
           "Opening this file was forbidden for security reasons\n",
           s->url, url);
    return AVERROR(EPERM);
}

/* Open the WebVTT demuxer on the current segment. *unsupported is set if the
 * segment is readable but not WebVTT (e.g. WebVTT or TTML in fragmented MP4),
 * which this demuxer does not read. */
static int init_subtitle_context(struct playlist *pls, int *unsupported)
{
    HLSContext *c = pls->parent->priv_data;
    const AVInputFormat *in_fmt, *detected = NULL;
    AVDictionary *opts = NULL;
    int ret;

    *unsupported = 0;
    if (!(pls->ctx = avformat_alloc_context()))
        return AVERROR(ENOMEM);

    /* the buffer of the previous segment (a custom I/O context is not freed
     * with the demuxer) */
    av_freep(&pls->pb.pub.buffer);
    pls->read_buffer = av_malloc(INITIAL_BUFFER_SIZE);
    if (!pls->read_buffer) {
        avformat_free_context(pls->ctx);
        pls->ctx = NULL;
        return AVERROR(ENOMEM);
    }

    ffio_init_context(&pls->pb, pls->read_buffer, INITIAL_BUFFER_SIZE, 0, pls,
                      read_data_subtitle_segment, NULL, NULL);
    pls->pb.pub.seekable = 0;
    pls->ctx->pb       = &pls->pb.pub;
    pls->ctx->io_open  = nested_io_open;

    ret = ff_copy_whiteblacklists(pls->ctx, pls->parent);
    if (ret < 0)
        goto fail;

    ret = av_probe_input_buffer2(&pls->pb.pub, &detected,
                                 current_segment(pls)->url, NULL, 0, 0);
    if (ret < 0)
        goto fail;
    in_fmt = av_find_input_format("webvtt");
    if (ret <= AVPROBE_SCORE_RETRY || detected != in_fmt) {
        /* a format told apart for sure is one of the whole rendition, a
         * guess may be a single broken segment (an error page) */
        if (detected && detected != in_fmt && ret > AVPROBE_SCORE_RETRY) {
            av_log(pls->parent, AV_LOG_WARNING,
                   "Subtitle segments of playlist %d are %s, only WebVTT is supported\n",
                   pls->index, detected->name);
            *unsupported = 1;
        }
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    ret = test_segment(pls->parent, in_fmt, pls, current_segment(pls));
    if (ret < 0)
        goto fail;

    av_dict_copy(&opts, c->seg_format_opts, 0);
    ret = avformat_open_input(&pls->ctx, current_segment(pls)->url, in_fmt, &opts);
    av_dict_free(&opts);

    return ret;
fail:
    avformat_free_context(pls->ctx);
    pls->ctx = NULL;
    return ret;
}

/* May the next segment of subtitle playlist pls be loaded? Only if it starts
 * at most one segment after the one the audio/video playlists are in: a
 * selected subtitle rendition is read on demand, not to its end (thousands
 * of mostly empty segments in a long video) as soon as it is switched in. */
static int subtitle_segment_ready(HLSContext *c, struct playlist *pls)
{
    int has_av = 0;
    int64_t origin, pos, i, current = 0;

    for (i = 0; i < c->n_playlists; i++)
        if (!c->playlists[i]->is_subtitle && c->playlists[i]->needed)
            has_av = 1;
    if (!has_av)
        return 1;
    if (c->av_timestamp == AV_NOPTS_VALUE)
        return 0;

    if (pls->finished) {
        if (c->first_timestamp == AV_NOPTS_VALUE)
            return 0;
        origin = segment_start_time(c, pls, pls->start_seq_no);
    } else {
        /* A live window has no timeline anchor for subtitles (their cues
         * say nothing about where a segment starts): the segment selected
         * when the playlist was switched in is taken to start at the
         * position played then. Only paces the requests, the timestamps of
         * the cues are not touched. */
        if (pls->subtitle_gate_origin == AV_NOPTS_VALUE) {
            int64_t n = av_clip64(pls->cur_seq_no - pls->start_seq_no, 0, pls->n_segments);
            origin = c->av_timestamp;
            for (i = 0; i < n; i++)
                origin = av_sat_sub64(origin, pls->segments[i]->duration);
            pls->subtitle_gate_origin = origin;
        }
        origin = pls->subtitle_gate_origin;
    }

    pos = origin;
    while (current + 1 < pls->n_segments &&
           c->av_timestamp >= av_sat_add64(pos, pls->segments[current]->duration)) {
        pos = av_sat_add64(pos, pls->segments[current]->duration);
        current++;
    }
    return pls->cur_seq_no - pls->start_seq_no <= current + 1;
}

static int read_subtitle_packet(struct playlist *v, AVPacket *pkt)
{
    HLSContext *c = v->parent->priv_data;
    struct segment *seg;
    int ret, unsupported;

restart:
    if (!v->ctx) {
        int64_t index = v->cur_seq_no - v->start_seq_no;
        /* no request before the audio/video playlists get near the segment */
        if (index >= 0 && index < v->n_segments && !subtitle_segment_ready(c, v))
            return AVERROR(EAGAIN);
        /* at the end of a live window wait for the reload interval, instead
         * of reloading the playlist for every packet of the others */
        if (index >= v->n_segments && !v->finished &&
            av_gettime_relative() - v->last_load_time < default_reload_interval(v))
            return AVERROR(EAGAIN);
    }
    ret = reload_playlist(v, c);
    if (ret < 0) {
        /* a failed reload is tried again after the reload interval */
        if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
            v->last_load_time = av_gettime_relative();
        return ret;
    }
    if (!v->ctx && !subtitle_segment_ready(c, v))
        return AVERROR(EAGAIN);

    if (v->input && !v->ctx)
        ff_format_io_close(v->parent, &v->input);

    if (!v->ctx) {
        /* Every subtitle segment gets a fresh demuxer anyway, a new
         * discontinuity region only needs its timestamp offset. Checked
         * here, after the reload (which may renumber the regions) and for
         * the first segment after a start, seek or switch in as well. */
        seg = current_segment(v);
        if (seg && discontinuity_changed(v, seg))
            discontinuity_select(c, v, 1);
        if (v->has_discontinuity) {
            /* an offset of the audio/video playlists beats the estimate
             * from the first cue */
            int64_t offset = disc_offset_lookup(c, v, v->active_discontinuity_sequence);
            if (offset != AV_NOPTS_VALUE)
                v->discontinuity_offset = offset;
        }

        ret = init_subtitle_context(v, &unsupported);
        if (ret == AVERROR_EXIT || ret == AVERROR(ENOMEM) ||
            ff_check_interrupt(c->interrupt_callback))
            return ret < 0 ? ret : AVERROR_EXIT;
        if (unsupported) {
            /* the other segments are no different: stop requesting them */
            ff_format_io_close(v->parent, &v->input);
            v->broken = 1;
            v->needed = 0;
            return AVERROR_EOF;
        }
        if (ret < 0) {
            /* A missing or broken subtitle segment loses its cues, not
             * the playback: go on with the next one. */
            av_log(v->parent, AV_LOG_WARNING,
                   "Skipping subtitle segment %"PRId64" of playlist %d: %s\n",
                   v->cur_seq_no, v->index, av_err2str(ret));
            goto next_segment;
        }
    }

    ret = av_read_frame(v->ctx, v->pkt);
    if (!ret) {
        return ret;
    }
    if (ret == AVERROR_EXIT)
        return ret;
next_segment:
    ff_format_io_close(v->parent, &v->input);
    v->cur_seq_no++;
    /* c->cur_seq_no stays the one of the audio/video playlists: a playlist
     * switched in during live playback starts there (select_cur_seq_no()),
     * and subtitles are read up to one segment ahead. */

    avformat_close_input(&v->ctx);

    goto restart;
}

static void add_renditions_to_variant(HLSContext *c, struct variant *var,
                                      enum AVMediaType type, const char *group_id)
{
    int i;

    for (i = 0; i < c->n_renditions; i++) {
        struct rendition *rend = c->renditions[i];

        if (rend->type == type && !strcmp(rend->group_id, group_id)) {

            if (rend->playlist)
                /* rendition is an external playlist
                 * => add the playlist to the variant */
                dynarray_add(&var->playlists, &var->n_playlists, rend->playlist);
            else
                /* rendition is part of the variant main Media Playlist
                 * => add the rendition to the main Media Playlist */
                dynarray_add(&var->playlists[0]->renditions,
                             &var->playlists[0]->n_renditions,
                             rend);
        }
    }
}

static void add_metadata_from_renditions(AVFormatContext *s, struct playlist *pls,
                                         enum AVMediaType type)
{
    int rend_idx = 0;
    int i;

    for (i = 0; i < pls->n_main_streams; i++) {
        AVStream *st = pls->main_streams[i];

        if (st->codecpar->codec_type != type)
            continue;

        for (; rend_idx < pls->n_renditions; rend_idx++) {
            struct rendition *rend = pls->renditions[rend_idx];

            if (rend->type != type)
                continue;

            if (rend->language[0])
                av_dict_set(&st->metadata, "language", rend->language, 0);
            if (rend->name[0])
                av_dict_set(&st->metadata, "comment", rend->name, 0);

            st->disposition |= rend->disposition;
        }
        if (rend_idx >=pls->n_renditions)
            break;
    }
}

/* if timestamp was in valid range: returns 1 and sets seq_no
 * if not: returns 0 and sets seq_no to closest segment */
static int find_timestamp_in_playlist(HLSContext *c, struct playlist *pls,
                                      int64_t timestamp, int64_t *seq_no,
                                      int64_t *seg_start_ts)
{
    int i;
    int64_t pos = pls->has_discontinuity ?
                  segment_start_time(c, pls, pls->start_seq_no) :
                  c->first_timestamp == AV_NOPTS_VALUE ? 0 : c->first_timestamp;

    if (timestamp < pos) {
        /* Seeking before the start of the playlist, clamp to the first segment. */
        *seq_no = pls->start_seq_no;
        if (seg_start_ts)
            *seg_start_ts = pos;
        return 1;
    }

    for (i = 0; i < pls->n_segments; i++) {
        int64_t diff = pos + pls->segments[i]->duration - timestamp;
        if (diff > 0) {
            *seq_no = pls->start_seq_no + i;
            if (seg_start_ts) {
                *seg_start_ts = pos;
            }
            return 1;
        }
        pos += pls->segments[i]->duration;
    }

    *seq_no = pls->start_seq_no + pls->n_segments - 1;

    return 0;
}

/* Does pls number its segments like the playlists being read, so that
 * c->cur_seq_no is the same content? Without #EXT-X-DISCONTINUITY-SEQUENCE
 * it cannot be told and is assumed as before; with it the segment at
 * c->cur_seq_no has to be in the same region as in another playlist that
 * carries it. */
static int shares_numbering(HLSContext *c, struct playlist *pls)
{
    const struct segment *seg = segment_at(pls, c->cur_seq_no);
    int compared = 0;

    if (!seg)
        return 0;
    if (!pls->discontinuity_sequence_seen)
        return 1;
    for (int i = 0; i < c->n_playlists; i++) {
        struct playlist *other = c->playlists[i];
        const struct segment *oseg;

        if (other == pls || !other->needed || other->is_subtitle ||
            !other->discontinuity_sequence_seen)
            continue;
        oseg = segment_at(other, c->cur_seq_no);
        if (!oseg)
            continue;
        if (oseg->discontinuity_sequence == seg->discontinuity_sequence)
            return 1;
        compared = 1;
    }
    return !compared;
}

/* The segment of pls that timestamp falls into, counted from the timeline
 * anchor of the playlist (which must be set), or -1 if the timestamp is not
 * in the window: the anchor and the timestamp do not belong together then
 * (a playing position outside the live window, timestamps that wrapped). */
static int64_t seq_no_by_anchor(HLSContext *c, struct playlist *pls, int64_t timestamp)
{
    int64_t pos = segment_start_time(c, pls, pls->start_seq_no);

    if (timestamp < pos)
        return -1;
    for (int i = 0; i < pls->n_segments; i++) {
        pos = av_sat_add64(pos, pls->segments[i]->duration);
        if (timestamp < pos)
            return pls->start_seq_no + i;
    }
    return -1;
}

static int64_t select_cur_seq_no(HLSContext *c, struct playlist *pls)
{
    int64_t seq_no;
    int live_start_index = c->live_start_index == LIVE_START_INDEX_AUTO ?
                           -3 : c->live_start_index;

    if (!pls->finished && !c->first_packet &&
        av_gettime_relative() - pls->last_load_time >= default_reload_interval(pls))
        /* reload the playlist since it was suspended */
        parse_playlist(c, pls->url, pls, NULL);

    /* If playback is already in progress (we are just selecting a new
     * playlist) and this is a complete file, find the matching segment
     * by counting durations. */
    if ((pls->finished || pls->type == PLS_TYPE_EVENT) && c->cur_timestamp != AV_NOPTS_VALUE) {
        find_timestamp_in_playlist(c, pls, c->cur_timestamp, &seq_no, NULL);
        return seq_no;
    }

    /* An unfinished EVENT playlist starts on its first segment, unless
     * #EXT-X-START applies or the caller set live_start_index. */
    if (!pls->finished &&
        (pls->type != PLS_TYPE_EVENT ||
         (c->first_packet &&
          ((pls->time_offset_flag && c->prefer_x_start) ||
           c->live_start_index != LIVE_START_INDEX_AUTO)))) {
        if (!c->first_packet) { /* we are doing a segment selection during playback */
            int in_window = c->cur_seq_no >= pls->start_seq_no &&
                            c->cur_seq_no < pls->start_seq_no + pls->n_segments;

            /* With discontinuities the same media sequence number need not
             * be the same content: select by timestamp unless the playlist
             * shares the numbering, but only from a timeline anchor that
             * has followed the playlist (else from the media sequence if it
             * is in the window). */
            if (pls->has_discontinuity && c->cur_timestamp != AV_NOPTS_VALUE &&
                (!in_window ||
                 (pls->timeline_time != AV_NOPTS_VALUE && !shares_numbering(c, pls)))) {
                find_timestamp_in_playlist(c, pls, c->cur_timestamp, &seq_no, NULL);
                return seq_no;
            }
            /* Without discontinuities the media sequence number of the
             * playlists being read is taken as before. Only if this
             * playlist has a timeline anchor (it was read not longer ago
             * than its window is long, the anchor has followed the
             * reloads) and that anchor puts the playing position into the
             * window, more than one segment away from that number or with
             * the number not in the window at all, the renditions are
             * numbered independently: the segment is taken by time then.
             * Without an anchor, or with a playing position that is not
             * in the window by it, the rules below stay. */
            if (!pls->has_discontinuity && pls->n_segments > 0 &&
                c->cur_timestamp != AV_NOPTS_VALUE &&
                pls->timeline_time != AV_NOPTS_VALUE) {
                int64_t by_time = seq_no_by_anchor(c, pls, c->cur_timestamp);
                if (by_time >= 0 &&
                    (!in_window || FFABS(by_time - c->cur_seq_no) > 1))
                    return by_time;
            }
            if (in_window)
                /* While spec 3.4.3 says that we cannot assume anything about the
                 * content at the same sequence number on different playlists,
                 * in practice this seems to work and doing it otherwise would
                 * require us to download a segment to inspect its timestamps. */
                return c->cur_seq_no;
        }

        /* If this is a live stream, start live_start_index segments from the
         * start or end */
        if (live_start_index < 0)
            seq_no = pls->start_seq_no + FFMAX(pls->n_segments +
                                            live_start_index, 0);
        else
            seq_no = pls->start_seq_no + FFMIN(live_start_index,
                                            pls->n_segments - 1);

        /* If #EXT-X-START in playlist, need to recalculate */
        if (pls->time_offset_flag && c->prefer_x_start) {
            int64_t start_timestamp;
            int64_t playlist_duration = 0;
            int64_t cur_timestamp = c->cur_timestamp == AV_NOPTS_VALUE ? 0 :
                                    c->cur_timestamp;

            for (int i = 0; i < pls->n_segments; i++)
                playlist_duration += pls->segments[i]->duration;

            /* If the absolute value of TIME-OFFSET exceeds
             * the duration of the playlist, it indicates either the end of the
             * playlist (if positive) or the beginning of the playlist (if
             * negative). */
            if (pls->start_time_offset >=0 &&
                pls->start_time_offset > playlist_duration)
                start_timestamp = cur_timestamp + playlist_duration;
            else if (pls->start_time_offset >= 0 &&
                        pls->start_time_offset <= playlist_duration)
                start_timestamp = cur_timestamp + pls->start_time_offset;
            else if (pls->start_time_offset < 0 &&
                        pls->start_time_offset < -playlist_duration)
                start_timestamp = cur_timestamp;
            else if (pls->start_time_offset < 0 &&
                        pls->start_time_offset > -playlist_duration)
                start_timestamp = cur_timestamp + playlist_duration +
                                    pls->start_time_offset;
            else
                start_timestamp = cur_timestamp;

            find_timestamp_in_playlist(c, pls, start_timestamp, &seq_no, NULL);
        }
        return seq_no;
    }

    /* Otherwise just start on the first segment. */
    return pls->start_seq_no;
}

static void add_stream_to_programs(AVFormatContext *s, struct playlist *pls, AVStream *stream)
{
    HLSContext *c = s->priv_data;
    int i, j;
    int bandwidth = -1;

    for (i = 0; i < c->n_variants; i++) {
        struct variant *v = c->variants[i];

        for (j = 0; j < v->n_playlists; j++) {
            if (v->playlists[j] != pls)
                continue;

            av_program_add_stream_index(s, i, stream->index);

            if (bandwidth < 0)
                bandwidth = v->bandwidth;
            else if (bandwidth != v->bandwidth)
                bandwidth = -1; /* stream in multiple variants with different bandwidths */
        }
    }

    if (bandwidth >= 0)
        av_dict_set_int(&stream->metadata, "variant_bitrate", bandwidth, 0);
}

static int set_stream_info_from_input_stream(AVStream *st, struct playlist *pls, AVStream *ist)
{
    int err;

    err = avcodec_parameters_copy(st->codecpar, ist->codecpar);
    if (err < 0)
        return err;

    if (pls->is_id3_timestamped) /* custom timestamps via id3 */
        avpriv_set_pts_info(st, 33, 1, MPEG_TIME_BASE);
    else
        avpriv_set_pts_info(st, ist->pts_wrap_bits, ist->time_base.num, ist->time_base.den);

    // copy disposition
    st->disposition = ist->disposition;

    av_dict_copy(&st->metadata, ist->metadata, 0);

    ffstream(st)->need_context_update = 1;

    return 0;
}

static int compatible_stream_type(const AVStream *st, const AVStream *ist)
{
    return st->codecpar->codec_type == ist->codecpar->codec_type ||
           st->codecpar->codec_id == AV_CODEC_ID_NONE;
}

/* Add new subdemuxer streams to our context, if any, and keep the main
 * demuxer streams of the others. The subdemuxer may change what it delivers
 * at an index (after a discontinuity an MPEG-TS PID can carry another kind
 * of stream, a stream can be missing for a while): a main demuxer stream
 * stays the stream it was created for, found again by the id and the media
 * type of the subdemuxer stream, so that the discard flags and the
 * selection of the caller stay with it. */
static int update_streams_from_subdemuxer(AVFormatContext *s, struct playlist *pls)
{
    AVStream **mapping;
    uint8_t *used;
    unsigned int i, j;
    int err, unchanged = pls->n_main_streams == pls->ctx->nb_streams;

    if (unchanged) {
        for (i = 0; i < pls->ctx->nb_streams; i++) {
            AVStream *ist = pls->ctx->streams[i];
            for (j = 0; j < pls->n_associated_streams; j++)
                if (pls->associated_streams[j].stream == pls->main_streams[i])
                    break;
            if (j == pls->n_associated_streams ||
                pls->associated_streams[j].id != ist->id ||
                pls->main_streams[i]->codecpar->codec_type != ist->codecpar->codec_type ||
                pls->main_streams[i]->codecpar->codec_id != ist->codecpar->codec_id ||
                (!pls->is_id3_timestamped &&
                 av_cmp_q(pls->main_streams[i]->time_base, ist->time_base))) {
                unchanged = 0;
                break;
            }
        }
        if (unchanged)
            return 0;
    }
    mapping = av_calloc(pls->ctx->nb_streams, sizeof(*mapping));
    used = av_calloc(pls->n_associated_streams + 1, 1);
    if (!mapping || !used) {
        av_free(mapping);
        av_free(used);
        return AVERROR(ENOMEM);
    }

    /* Match explicit identities before considering any media-type fallback. */
    for (i = 0; i < pls->ctx->nb_streams; i++) {
        AVStream *ist = pls->ctx->streams[i];
        int match = -1;
        for (j = 0; j < pls->n_associated_streams; j++) {
            struct hls_stream *entry = &pls->associated_streams[j];
            if (used[j] || entry->id != ist->id || !compatible_stream_type(entry->stream, ist))
                continue;
            if (i < pls->n_main_streams && entry->stream == pls->main_streams[i]) {
                match = j;
                break;
            }
            match = match == -1 ? j : -2;
        }
        if (match >= 0) {
            mapping[i] = pls->associated_streams[match].stream;
            used[match] = 1;
        }
    }

    /* The only unmatched stream of a media type takes the only unused main
     * stream of that type (its id changed); anything ambiguous is new. */
    for (i = 0; i < pls->ctx->nb_streams; i++) {
        AVStream *ist = pls->ctx->streams[i];
        int match = -1, incoming = 0;
        if (mapping[i])
            continue;
        for (j = 0; j < pls->ctx->nb_streams; j++)
            incoming += !mapping[j] &&
                        pls->ctx->streams[j]->codecpar->codec_type == ist->codecpar->codec_type;
        for (j = 0; j < pls->n_associated_streams; j++)
            if (!used[j] &&
                pls->associated_streams[j].stream->codecpar->codec_type == ist->codecpar->codec_type)
                match = match == -1 ? j : -2;
        if (incoming == 1 && match >= 0) {
            mapping[i] = pls->associated_streams[match].stream;
            pls->associated_streams[match].id = ist->id;
            used[match] = 1;
        }
    }

    for (i = 0; i < pls->ctx->nb_streams; i++) {
        AVStream *ist = pls->ctx->streams[i];
        if (!mapping[i]) {
            struct hls_stream entry;
            mapping[i] = avformat_new_stream(s, NULL);
            if (!mapping[i]) {
                err = AVERROR(ENOMEM);
                goto fail;
            }
            mapping[i]->id = pls->index;
            entry = (struct hls_stream){.stream = mapping[i], .id = ist->id};
            if (!av_dynarray2_add((void **)&pls->associated_streams, &pls->n_associated_streams,
                                 sizeof(entry), (const uint8_t *)&entry)) {
                err = AVERROR(ENOMEM);
                goto fail;
            }
            add_stream_to_programs(s, pls, mapping[i]);

            if (ist->codecpar->codec_id == AV_CODEC_ID_TIMED_ID3) {
                if (pls->timed_id3_stream_index < 0)
                    pls->timed_id3_stream_index = i;
                else if (pls->timed_id3_stream_index != (int)i)
                    av_log(s, AV_LOG_WARNING,
                           "Playlist has multiple timed ID3 streams; "
                           "only stream %d will be used for metadata propagation\n",
                           pls->timed_id3_stream_index);
            }
        }

        err = set_stream_info_from_input_stream(mapping[i], pls, ist);
        if (err < 0)
            goto fail;
    }
    av_free(pls->main_streams);
    pls->main_streams = mapping;
    pls->n_main_streams = pls->ctx->nb_streams;
    av_free(used);
    /* set_stream_info_from_input_stream() took the disposition and the
     * metadata of the segment demuxer, which knows nothing of the
     * rendition (language, name, DEFAULT, FORCED) */
    if (pls->is_subtitle)
        add_metadata_from_renditions(s, pls, AVMEDIA_TYPE_SUBTITLE);
    return 0;
fail:
    av_free(mapping);
    av_free(used);
    return err;
}

/* The stream of a subtitle rendition, created without loading its playlist
 * or a segment: WebVTT, discarded until the caller selects it. */
static int add_lazy_subtitle_stream(AVFormatContext *s, struct playlist *pls)
{
    AVStream *st = avformat_new_stream(s, NULL);
    struct hls_stream entry;

    if (!st)
        return AVERROR(ENOMEM);
    st->id = pls->index;
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id   = AV_CODEC_ID_WEBVTT;
    st->discard = AVDISCARD_ALL;
    avpriv_set_pts_info(st, 33, 1, MPEG_TIME_BASE);
    /* the WebVTT demuxer has a single stream with id 0 */
    entry = (struct hls_stream){ .stream = st, .id = 0 };
    if (!av_dynarray2_add((void **)&pls->associated_streams,
                          &pls->n_associated_streams, sizeof(entry),
                          (const uint8_t *)&entry))
        return AVERROR(ENOMEM);
    pls->main_streams = av_malloc(sizeof(*pls->main_streams));
    if (!pls->main_streams)
        return AVERROR(ENOMEM);
    pls->main_streams[0] = st;
    pls->n_main_streams  = 1;
    add_stream_to_programs(s, pls, st);
    add_metadata_from_renditions(s, pls, AVMEDIA_TYPE_SUBTITLE);
    return 0;
}

static void update_noheader_flag(AVFormatContext *s)
{
    HLSContext *c = s->priv_data;
    int flag_needed = 0;
    int i;

    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];

        if (pls->has_noheader_flag) {
            flag_needed = 1;
            break;
        }
    }

    if (flag_needed)
        s->ctx_flags |= AVFMTCTX_NOHEADER;
    else
        s->ctx_flags &= ~AVFMTCTX_NOHEADER;
}

static int hls_close(AVFormatContext *s)
{
    HLSContext *c = s->priv_data;

    free_playlist_list(c);
    free_variant_list(c);
    free_rendition_list(c);

    if (c->crypto_ctx.aes_ctx)
        av_free(c->crypto_ctx.aes_ctx);

    av_dict_free(&c->avio_opts);
    ff_format_io_close(c->ctx, &c->playlist_pb);

    return 0;
}

/* Is the rendition part of what hls_quality_mode / hls_audio_default_only
 * leave to be opened? */
static int rendition_selected(HLSContext *c, struct rendition *rend)
{
    struct variant *v = c->variants[c->selected_variant];
    const char *group = rend->type == AVMEDIA_TYPE_AUDIO ? v->audio_group :
                        rend->type == AVMEDIA_TYPE_VIDEO ? v->video_group :
                        v->subtitles_group;
    int i;

    if (c->hls_quality_mode && strcmp(group, rend->group_id))
        return 0;
    if (c->hls_audio_default_only && rend->type == AVMEDIA_TYPE_AUDIO &&
        !(rend->disposition & AV_DISPOSITION_DEFAULT)) {
        /* a group without a default rendition keeps all of them */
        for (i = 0; i < c->n_renditions; i++) {
            struct rendition *other = c->renditions[i];
            if (other->type == AVMEDIA_TYPE_AUDIO &&
                !strcmp(other->group_id, rend->group_id) &&
                (other->disposition & AV_DISPOSITION_DEFAULT))
                return 0;
        }
    }
    return 1;
}

/* hls_quality_mode: only the variant with the lowest (1) or highest (2)
 * BANDWIDTH and the renditions of its groups are opened.
 * hls_audio_default_only: of an audio group with a DEFAULT=YES rendition only
 * that one is opened. A playlist stays if anything that is kept refers to it
 * (playlists are shared by URL). Without the options nothing is excluded. */
static void preselect_playlists(HLSContext *c)
{
    int i, j;

    c->selected_variant = 0;
    if (!c->hls_quality_mode && !c->hls_audio_default_only) {
        for (i = 0; i < c->n_playlists; i++)
            c->playlists[i]->excluded = 0;
        return;
    }
    if (c->hls_quality_mode) {
        for (i = 1; i < c->n_variants; i++) {
            int bandwidth = c->variants[i]->bandwidth;
            int best = c->variants[c->selected_variant]->bandwidth;
            if ((c->hls_quality_mode == 1 && bandwidth < best) ||
                (c->hls_quality_mode == 2 && bandwidth > best))
                c->selected_variant = i;
        }
    }
    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];
        int keep = 0;

        for (j = 0; j < c->n_variants; j++) {
            if ((!c->hls_quality_mode || j == c->selected_variant) &&
                c->variants[j]->n_playlists &&
                c->variants[j]->playlists[0] == pls)
                keep = 1;
        }
        for (j = 0; j < c->n_renditions; j++) {
            struct rendition *rend = c->renditions[j];
            if (rend->playlist == pls && rendition_selected(c, rend))
                keep = 1;
        }
        pls->excluded = !keep;
    }
}

static int hls_read_header(AVFormatContext *s)
{
    HLSContext *c = s->priv_data;
    int ret = 0, i;
    int64_t highest_cur_seq_no = 0;

    c->ctx                = s;
    c->interrupt_callback = &s->interrupt_callback;

    c->first_packet = 1;
    c->selected_variant = 0;
    c->first_timestamp = AV_NOPTS_VALUE;
    c->first_timestamp_pls = NULL;
    c->cur_timestamp = AV_NOPTS_VALUE;
    c->n_disc_offsets = 0;
    c->disc_offsets_next = 0;
    c->av_timestamp = AV_NOPTS_VALUE;

    if ((ret = ffio_copy_url_options(s->pb, &c->avio_opts)) < 0)
        return ret;

    /* http_persistent and http_multiple auto-detection both rely on the
     * AVIOContext being backed by the builtin URLContext. Neither works
     * when io_open is overridden with a custom callback. */
    if (!ffio_geturlcontext(s->pb)) {
        if (c->http_persistent) {
            av_log(s, AV_LOG_WARNING, "Disabling http_persistent due to custom io_open.\n");
            c->http_persistent = 0;
        }
        /* Only auto-detection is disabled, enabling http_multiple can still work
         * with custom io_open. */
        if (c->http_multiple == -1) {
            av_log(s, AV_LOG_WARNING, "Disabling http_multiple due to custom io_open.\n");
            c->http_multiple = 0;
        }
    }

    /* XXX: Some HLS servers don't like being sent the range header,
       in this case, we need to set http_seekable = 0 to disable
       the range header */
    av_dict_set_int(&c->avio_opts, "seekable", c->http_seekable, 0);

    if ((ret = parse_playlist(c, s->url, NULL, s->pb)) < 0)
        return ret;

    if (c->n_variants == 0) {
        av_log(s, AV_LOG_WARNING, "Empty playlist\n");
        return AVERROR_EOF;
    }
    preselect_playlists(c);
    /* If the playlist only contained playlists (Master Playlist),
     * parse each individual playlist. */
    if (c->n_playlists > 1 || c->playlists[0]->n_segments == 0) {
        for (i = 0; i < c->n_playlists; i++) {
            struct playlist *pls = c->playlists[i];
            /* subtitle renditions are loaded when they are selected */
            if (pls->excluded || pls->is_subtitle)
                continue;
            pls->m3u8_hold_counters = 0;
            if ((ret = parse_playlist(c, pls->url, pls, NULL)) < 0) {
                av_log(s, AV_LOG_WARNING, "parse_playlist error %s [%s]\n", av_err2str(ret), pls->url);
                pls->broken = 1;
                if (c->n_playlists > 1)
                    continue;
                return ret;
            }
        }
    }

    for (i = 0; i < c->n_variants; i++) {
        if (c->variants[i]->playlists[0]->excluded)
            continue;
        if (c->variants[i]->playlists[0]->n_segments == 0) {
            av_log(s, AV_LOG_WARNING, "Empty segment [%s]\n", c->variants[i]->playlists[0]->url);
            c->variants[i]->playlists[0]->broken = 1;
        }
    }

    /* Calculate the total duration of the stream if all segments are
     * available (finished or EVENT playlists). */
    if (c->variants[c->selected_variant]->playlists[0]->finished ||
        c->variants[c->selected_variant]->playlists[0]->type == PLS_TYPE_EVENT) {
        struct playlist *main_pls = c->variants[c->selected_variant]->playlists[0];
        int64_t duration = 0;
        for (i = 0; i < main_pls->n_segments; i++)
            duration += main_pls->segments[i]->duration;
        s->duration = duration;
    }

    /* Associate renditions with variants */
    for (i = 0; i < c->n_variants; i++) {
        struct variant *var = c->variants[i];

        if (var->audio_group[0])
            add_renditions_to_variant(c, var, AVMEDIA_TYPE_AUDIO, var->audio_group);
        if (var->video_group[0])
            add_renditions_to_variant(c, var, AVMEDIA_TYPE_VIDEO, var->video_group);
        if (var->subtitles_group[0])
            add_renditions_to_variant(c, var, AVMEDIA_TYPE_SUBTITLE, var->subtitles_group);
    }

    /* Create a program for each variant */
    for (i = 0; i < c->n_variants; i++) {
        struct variant *v = c->variants[i];
        AVProgram *program;

        program = av_new_program(s, i);
        if (!program)
            return AVERROR(ENOMEM);
        av_dict_set_int(&program->metadata, "variant_bitrate", v->bandwidth, 0);
    }

    /* Select the starting segments */
    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];

        if (pls->excluded || pls->n_segments == 0)
            continue;

        pls->cur_seq_no = select_cur_seq_no(c, pls);
        highest_cur_seq_no = FFMAX(highest_cur_seq_no, pls->cur_seq_no);
    }

    av_dict_set(&c->seg_format_opts, "prefer_hls_mpegts_pts", "1", 0);

    /* Open the demuxer for each playlist */
    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];
        const AVInputFormat *in_fmt = NULL;
        char *url;
        AVDictionary *options = NULL;
        struct segment *seg = NULL;

        if (pls->excluded)
            continue;

        if (pls->is_subtitle) {
            /* neither playlist nor segment are loaded until it is selected
             * (recheck_discard_flags()) */
            pls->index  = i;
            pls->parent = s;
            ret = add_lazy_subtitle_stream(s, pls);
            if (ret < 0)
                return ret;
            continue;
        }

        if (!(pls->ctx = avformat_alloc_context()))
            return AVERROR(ENOMEM);
        ffformatcontext(pls->ctx)->cenc_key_parent = s;

        if (pls->n_segments == 0)
            continue;

        pls->index  = i;
        pls->needed = 1;
        pls->parent = s;

        /*
         * If this is a live stream and this playlist looks like it is one segment
         * behind, try to sync it up so that every substream starts at the same
         * time position (so e.g. avformat_find_stream_info() will see packets from
         * all active streams within the first few seconds). This is not very generic,
         * though, as the sequence numbers are technically independent.
         */
        if (!pls->finished && pls->cur_seq_no == highest_cur_seq_no - 1 &&
            highest_cur_seq_no < pls->start_seq_no + pls->n_segments) {
            pls->cur_seq_no = highest_cur_seq_no;
        }

        /* the region of the segment actually started on */
        discontinuity_select(c, pls, 0);

        pls->read_buffer = av_malloc(INITIAL_BUFFER_SIZE);
        if (!pls->read_buffer){
            avformat_free_context(pls->ctx);
            pls->ctx = NULL;
            return AVERROR(ENOMEM);
        }

        ffio_init_context(&pls->pb, pls->read_buffer, INITIAL_BUFFER_SIZE, 0, pls,
                          read_data_continuous, NULL, NULL);

        /*
         * If encryption scheme is SAMPLE-AES, try to read  ID3 tags of
         * external audio track that contains audio setup information
         */
        seg = current_segment(pls);
        if (seg && seg->key_type == KEY_SAMPLE_AES && pls->n_renditions > 0 &&
            pls->renditions[0]->type == AVMEDIA_TYPE_AUDIO) {
            uint8_t buf[HLS_MAX_ID3_TAGS_DATA_LEN];
            if ((ret = avio_read(&pls->pb.pub, buf, HLS_MAX_ID3_TAGS_DATA_LEN)) < 0) {
                /* Fail if error was not end of file */
                if (ret != AVERROR_EOF) {
                    avformat_free_context(pls->ctx);
                    pls->ctx = NULL;
                    return ret;
                }
            }
            ret = 0;
            /* Reset reading */
            ff_format_io_close(pls->parent, &pls->input);
            pls->input = NULL;
            pls->input_read_done = 0;
            ff_format_io_close(pls->parent, &pls->input_next);
            pls->input_next = NULL;
            pls->input_next_requested = 0;
            pls->cur_seg_offset = 0;
            drop_segment_head(pls);
            pls->cur_init_section = NULL;
            /* Reset EOF flag */
            pls->pb.pub.eof_reached = 0;
            /* Clear any buffered data */
            pls->pb.pub.buf_end = pls->pb.pub.buf_ptr = pls->pb.pub.buffer;
            /* Reset the position */
            pls->pb.pub.pos = 0;
        }

        /*
         * If encryption scheme is SAMPLE-AES and audio setup information is present in external audio track,
         * use that information to find the media format, otherwise probe input data
         */
        seg = current_segment(pls);
        if (seg && seg->key_type == KEY_SAMPLE_AES && pls->is_id3_timestamped &&
            pls->audio_setup_info.codec_id != AV_CODEC_ID_NONE) {
            av_assert1(pls->audio_setup_info.codec_id == AV_CODEC_ID_AAC ||
                       pls->audio_setup_info.codec_id == AV_CODEC_ID_AC3 ||
                       pls->audio_setup_info.codec_id == AV_CODEC_ID_EAC3);
            // Keep this list in sync with ff_hls_senc_read_audio_setup_info()
            in_fmt = av_find_input_format(pls->audio_setup_info.codec_id == AV_CODEC_ID_AAC ? "aac" :
                                          pls->audio_setup_info.codec_id == AV_CODEC_ID_AC3 ? "ac3" : "eac3");
        } else {
            pls->ctx->probesize = s->probesize > 0 ? s->probesize : 1024 * 4;
            pls->ctx->max_analyze_duration = s->max_analyze_duration > 0 ? s->max_analyze_duration : 4 * AV_TIME_BASE;
            pls->ctx->interrupt_callback = s->interrupt_callback;
            url = av_strdup(pls->segments[0]->url);
            ret = av_probe_input_buffer(&pls->pb.pub, &in_fmt, url, NULL, 0, 0);

            for (int n = 0; n < pls->n_segments; n++)
                if (ret >= 0)
                    ret = test_segment(s, in_fmt, pls, pls->segments[n]);

            if (ret < 0) {
                /* Free the ctx - it isn't initialized properly at this point,
                * so avformat_close_input shouldn't be called. If
                * avformat_open_input fails below, it frees and zeros the
                * context, so it doesn't need any special treatment like this. */
                av_log(s, AV_LOG_ERROR, "Error when loading first segment '%s'\n", url);
                avformat_free_context(pls->ctx);
                pls->ctx = NULL;
                av_free(url);
                return ret;
            }
            av_free(url);
        }

        seg = current_segment(pls);
        if (seg && seg->key_type == KEY_SAMPLE_AES) {
            if (strstr(in_fmt->name, "mov")) {
                if (!strncmp(pls->key_url, "keys://", 7)) {
                    /* special URL with encoded kid=key pairs */
                    av_dict_set(&options, "decryption_keys", pls->key_url + 7, 0);
                } else {
                    char key[33];
                    ff_data_to_hex(key, pls->key, sizeof(pls->key), 0);
                    av_dict_set(&options, "decryption_key", key, 0);
                }
            } else if (!c->crypto_ctx.aes_ctx) {
                c->crypto_ctx.aes_ctx = av_aes_alloc();
                if (!c->crypto_ctx.aes_ctx) {
                    avformat_free_context(pls->ctx);
                    pls->ctx = NULL;
                    return AVERROR(ENOMEM);
                }
            }
        }

        pls->ctx->pb       = &pls->pb.pub;
        pls->ctx->io_open  = nested_io_open;
        pls->ctx->flags   |= s->flags & ~AVFMT_FLAG_CUSTOM_IO;

        if ((ret = ff_copy_whiteblacklists(pls->ctx, s)) < 0)
            return ret;

        av_dict_copy(&options, c->seg_format_opts, 0);
        if (c->cenc_decryption_key_url && *c->cenc_decryption_key_url && strstr(in_fmt->name, "mov")) {
            /* the key tags of the segment the demuxer reads first */
            const struct segment *first = current_segment(pls);
            av_dict_set(&options, "decryption_key_url", c->cenc_decryption_key_url, 0);
            if (first && first->cenc_key_tags >= 0) {
                const struct cenc_key_tags *tags = pls->cenc_key_tags[first->cenc_key_tags];
                if (tags->pssh)
                    av_dict_set(&options, "decryption_pssh", tags->pssh, 0);
                if (tags->kid[0])
                    av_dict_set(&options, "decryption_kid", tags->kid, 0);
                pls->cenc_key_tags_applied = first->cenc_key_tags + 1;
            }
        }

        ret = avformat_open_input(&pls->ctx, pls->segments[0]->url, in_fmt, &options);
        av_dict_free(&options);
        if (ret < 0)
            return ret;

        if (pls->id3_deferred_extra && pls->ctx->nb_streams == 1) {
            ff_id3v2_parse_apic(pls->ctx, pls->id3_deferred_extra);
            avformat_queue_attached_pictures(pls->ctx);
            ff_id3v2_parse_priv(pls->ctx, pls->id3_deferred_extra);
            ff_id3v2_free_extra_meta(&pls->id3_deferred_extra);
        }

        if (pls->is_id3_timestamped == -1)
            av_log(s, AV_LOG_WARNING, "No expected HTTP requests have been made\n");

        /*
         * For ID3 timestamped raw audio streams we need to detect the packet
         * durations to calculate timestamps in fill_timing_for_id3_timestamped_stream(),
         * but for other streams we can rely on our user calling avformat_find_stream_info()
         * on us if they want to.
         */
        if (pls->is_id3_timestamped || (pls->n_renditions > 0 && pls->renditions[0]->type == AVMEDIA_TYPE_AUDIO)) {
            seg = current_segment(pls);
            if (seg && seg->key_type == KEY_SAMPLE_AES && pls->audio_setup_info.setup_data_length > 0 &&
                pls->ctx->nb_streams == 1)
                ret = ff_hls_senc_parse_audio_setup_info(pls->ctx->streams[0], &pls->audio_setup_info);
            else
                ret = avformat_find_stream_info(pls->ctx, NULL);

            if (ret < 0)
                return ret;
        }

        pls->has_noheader_flag = !!(pls->ctx->ctx_flags & AVFMTCTX_NOHEADER);

        /* Create new AVStreams for each stream in this playlist */
        ret = update_streams_from_subdemuxer(s, pls);
        if (ret < 0)
            return ret;

        /*
         * Copy any metadata from playlist to main streams, but do not set
         * event flags.
         */
        if (pls->n_main_streams)
            av_dict_copy(&pls->main_streams[0]->metadata, pls->ctx->metadata, 0);

        add_metadata_from_renditions(s, pls, AVMEDIA_TYPE_AUDIO);
        add_metadata_from_renditions(s, pls, AVMEDIA_TYPE_VIDEO);
        add_metadata_from_renditions(s, pls, AVMEDIA_TYPE_SUBTITLE);
    }

    update_noheader_flag(s);

    return 0;
}

/* Restart the reading of a playlist (seek, playlist switched in again):
 * drop the segment connections and everything buffered from the old
 * position, in the I/O context, the packet and the segment demuxer. */
static void reset_playlist(struct playlist *pls)
{
    AVIOContext *const pb = &pls->pb.pub;

    ff_format_io_close(pls->parent, &pls->input);
    pls->input_read_done = 0;
    ff_format_io_close(pls->parent, &pls->input_next);
    pls->input_next_requested = 0;
    drop_segment_head(pls);
    /* a key that failed with an error that may pass is requested again */
    if (!key_fail_is_final(pls->key_fail_ret))
        pls->key_fail_count = 0;
    av_packet_unref(pls->pkt);
    pb->eof_reached = 0;
    /* an error of the old position is not the one of the new position */
    pb->error = 0;
    /* Clear any buffered data */
    pb->buf_end = pb->buf_ptr = pb->buffer;
    /* Reset the pos, to let the mpegts/mov demuxer know we've seeked. */
    pb->pos = 0;
    /* Flush the packet queue of the subdemuxer. */
    if (pls->ctx) {
        ff_read_frame_flush(pls->ctx);
        for (unsigned j = 0; j < pls->ctx->nb_streams; j++)
            ffstream(pls->ctx->streams[j])->pts_wrap_reference = AV_NOPTS_VALUE;
    }
    if (pls->is_subtitle)
        avformat_close_input(&pls->ctx);

    /* Reset the init segment so it's re-fetched and served appropriately */
    pls->cur_init_section = NULL;
}

static int recheck_discard_flags(AVFormatContext *s, int first)
{
    HLSContext *c = s->priv_data;
    int i, changed = 0;
    int cur_needed;

    /* Check if any new streams are needed */
    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];

        cur_needed = playlist_needed(c->playlists[i]);

        if (pls->broken || pls->excluded) {
            continue;
        }
        if (cur_needed && !pls->needed) {
            if (pls->is_subtitle && !pls->last_load_time) {
                /* a subtitle rendition selected for the first time */
                int ret;
                pls->m3u8_hold_counters = 0;
                ret = parse_playlist(c, pls->url, pls, NULL);
                if (ret == AVERROR_EXIT)
                    return ret;
                if (ret < 0 || !pls->n_segments) {
                    /* not requested again on every packet */
                    av_log(s, AV_LOG_WARNING, "Subtitle playlist %d failed to load: %s\n",
                           i, ret < 0 ? av_err2str(ret) : "no segments");
                    pls->broken = 1;
                    continue;
                }
            }
            if (pls->is_subtitle) {
                /* re-estimated from the position played now */
                pls->subtitle_gate_origin = AV_NOPTS_VALUE;
                pls->m3u8_hold_counters = 0;
            }
            pls->needed = 1;
            changed = 1;
            /* A playlist switched in during playback may have been read
             * before (for the stream info, or until its streams were
             * discarded): what is left of that, a buffered packet, the
             * rest of a segment, packets queued in the segment demuxer,
             * belongs to another position. Restart it like a seek does.
             * (Before the first packet only playlists that were not read
             * yet are switched in.) */
            if (!first)
                reset_playlist(pls);
            pls->cur_seq_no = select_cur_seq_no(c, pls);
            if (pls->is_subtitle)
                /* not a repeated request of the same segment */
                pls->last_seq_no = pls->cur_seq_no;
            discontinuity_select(c, pls, 1);
            pls->pb.pub.eof_reached = 0;
            if (c->cur_timestamp != AV_NOPTS_VALUE) {
                /* catch up */
                pls->seek_timestamp = c->cur_timestamp +
                    (pls->ts_offset != AV_NOPTS_VALUE ? pls->ts_offset : 0);
                pls->seek_flags = AVSEEK_FLAG_ANY;
                pls->seek_stream_index = -1;
            }
            av_log(s, AV_LOG_INFO, "Now receiving playlist %d, segment %"PRId64"\n", i, pls->cur_seq_no);
        } else if ((first || pls->is_subtitle) && !cur_needed && pls->needed) {
            /* A subtitle rendition switched off during playback stops at
             * once: its buffered cue belongs to a discarded stream, and no
             * further segment is requested. */
            if (pls->is_subtitle)
                av_packet_unref(pls->pkt);
            ff_format_io_close(pls->parent, &pls->input);
            pls->input_read_done = 0;
            ff_format_io_close(pls->parent, &pls->input_next);
            pls->input_next_requested = 0;
            drop_segment_head(pls);
            if (pls->is_subtitle)
                avformat_close_input(&pls->ctx);
            pls->needed = 0;
            changed = 1;
            av_log(s, AV_LOG_INFO, "No longer receiving playlist %d\n", i);
        }
    }
    return changed;
}

static void fill_timing_for_id3_timestamped_stream(struct playlist *pls)
{
    if (pls->id3_offset >= 0) {
        pls->pkt->dts = pls->id3_mpegts_timestamp +
                                 av_rescale_q(pls->id3_offset,
                                              pls->ctx->streams[pls->pkt->stream_index]->time_base,
                                              MPEG_TIME_BASE_Q);
        if (pls->pkt->duration)
            pls->id3_offset += pls->pkt->duration;
        else
            pls->id3_offset = -1;
    } else {
        /* there have been packets with unknown duration
         * since the last id3 tag, should not normally happen */
        pls->pkt->dts = AV_NOPTS_VALUE;
    }

    if (pls->pkt->duration)
        pls->pkt->duration = av_rescale_q(pls->pkt->duration,
                                         pls->ctx->streams[pls->pkt->stream_index]->time_base,
                                         MPEG_TIME_BASE_Q);

    pls->pkt->pts = AV_NOPTS_VALUE;
}

static AVRational get_timebase(struct playlist *pls)
{
    if (pls->is_id3_timestamped)
        return MPEG_TIME_BASE_Q;

    return pls->ctx->streams[pls->pkt->stream_index]->time_base;
}

static int compare_ts_with_wrapdetect(int64_t ts_a, struct playlist *pls_a,
                                      int64_t ts_b, struct playlist *pls_b)
{
    int64_t scaled_ts_a = av_rescale_q(ts_a, get_timebase(pls_a), MPEG_TIME_BASE_Q);
    int64_t scaled_ts_b = av_rescale_q(ts_b, get_timebase(pls_b), MPEG_TIME_BASE_Q);

    return av_compare_mod(scaled_ts_a, scaled_ts_b, 1LL << 33);
}

/**
 * Check whether any entry in @p update differs from the corresponding entry
 * in @p current.
 *
 * HLS segments can repeat timed ID3 metadata in every segment so that
 * listeners joining mid-stream immediately receive the current track
 * information. Only signal a metadata change when values actually differ
 * to avoid spurious updates on every segment boundary.
 */
static int metadata_has_changed(const AVDictionary *current,
                                 const AVDictionary *update)
{
    if (av_dict_count(current) != av_dict_count(update))
        return 1;
    const AVDictionaryEntry *e = NULL;
    while ((e = av_dict_iterate(update, e))) {
        const AVDictionaryEntry *cur = av_dict_get(current, e->key, NULL, 0);
        if (!cur || strcmp(cur->value, e->value))
            return 1;
    }
    return 0;
}

static int hls_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    HLSContext *c = s->priv_data;
    int ret, i, minplaylist = -1;

    recheck_discard_flags(s, c->first_packet);
    c->first_packet = 0;

    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];
        /* Make sure we've got one buffered packet from each open playlist
         * stream */
        if (pls->needed && !pls->pkt->data) {
            while (1) {
                int64_t ts_diff;
                AVRational tb;
                struct segment *seg = NULL;
                if (pls->is_subtitle)
                    ret = read_subtitle_packet(pls, pls->pkt);
                else
                    ret = av_read_frame(pls->ctx, pls->pkt);
                if (ret < 0) {
                    /* No cue now (the next segment is not due yet, the end
                     * of a live window, a failed reload): the audio/video
                     * packets go on. */
                    if (pls->is_subtitle && ret != AVERROR_EXIT &&
                        ret != AVERROR(ENOMEM))
                        break;
                    if (pls->discontinuity_pending) {
                        /* The next segment starts a new discontinuity
                         * region: restart the segment demuxer on it (like
                         * after a seek) and continue the timeline. */
                        AVIOContext *pb = &pls->pb.pub;

                        pls->discontinuity_pending = 0;
                        pls->discontinuity_start_seq_no = pls->cur_seq_no;
                        pls->discontinuity_region_start = 1;
                        pls->discontinuity_offset =
                            disc_offset_lookup(c, pls, pls->active_discontinuity_sequence);
                        av_log(s, AV_LOG_VERBOSE, "Playlist %d: discontinuity sequence %"PRId64
                               " starts at segment %"PRId64"\n", pls->index,
                               pls->active_discontinuity_sequence, pls->cur_seq_no);
                        pls->id3_mpegts_timestamp = AV_NOPTS_VALUE;
                        pls->id3_offset = -1;
                        pb->eof_reached = 0;
                        pb->buf_end = pb->buf_ptr = pb->buffer;
                        pb->pos = 0;
                        ff_read_frame_flush(pls->ctx);
                        for (unsigned j = 0; j < pls->ctx->nb_streams; j++)
                            ffstream(pls->ctx->streams[j])->pts_wrap_reference = AV_NOPTS_VALUE;
                        continue;
                    }
                    if (!avio_feof(&pls->pb.pub) && ret != AVERROR_EOF)
                        return ret;
                    break;
                } else {
                    /* stream_index check prevents matching picture attachments etc. */
                    if (pls->is_id3_timestamped && pls->pkt->stream_index == 0) {
                        /* audio elementary streams are id3 timestamped */
                        fill_timing_for_id3_timestamped_stream(pls);
                    }

                    if (pls->has_discontinuity &&
                        pls->discontinuity_offset == AV_NOPTS_VALUE &&
                        pls->pkt->dts != AV_NOPTS_VALUE) {
                        /* First packet of a discontinuity region: an offset
                         * stored meanwhile (by another playlist if shared) is
                         * reused, else the region continues the timeline at
                         * the scheduled start of its first segment. A
                         * subtitle playlist estimates it from its first cue
                         * only until the audio/video playlists have stored
                         * one (see read_subtitle_packet()). */
                        int64_t offset = disc_offset_lookup(c, pls, pls->active_discontinuity_sequence);

                        if (offset == AV_NOPTS_VALUE) {
                            int64_t raw = av_rescale_q(pls->pkt->dts,
                                                       get_timebase(pls), AV_TIME_BASE_Q);
                            if (c->first_timestamp == AV_NOPTS_VALUE)
                                offset = 0;
                            else
                                offset = av_sat_sub64(segment_start_time(c, pls,
                                                      pls->discontinuity_start_seq_no), raw);
                            disc_offset_store(c, pls, pls->active_discontinuity_sequence, offset);
                            av_log(s, AV_LOG_VERBOSE, "Playlist %d: discontinuity sequence %"PRId64
                                   " timestamp offset %"PRId64" us\n", pls->index,
                                   pls->active_discontinuity_sequence, offset);
                        }
                        pls->discontinuity_offset = offset;
                    }
                    if (pls->has_discontinuity &&
                        pls->discontinuity_offset != AV_NOPTS_VALUE &&
                        pls->discontinuity_offset) {
                        int64_t offset = av_rescale_q(pls->discontinuity_offset,
                                                     AV_TIME_BASE_Q,
                                                     get_timebase(pls));
                        if (pls->pkt->dts != AV_NOPTS_VALUE)
                            pls->pkt->dts = av_sat_add64(pls->pkt->dts, offset);
                        if (pls->pkt->pts != AV_NOPTS_VALUE)
                            pls->pkt->pts = av_sat_add64(pls->pkt->pts, offset);
                    }

                    if (pls->discontinuity_region_start &&
                        pls->pkt->dts != AV_NOPTS_VALUE) {
                        /* The first packet since the segment demuxer was
                         * (re)started comes from segment
                         * discontinuity_start_seq_no: it anchors the timeline
                         * of the playlist unless an anchor has followed it
                         * (bookkeeping only for a playlist without
                         * discontinuities). */
                        pls->discontinuity_region_start = 0;
                        if (pls->timeline_time == AV_NOPTS_VALUE && !pls->is_subtitle) {
                            pls->timeline_seq_no = pls->discontinuity_start_seq_no;
                            pls->timeline_time   = av_rescale_q(pls->pkt->dts,
                                                                get_timebase(pls), AV_TIME_BASE_Q);
                        }
                        if (pls->has_discontinuity)
                            disc_offset_store(c, pls, pls->active_discontinuity_sequence,
                                              pls->discontinuity_offset);
                    }

                    /* not from a cue: it starts anywhere in its segment,
                     * not at the segment start this offset stands for */
                    if (pls->ts_offset == AV_NOPTS_VALUE && !pls->is_subtitle &&
                        pls->pkt->dts    != AV_NOPTS_VALUE) {
                        int64_t seg_idx = pls->cur_seq_no - pls->start_seq_no;
                        int64_t ts = av_rescale_q(pls->pkt->dts,
                            get_timebase(pls), AV_TIME_BASE_Q);

                        if (pls->has_discontinuity) {
                            /* With discontinuities first_timestamp stands
                             * for the first segment of the window for a live
                             * window too (parse_playlist() moves it along as
                             * segments expire, find_timestamp_in_playlist()
                             * counts from there): count back from the first
                             * segment of the region, cur_seq_no may be past
                             * it already with small segments. */
                            seg_idx = pls->discontinuity_start_seq_no - pls->start_seq_no;
                            if (!pls->finished)
                                for (int64_t k = 0; k < seg_idx && k < pls->n_segments; k++)
                                    ts -= pls->segments[k]->duration;
                        } else if (pls->type == PLS_TYPE_EVENT)
                            /* EVENT playlists preserve all segments from the start */
                            for (int64_t k = 0; k < seg_idx && k < pls->n_segments; k++)
                                ts -= pls->segments[k]->duration;

                        if (c->first_timestamp == AV_NOPTS_VALUE) {
                            c->first_timestamp = ts;
                            c->first_timestamp_pls = pls;

                            if (pls->type == PLS_TYPE_EVENT)
                                for (unsigned k = 0; k < s->nb_streams; k++) {
                                    AVStream *st = s->streams[k];
                                    if (st->start_time == AV_NOPTS_VALUE)
                                        st->start_time = av_rescale_q(ts,
                                            AV_TIME_BASE_Q, st->time_base);
                                }
                        }
                        pls->ts_offset = ts - c->first_timestamp;
                    }
                }

                seg = current_segment(pls);
                if (seg && seg->key_type == KEY_SAMPLE_AES && !strstr(pls->ctx->iformat->name, "mov")) {
                    enum AVCodecID codec_id = pls->ctx->streams[pls->pkt->stream_index]->codecpar->codec_id;
                    memcpy(c->crypto_ctx.iv, seg->iv, sizeof(seg->iv));
                    memcpy(c->crypto_ctx.key, pls->key, sizeof(pls->key));
                    ff_hls_senc_decrypt_frame(codec_id, &c->crypto_ctx, pls->pkt);
                }

                if (pls->seek_timestamp == AV_NOPTS_VALUE)
                    break;

                ret = update_streams_from_subdemuxer(s, pls);
                if (ret < 0) {
                    av_packet_unref(pls->pkt);
                    return ret;
                }
                if (pls->seek_stream_index < 0 ||
                    pls->seek_stream_index == pls->main_streams[pls->pkt->stream_index]->index) {

                    if (pls->pkt->dts == AV_NOPTS_VALUE) {
                        pls->seek_timestamp = AV_NOPTS_VALUE;
                        break;
                    }

                    tb = get_timebase(pls);
                    ts_diff = av_rescale_rnd(pls->pkt->dts, AV_TIME_BASE,
                                            tb.den, AV_ROUND_DOWN) -
                            pls->seek_timestamp;
                    /* a cue that began before the target is kept if it is
                     * still on screen there */
                    if (pls->is_subtitle && ts_diff < 0 &&
                        (pls->seek_flags & AVSEEK_FLAG_ANY) &&
                        av_sat_add64(ts_diff, av_rescale_q(pls->pkt->duration, tb,
                                                           AV_TIME_BASE_Q)) > 0)
                        ts_diff = 0;
                    if (ts_diff >= 0 && (pls->seek_flags  & AVSEEK_FLAG_ANY ||
                                        pls->pkt->flags & AV_PKT_FLAG_KEY)) {
                        pls->seek_timestamp = AV_NOPTS_VALUE;
                        break;
                    }
                }
                av_packet_unref(pls->pkt);
            }
        }
        /* Check if this stream has the packet with the lowest dts */
        if (pls->pkt->data) {
            struct playlist *minpls = minplaylist < 0 ?
                                     NULL : c->playlists[minplaylist];
            if (minplaylist < 0) {
                minplaylist = i;
            } else {
                int64_t dts     =    pls->pkt->dts;
                int64_t mindts  = minpls->pkt->dts;

                if (dts == AV_NOPTS_VALUE ||
                    (mindts != AV_NOPTS_VALUE && compare_ts_with_wrapdetect(dts, pls, mindts, minpls) < 0))
                    minplaylist = i;
            }
        }
    }

    /* If we got a packet, return it */
    if (minplaylist >= 0) {
        struct playlist *pls = c->playlists[minplaylist];
        AVStream *ist;
        AVStream *st;

        ret = update_streams_from_subdemuxer(s, pls);
        if (ret < 0) {
            av_packet_unref(pls->pkt);
            return ret;
        }

        // If sub-demuxer reports updated metadata, copy it to the first stream
        // and set its AVSTREAM_EVENT_FLAG_METADATA_UPDATED flag.
        if (pls->ctx->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED) {
            if (pls->n_main_streams) {
                st = pls->main_streams[0];
                av_dict_copy(&st->metadata, pls->ctx->metadata, 0);
                st->event_flags |= AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
            }
            pls->ctx->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
        }

        /* check if noheader flag has been cleared by the subdemuxer */
        if (pls->has_noheader_flag && !(pls->ctx->ctx_flags & AVFMTCTX_NOHEADER)) {
            pls->has_noheader_flag = 0;
            update_noheader_flag(s);
        }

        if (pls->pkt->stream_index >= pls->n_main_streams) {
            av_log(s, AV_LOG_ERROR, "stream index inconsistency: index %d, %d main streams, %d subdemuxer streams\n",
                   pls->pkt->stream_index, pls->n_main_streams, pls->ctx->nb_streams);
            av_packet_unref(pls->pkt);
            return AVERROR_BUG;
        }

        ist = pls->ctx->streams[pls->pkt->stream_index];
        st = pls->main_streams[pls->pkt->stream_index];

        // Propagate timed-ID3 metadata changes to the main stream, suppressing
        // duplicate packets that repeat unchanged metadata at segment boundaries.
        if (ist->codecpar->codec_id == AV_CODEC_ID_TIMED_ID3 &&
            pls->pkt->stream_index == pls->timed_id3_stream_index &&
            ist->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
            ist->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
            if (metadata_has_changed(pls->timed_id3_metadata, ist->metadata)) {
                int ret = av_dict_copy(&st->metadata, ist->metadata, 0);
                if (ret < 0)
                    return ret;
                av_dict_free(&pls->timed_id3_metadata);
                ret = av_dict_copy(&pls->timed_id3_metadata, ist->metadata, 0);
                if (ret < 0)
                    return ret;
                st->event_flags |= AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
            }
        }

        av_packet_move_ref(pkt, pls->pkt);
        pkt->stream_index = st->index;

        /* The playing position: a cue, read up to a segment ahead, is not
         * (a playlist switched in would start a segment late). */
        if (pkt->dts != AV_NOPTS_VALUE && !pls->is_subtitle) {
            c->cur_timestamp = av_rescale_q(pkt->dts,
                                            ist->time_base,
                                            AV_TIME_BASE_Q);
            c->av_timestamp = av_sat_sub64(c->cur_timestamp,
                pls->ts_offset != AV_NOPTS_VALUE ? pls->ts_offset : 0);
        }

        /* There may be more situations where this would be useful, but this at least
         * handles newly probed codecs properly (i.e. request_probe by mpegts). */
        if (ist->codecpar->codec_id != st->codecpar->codec_id ||
            ist->codecpar->codec_type != st->codecpar->codec_type) {
            ret = set_stream_info_from_input_stream(st, pls, ist);
            if (ret < 0) {
                return ret;
            }
        }

        return 0;
    }
    return AVERROR_EOF;
}

static int hls_read_seek(AVFormatContext *s, int stream_index,
                               int64_t timestamp, int flags)
{
    HLSContext *c = s->priv_data;
    struct playlist *seek_pls = NULL;
    int i, j;
    int64_t first_timestamp, seek_timestamp, duration;
    int64_t seq_no, seg_start_ts;

    if ((flags & AVSEEK_FLAG_BYTE) || (c->ctx->ctx_flags & AVFMTCTX_UNSEEKABLE))
        return AVERROR(ENOSYS);

    first_timestamp = c->first_timestamp == AV_NOPTS_VALUE ?
                      0 : c->first_timestamp;

    seek_timestamp = av_rescale_rnd(timestamp, AV_TIME_BASE,
                                    s->streams[stream_index]->time_base.den,
                                    AV_ROUND_DOWN);

    duration = s->duration == AV_NOPTS_VALUE ?
               0 : s->duration;

    if (0 < duration && duration < seek_timestamp - first_timestamp)
        return AVERROR(EIO);

    /* find the playlist with the specified stream */
    for (i = 0; i < c->n_playlists; i++) {
        struct playlist *pls = c->playlists[i];
        for (j = 0; j < pls->n_associated_streams; j++) {
            if (pls->associated_streams[j].stream == s->streams[stream_index]) {
                seek_pls = pls;
                break;
            }
        }
    }
    /* check if the timestamp is valid for the playlist with the
     * specified stream index */
    if (!seek_pls || !find_timestamp_in_playlist(c, seek_pls, seek_timestamp, &seq_no, &seg_start_ts))
        return AVERROR(EIO);

    if (s->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        flags & AVSEEK_FLAG_BACKWARD && !(flags & AVSEEK_FLAG_ANY)) {
        /* Seeking to start of segment ensures we seek to a keyframe located
         * before the given timestamp. */
        seek_timestamp = seg_start_ts;
    }

    /* set segment now so we do not need to search again below */
    seek_pls->cur_seq_no = seq_no;
    seek_pls->seek_stream_index = stream_index;

    /* Reset PTS wrap detection so backward seeks don't get misinterpreted
     * as a forward PTS wrap. */
    for (i = 0; i < (int)s->nb_streams; i++)
        ffstream(s->streams[i])->pts_wrap_reference = AV_NOPTS_VALUE;

    for (i = 0; i < c->n_playlists; i++) {
        /* Reset reading */
        struct playlist *pls = c->playlists[i];

        /* nothing is read from these, and they have no current segment */
        if (pls->excluded || pls->broken || !pls->n_segments)
            continue;

        reset_playlist(pls);

        /* The discard in hls_read_packet compares this playlist's packet DTS
         * against seek_timestamp, but seek_timestamp is on c->first_timestamp's
         * baseline (whichever stream produced the first packet). Streams can
         * have different DTS baselines, so translate the threshold onto this
         * playlist's own baseline using its captured offset. */
        if (pls->ts_offset != AV_NOPTS_VALUE)
            pls->seek_timestamp = seek_timestamp + pls->ts_offset;
        else
            pls->seek_timestamp = seek_timestamp;
        pls->seek_flags = flags;

        if (pls != seek_pls) {
            /* set closest segment seq_no for playlists not handled above */
            find_timestamp_in_playlist(c, pls, seek_timestamp, &pls->cur_seq_no, NULL);
            /* seek the playlist to the given position without taking
             * keyframes into account since this playlist does not have the
             * specified stream where we should look for the keyframes */
            pls->seek_stream_index = -1;
            pls->seek_flags |= AVSEEK_FLAG_ANY;
        }

        pls->last_seq_no = pls->cur_seq_no;
        if (pls->is_subtitle)
            pls->m3u8_hold_counters = 0;

        /* the seek restarted the segment demuxer, continue the timeline
         * from the region of the segment seeked to */
        discontinuity_select(c, pls, 1);
    }

    c->cur_timestamp = seek_timestamp;
    /* seek_timestamp is on the timeline of c->first_timestamp */
    c->av_timestamp  = seek_timestamp;

    return 0;
}

static int hls_probe(const AVProbeData *p)
{
    /* Require #EXTM3U at the start, and either one of the ones below
     * somewhere for a proper match. */
    if (strncmp(p->buf, "#EXTM3U", 7))
        return 0;

    if (strstr(p->buf, "#EXT-X-STREAM-INF:")     ||
        strstr(p->buf, "#EXT-X-TARGETDURATION:") ||
        strstr(p->buf, "#EXT-X-MEDIA-SEQUENCE:")) {

        int mime_ok = p->mime_type && !(
            av_strcasecmp(p->mime_type, "application/vnd.apple.mpegurl") &&
            av_strcasecmp(p->mime_type, "audio/mpegurl")
            );

        int mime_x = p->mime_type && !(
            av_strcasecmp(p->mime_type, "audio/x-mpegurl") &&
            av_strcasecmp(p->mime_type, "application/x-mpegurl")
            );

        if (!mime_ok &&
            !mime_x &&
            !av_match_ext    (p->filename, "m3u8,m3u") &&
             ff_match_url_ext(p->filename, "m3u8,m3u") <= 0) {
            av_log(NULL, AV_LOG_ERROR, "Not detecting m3u8/hls with non standard extension and non standard mime type\n");
            return 0;
        }
        if (mime_x)
            av_log(NULL, AV_LOG_WARNING, "mime type is not rfc8216 compliant\n");

        return AVPROBE_SCORE_MAX;
    }
    return 0;
}

#define OFFSET(x) offsetof(HLSContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption hls_options[] = {
    {"hls_quality_mode", "Preselect one variant: 0=all, 1=lowest bandwidth, 2=highest bandwidth",
        OFFSET(hls_quality_mode), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 2, FLAGS},
    {"hls_audio_default_only", "Only open default audio renditions when a group has a default",
        OFFSET(hls_audio_default_only), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS},
    {"live_start_index", "segment index to start live streams at (negative values are from the end)",
        OFFSET(live_start_index), AV_OPT_TYPE_INT, {.i64 = LIVE_START_INDEX_AUTO}, INT_MIN, INT_MAX, FLAGS, .unit = "live_start_index"},
    {"auto", "-3, the first segment for EVENT playlists", 0, AV_OPT_TYPE_CONST, {.i64 = LIVE_START_INDEX_AUTO}, 0, 0, FLAGS, .unit = "live_start_index"},
    {"prefer_x_start", "prefer to use #EXT-X-START if it's in playlist instead of live_start_index",
        OFFSET(prefer_x_start), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS},
    {"allowed_extensions", "List of file extensions that hls is allowed to access",
        OFFSET(allowed_extensions), AV_OPT_TYPE_STRING,
        {.str = "3gp,aac,avi,ac3,eac3,flac,mkv,m3u8,m4a,m4s,m4v,mpg,mov,mp2,mp3,mp4,mpeg,mpegts,ogg,ogv,oga,ts,vob,vtt,wav,webvtt"
            ",cmfv,cmfa" // Ticket11526 www.nicovideo.jp
            ",ec3"       // part of Ticket11435 (Elisa Viihde (Finnish online recording service))
            ",fmp4"      // https://github.com/yt-dlp/yt-dlp/issues/12700
        },
        INT_MIN, INT_MAX, FLAGS},
    {"allowed_segment_extensions", "List of file extensions that hls is allowed to access",
        OFFSET(allowed_segment_extensions), AV_OPT_TYPE_STRING,
        {.str = "3gp,aac,avi,ac3,eac3,flac,mkv,m3u8,m4a,m4s,m4v,mpg,mov,mp2,mp3,mp4,mpeg,mpegts,ogg,ogv,oga,ts,vob,vtt,wav,webvtt"
            ",cmfv,cmfa" // Ticket11526 www.nicovideo.jp
            ",ec3"       // part of Ticket11435 (Elisa Viihde (Finnish online recording service))
            ",fmp4"      // https://github.com/yt-dlp/yt-dlp/issues/12700
            ",html"      // https://flash1.bogulus.cfd/
        },
        INT_MIN, INT_MAX, FLAGS},
    {"extension_picky", "Be picky with all extensions matching",
        OFFSET(extension_picky), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, FLAGS},
    {"skip_image_head", "Read segments that start with a PNG, JPEG, GIF or WebP image from the MPEG-TS data behind it",
        OFFSET(skip_image_head), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, FLAGS},
    {"max_reload", "Maximum number of times a insufficient list is attempted to be reloaded",
        OFFSET(max_reload), AV_OPT_TYPE_INT, {.i64 = 100}, 0, INT_MAX, FLAGS},
    {"key_uri_old", "allow to replace part of AES key uri - old", OFFSET(key_uri_replace_old), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS},
    {"key_uri_new", "allow to replace part of AES key uri - new", OFFSET(key_uri_replace_new), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS},
    {"m3u8_hold_counters", "The maximum number of times to load m3u8 when it refreshes without new segments",
        OFFSET(m3u8_hold_counters), AV_OPT_TYPE_INT, {.i64 = 1000}, 0, INT_MAX, FLAGS},
    {"http_persistent", "Use persistent HTTP connections",
        OFFSET(http_persistent), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, FLAGS },
    {"http_multiple", "Use multiple HTTP connections for fetching segments",
        OFFSET(http_multiple), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, FLAGS},
    {"http_seekable", "Use HTTP partial requests, 0 = disable, 1 = enable, -1 = auto",
        OFFSET(http_seekable), AV_OPT_TYPE_BOOL, { .i64 = -1}, -1, 1, FLAGS},
    {"cenc_decryption_key_url", "URL that is opened for the key of a KID of fMP4 segments in CENC ({kid}, {type}, {pssh}; see decryption_key_url of the mov demuxer)",
        OFFSET(cenc_decryption_key_url), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},
    {"seg_format_options", "Set options for segment demuxer",
        OFFSET(seg_format_opts), AV_OPT_TYPE_DICT, {.str = NULL}, 0, 0, FLAGS},
    {"seg_max_retry", "Maximum number of times to reload a segment on error.",
     OFFSET(seg_max_retry), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {NULL}
};

static const AVClass hls_class = {
    .class_name = "hls demuxer",
    .item_name  = av_default_item_name,
    .option     = hls_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_hls_demuxer = {
    .p.name         = "hls",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Apple HTTP Live Streaming"),
    .p.priv_class   = &hls_class,
    .p.flags        = AVFMT_NOGENSEARCH | AVFMT_TS_DISCONT | AVFMT_NO_BYTE_SEEK | AVFMT_SHOW_IDS,
    .priv_data_size = sizeof(HLSContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = hls_probe,
    .read_header    = hls_read_header,
    .read_packet    = hls_read_packet,
    .read_close     = hls_close,
    .read_seek      = hls_read_seek,
};
