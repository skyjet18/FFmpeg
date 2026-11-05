/* This file is part of FFmpeg, licensed under the GNU LGPL version 2.1 or later. */
#include "libavutil/avassert.h"
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
    io = avio_alloc_context(av_malloc(4096), 4096, 0, &reader, read_packet, NULL, NULL);
    av_assert0(io);
    in->pb = io;
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
