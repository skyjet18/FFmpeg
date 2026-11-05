"""Loopback tests for DASH recovery/startup/ranges and HLS discontinuities."""

import argparse
from collections import Counter
import http.server
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET


def run(*arguments):
    result = subprocess.run(arguments, capture_output=True, timeout=20)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--driver", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ffmpeg-dash-http-") as directory:
        root = Path(directory)
        audio = root / "input.pcm"
        audio.write_bytes(struct.pack("<hh", 4000, -4000) * 48000 * 8)
        run(args.ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
            "-i", str(audio), "-map", "0:a", "-map", "0:a", "-c:a", "aac",
            "-b:a:0", "64k", "-b:a:1", "96k", "-seg_duration", "1",
            "-use_template", "1", "-use_timeline", "1", "-adaptation_sets",
            "id=0,streams=0,1", "-f", "dash", str(root / "static.mpd"))
        run(args.ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
            "-i", str(audio), "-t", "3", "-c:a", "aac", "-seg_duration", "1",
            "-single_file", "1", "-f", "dash", str(root / "range.mpd"))
        video = root / "video.raw"
        video.write_bytes(bytes([96]) * (16 * 16 * 3 // 2 * 25))
        raw_video = ("-f", "rawvideo", "-pixel_format", "yuv420p",
                     "-video_size", "16x16", "-i", str(video))
        run(args.ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
            "-i", str(audio), *raw_video, "-map", "0:a", "-map", "1:v",
            "-t", "1", "-c:a", "aac", "-c:v", "mpeg2video",
            "-streamid", "0:256", "-streamid", "1:257", "-f", "mpegts", str(root / "av.ts"))
        run(args.ffmpeg, "-y", *raw_video, "-c:v", "mpeg2video",
            "-streamid", "0:257", "-f", "mpegts", str(root / "ad.ts"))
        (root / "transition.m3u8").write_text(
            "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:1,\nav.ts\n"
            "#EXT-X-DISCONTINUITY\n#EXTINF:1,\nad.ts\n"
            "#EXT-X-DISCONTINUITY\n#EXTINF:1,\nav.ts\n#EXT-X-ENDLIST\n",
            encoding="ascii")
        ns = {"d": "urn:mpeg:dash:schema:mpd:2011"}
        ET.register_namespace("", ns["d"])
        mpd = ET.parse(root / "static.mpd").getroot()
        mpd.set("type", "dynamic")
        mpd.set("minimumUpdatePeriod", "PT0S")
        mpd.set("suggestedPresentationDelay", "PT3S")
        mpd.attrib.pop("mediaPresentationDuration", None)
        initial_mpd = ET.fromstring(ET.tostring(mpd))
        for timeline in initial_mpd.findall(".//d:SegmentTimeline", ns):
            assert len(timeline) == 2, ET.tostring(initial_mpd)
            timeline[1].set("r", "1")
        initial = ET.tostring(initial_mpd)
        for template in mpd.findall(".//d:SegmentTemplate", ns):
            timeline = template.find("d:SegmentTimeline", ns)
            first = timeline[0]
            start = int(first.get("t", "0"))
            duration = int(first.get("d"))
            repeat = int(first.get("r", "0"))
            if repeat:
                first.set("t", str(start + duration))
                first.set("r", str(repeat - 1))
            else:
                timeline.remove(first)
                timeline[0].set("t", str(start + duration))
            template.set("startNumber", str(int(template.get("startNumber", "1")) + 1))
        rotated = ET.tostring(mpd)
        attempts = Counter()
        requests = []
        slow_representation = False
        flaky_fragment = None

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                nonlocal flaky_fragment
                attempts[self.path] += 1
                requests.append((self.path, self.headers.get("Range")))
                status, payload = 200, b"fragment"
                if self.path.startswith("/late"):
                    status = 404 if attempts[self.path] < 3 else 200
                elif self.path.startswith("/missing"):
                    status = 404
                elif self.path.startswith("/forbidden"):
                    status = 403
                elif self.path == "/manifest.mpd":
                    payload = initial if attempts[self.path] == 1 else rotated
                else:
                    if slow_representation and self.path == "/init-stream1.m4s":
                        time.sleep(1.1)
                    file = root / self.path.lstrip("/")
                    if file.exists():
                        payload = file.read_bytes()
                    else:
                        status = 404
                    if slow_representation and self.path.startswith("/chunk-stream0-"):
                        if flaky_fragment is None:
                            flaky_fragment = self.path
                        if self.path == flaky_fragment and attempts[self.path] < 3:
                            status = 404
                range_header = self.headers.get("Range")
                total = len(payload)
                if status == 200 and range_header:
                    match = re.fullmatch(r"bytes=(\d+)-(\d*)", range_header)
                    assert match, range_header
                    start = int(match[1])
                    end = min(int(match[2]) if match[2] else total - 1, total - 1)
                    payload = payload[start:end + 1]
                    status = 206
                self.send_response(status)
                if status == 206:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def handle(self):
                try:
                    super().handle()
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            for path, live, cancel, expected, success in (
                ("/late.m4s", 1, 0, 3, True),
                ("/missing.m4s", 1, 0, 3, False),
                ("/forbidden.m4s", 1, 0, 1, False),
                ("/missing-vod.m4s", 0, 0, 1, False),
                ("/missing-cancel.m4s", 1, 40, 1, False),
            ):
                output = run(args.driver, "fragment", base + path, str(live), str(cancel))
                match = re.search(r"result=(-?\d+) elapsed=(\d+)", output)
                result, elapsed = map(int, match.groups())
                assert (result >= 0) == success, output
                assert attempts[path] == expected, (path, attempts, output)
                if cancel:
                    assert elapsed < 250000, output
                elif expected == 3:
                    assert 800000 <= elapsed < 3000000, output
            print("PASS actual HTTP retries: late/exhausted/forbidden/VOD/cancel and delay bounds")
            slow_representation = True
            output = run(args.driver, "manifest", base + "/manifest.mpd")
            packets = re.findall(r"packet=(\d+) type=(\d+) pts=(-?\d+)", output)
            first_pts = {}
            for stream, kind, pts in packets:
                first_pts.setdefault(stream, int(pts))
            assert len(first_pts) == 2, output
            assert max(first_pts.values()) - min(first_pts.values()) < 1024, first_pts
            assert attempts["/manifest.mpd"] >= 2, requests
            assert flaky_fragment and attempts[flaky_fragment] == 3, requests
            assert any(re.search(r"chunk-stream[01]-0000[4-8]", path) for path, _ in requests), requests
            assert all(header is None for path, header in requests if path.endswith(".m4s")), requests
            print("PASS startup manifest rotation: both representations progress with shared media origin")
            print("PASS standalone DASH segments: no unnecessary HTTP Range requests")
            output = run(args.driver, "manifest", base + "/range.mpd")
            assert "packet=" in output, output
            ranges = [header for path, header in requests if path.endswith("-stream0.mp4")]
            assert len(ranges) >= 3 and all(ranges), requests
            assert len(set(ranges)) >= 3, ranges
            print("PASS single-file DASH: distinct initialization and media byte ranges")
            output = run(args.driver, "manifest", base + "/transition.m3u8")
            packets = re.findall(r"packet=(\d+) type=(\d+) pts=(-?\d+)", output)
            audio_streams = {stream for stream, kind, _ in packets if kind == "1"}
            video_streams = {stream for stream, kind, _ in packets if kind == "0"}
            assert len(audio_streams) == len(video_streams) == 1, output
            assert audio_streams.isdisjoint(video_streams), output
            assert attempts["/ad.ts"] and attempts["/av.ts"] >= 2, requests
            assert output.count(f"packet={next(iter(audio_streams))} type=1") > 60, output
            print("PASS HLS A/V -> video-only ad -> A/V: distinct stable slots and returning audio")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
