"""Generated mixed-TS HLS fixtures and loopback audio-switch regression."""

import argparse
import http.server
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import threading


def run(*arguments):
    result = subprocess.run(arguments, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors="replace") +
                           result.stderr.decode(errors="replace"))
    return result.stdout.decode()


def generate(root, ffmpeg):
    video = root / "video.raw"
    video.write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25))
    for track, frequency in enumerate((400, 800, 1200)):
        audio = root / f"audio{track}.pcm"
        cycle = b"".join(struct.pack("<hh", value, value)
                         for i in range(48000)
                         for value in [int(6000 * math.sin(2 * math.pi * frequency * i / 48000))])
        audio.write_bytes(cycle * 40)
        inputs = ("-stream_loop", "-1", "-f", "rawvideo", "-pixel_format",
                  "yuv420p", "-video_size", "64x64", "-i", str(video)) if track == 0 else ()
        run(ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
            "-i", str(audio), *inputs, "-map", "0:a",
            *(("-map", "1:v", "-c:v", "mpeg2video", "-g", "25", "-bf", "0")
              if track == 0 else ()), "-t", "40", "-c:a", "aac",
            "-b:a", "96k", "-f", "hls", "-hls_time", "2",
            "-hls_list_size", "0", "-hls_segment_filename",
            str(root / f"track{track}-%03d.ts"), str(root / f"track{track}.m3u8"))
        playlist = root / f"track{track}.m3u8"
        playlist.write_text(playlist.read_text().replace(
            "#EXT-X-MEDIA-SEQUENCE:0", f"#EXT-X-MEDIA-SEQUENCE:{track * 100}"),
            encoding="ascii")
    for live in (False, True):
        suffix = "-live" if live else ""
        (root / f"master{suffix}.m3u8").write_text(
            '#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="embedded",'
            'DEFAULT=YES,AUTOSELECT=YES,LANGUAGE="sk"\n'
            + "".join(f'#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="audio{i}",'
                      f'DEFAULT=NO,AUTOSELECT=YES,LANGUAGE="{("en", "de")[i - 1]}",'
                      f'URI="track{i}{suffix}.m3u8"\n'
                      for i in (1, 2))
            + '#EXT-X-STREAM-INF:BANDWIDTH=500000,AUDIO="a"\n'
            f'track0{suffix}.m3u8\n', encoding="ascii")


class HlsHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        root = self.server.root
        path = self.path.lstrip("/")
        match = re.fullmatch(r"track\d-(\d+)\.ts", path)
        if match:
            self.server.latest_segment = max(self.server.latest_segment, int(match[1]))
        if path.endswith("-live.m3u8") and path.startswith("track"):
            original = (root / path.replace("-live", "")).read_text()
            sequence = int(re.search(r"#EXT-X-MEDIA-SEQUENCE:(\d+)", original)[1])
            segments = re.findall(r"(#EXTINF:[^\n]+\n[^\n]+\n)", original)
            # Advance a shared live window as media is consumed, independently
            # of how quickly the native demuxer reads it.
            start = max(0, self.server.latest_segment - 3)
            self.server.windows.append(start)
            data = ("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:3\n"
                    f"#EXT-X-MEDIA-SEQUENCE:{sequence + start}\n"
                    + "".join(segments[start:start + 5])).encode()
        else:
            file = root / path
            if not file.is_file():
                self.send_error(404)
                return
            data = file.read_bytes()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


def serve(root):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HlsHandler)
    server.root = root
    server.latest_segment = 0
    server.windows = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--driver", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-http-") as directory:
        root = Path(directory)
        generate(root, args.ffmpeg)
        server, thread = serve(root)
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            for live in (False, True):
                for initial in (0, 1):
                    server.latest_segment = 0
                    server.windows.clear()
                    suffix = "-live" if live else ""
                    output = run(args.driver, base + f"/master{suffix}.m3u8",
                                 str(initial), str(int(live)))
                    starts = re.findall(r"stage=(\d+) type=(\d+) first_us=(-?\d+) target_us=(-?\d+)",
                                        output)
                    assert len(starts) == 14, output
                    for stage, kind, first, target in starts:
                        assert abs(int(first) - int(target)) < 2100000, output
                    if live:
                        assert max(server.windows) > 0, server.windows
                    print(f"PASS mixed-TS HLS live={live} initial={initial}: "
                          "seven selections with selected audio and video progress")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
