"""MPEG-TS demuxer: PES stream_id side data and unchanged packet payloads.

Every packet read from MPEG-TS carries its PES stream_id as
AV_PKT_DATA_MPEGTS_STREAM_ID side data, and the payload is the elementary
stream data alone: the same sizes as the packets the encoder wrote, also
after a TS -> TS remux with -c copy.
"""

import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile

SIDE_DATA = "MPEGTS Stream ID"
DURATION = 4


def run(*arguments):
    result = subprocess.run(arguments, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, arguments)) + "\n" +
                           result.stdout.decode(errors="replace") +
                           result.stderr.decode(errors="replace"))
    return result.stdout.decode()


def generate(root, ffmpeg):
    video = root / "video.raw"
    with video.open("wb") as stream:
        for frame in range(DURATION * 25):
            stream.write(bytes(16 + (pixel * 13 + frame * 7) % 220
                               for pixel in range(64 * 64)))
            stream.write(bytes([128]) * (64 * 64 // 2))
    audio = root / "audio.pcm"
    audio.write_bytes(b"".join(struct.pack("<hh", value, value)
                               for i in range(48000 * DURATION)
                               for value in [int(6000 * math.sin(2 * math.pi * 440 * i / 48000))]))
    reference = root / "reference.nut"
    run(ffmpeg, "-y", "-v", "error",
        "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
        "-framerate", "25", "-i", str(video),
        "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(audio),
        "-map", "0:v", "-map", "1:a",
        "-c:v", "mpeg2video", "-g", "12", "-bf", "2", "-c:a", "ac3", "-b:a", "96k",
        str(reference))
    ts = root / "demux.ts"
    run(ffmpeg, "-y", "-v", "error", "-i", str(reference), "-map", "0",
        "-c", "copy", str(ts))
    remux = root / "remux.ts"
    run(ffmpeg, "-y", "-v", "error", "-i", str(ts), "-map", "0",
        "-c", "copy", str(remux))
    return reference, ts, remux


def packets(ffprobe, path):
    data = json.loads(run(ffprobe, "-v", "error", "-show_streams",
                          "-show_packets", "-of", "json", str(path)))
    types = {stream["index"]: stream["codec_type"] for stream in data["streams"]}
    result = {}
    for packet in data["packets"]:
        side_data = [entry.get("side_data_type")
                     for entry in packet.get("side_data_list", [])]
        result.setdefault(types[packet["stream_index"]], []).append(
            (int(packet["size"]), side_data))
    return result


def check(ffprobe, reference, ts, remux):
    expected = packets(ffprobe, reference)
    assert sorted(expected) == ["audio", "video"], expected.keys()
    for path in (ts, remux):
        got = packets(ffprobe, path)
        assert sorted(got) == sorted(expected), (path.name, got.keys())
        for kind, want in expected.items():
            sizes = [size for size, _ in got[kind]]
            want_sizes = [size for size, _ in want]
            assert len(want_sizes) >= 20, (kind, len(want_sizes))
            # the payload is the elementary stream alone: no side data
            # appended to it, nothing lost
            assert sizes == want_sizes, (path.name, kind, len(sizes), len(want_sizes),
                                         [(i, a, b) for i, (a, b) in
                                          enumerate(zip(sizes, want_sizes)) if a != b][:5])
            # every PES packet exports its stream_id; a frame that the
            # parser hands out only when flushed at the end has none
            missing = sum(SIDE_DATA not in side_data for _, side_data in got[kind])
            assert missing <= 2, (path.name, kind,
                                  f"{missing} of {len(sizes)} packets without {SIDE_DATA!r}")
        print(f"{path.name}: " + ", ".join(
            f"{kind} {len(got[kind])} packets, sizes as encoded, {SIDE_DATA} present"
            for kind in sorted(got)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffprobe",
                        help="default: ffprobe next to the ffmpeg tool")
    args = parser.parse_args()
    ffprobe = args.ffprobe or os.path.join(os.path.dirname(args.ffmpeg) or ".",
                                           "ffprobe" + os.path.splitext(args.ffmpeg)[1])
    with tempfile.TemporaryDirectory(prefix="ffmpeg-mpegts-sid-") as directory:
        check(ffprobe, *generate(Path(directory), args.ffmpeg))
    print("mpegts_stream_id: OK")


if __name__ == "__main__":
    main()
