/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/bswap.h"
#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavformat/avformat.h"

typedef struct Reader {
    uint8_t *data;
    int size, pos;
} Reader;

static int read_packet(void *opaque, uint8_t *buf, int size)
{
    Reader *r = opaque;
    size = FFMIN(size, r->size - r->pos);
    if (!size)
        return AVERROR_EOF;
    memcpy(buf, r->data + r->pos, size);
    r->pos += size;
    return size;
}

/* The muxer announces E-AC-3 twice: by the stream type and by a registration
 * descriptor "EAC3". Broadcasts and many packagers send the stream type
 * alone: overwrite the format identifier of every PMT so that only the
 * stream type is left, and return that type. */
static int strip_registration(uint8_t *data, int size)
{
    static const uint8_t reg[] = { 0x05, 0x04, 'E', 'A', 'C', '3' };
    int stream_type = -1, patched = 0;

    for (int off = 0; off + 188 <= size; off += 188) {
        uint8_t *p = data + off, *section, *d = NULL;
        int len;

        /* payload unit start, no adaptation field, pointer field 0, PMT */
        if (p[0] != 0x47 || !(p[1] & 0x40) || (p[3] & 0x30) != 0x10 ||
            p[4] != 0 || p[5] != 0x02)
            continue;
        section = p + 5;
        len = 3 + (AV_RB16(section + 1) & 0xfff);
        av_assert0(len <= 183 && len > 16);
        for (int i = 12; i + (int)sizeof(reg) <= len - 4; i++)
            if (!memcmp(section + i, reg, sizeof(reg))) {
                d = section + i;
                break;
            }
        av_assert0(d);
        /* the only elementary stream entry follows the program info */
        stream_type = section[12 + (AV_RB16(section + 10) & 0xfff)];
        memcpy(d + 2, "none", 4);
        AV_WB32(section + len - 4,
                av_bswap32(av_crc(av_crc_get_table(AV_CRC_32_IEEE), -1,
                                  section, len - 4)));
        patched++;
    }
    av_assert0(patched);
    return stream_type;
}

int main(void)
{
    AVFormatContext *out = NULL, *in = avformat_alloc_context();
    AVIOContext *io;
    AVStream *st;
    AVPacket *pkt = av_packet_alloc();
    Reader reader = {0};
    int i, found = 0;

    av_assert0(in && pkt);
    av_assert0(avformat_alloc_output_context2(&out, NULL, "mpegts", NULL) >= 0);
    st = avformat_new_stream(out, NULL);
    av_assert0(st);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_EAC3;
    st->codecpar->sample_rate = 48000;
    av_channel_layout_default(&st->codecpar->ch_layout, 2);
    av_assert0(avio_open_dyn_buf(&out->pb) >= 0);
    av_assert0(avformat_write_header(out, NULL) >= 0);
    av_assert0(av_new_packet(pkt, 256) >= 0);
    memset(pkt->data, 0, pkt->size);
    pkt->data[0] = 0x0b;
    pkt->data[1] = 0x77;
    pkt->data[3] = 127;
    pkt->data[4] = 0x34;
    pkt->data[5] = 0x80;
    for (i = 0; i < 10; i++) {
        pkt->pts = pkt->dts = i * 2880;
        pkt->duration = 2880;
        av_assert0(av_write_frame(out, pkt) >= 0);
    }
    av_assert0(av_write_trailer(out) >= 0);
    reader.size = avio_close_dyn_buf(out->pb, &reader.data);
    out->pb = NULL;
    avformat_free_context(out);
    /* ATSC E-AC-3 */
    av_assert0(strip_registration(reader.data, reader.size) == 0x87);
    io = avio_alloc_context(av_malloc(4096), 4096, 0, &reader, read_packet, NULL, NULL);
    av_assert0(io);
    in->pb = io;
    /* no avformat_find_stream_info(): the codec is known from the PMT */
    av_assert0(avformat_open_input(&in, NULL, av_find_input_format("mpegts"), NULL) >= 0);
    for (i = 0; i < in->nb_streams; i++)
        found |= in->streams[i]->codecpar->codec_id == AV_CODEC_ID_EAC3;
    av_assert0(found);
    avformat_close_input(&in);
    av_freep(&io->buffer);
    avio_context_free(&io);
    av_packet_free(&pkt);
    av_free(reader.data);
    return 0;
}
