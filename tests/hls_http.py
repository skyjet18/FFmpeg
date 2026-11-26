"""Generated mixed-TS HLS fixtures and loopback audio-switch regression."""

import argparse
import http.server
import math
import os
from pathlib import Path
import re
import shutil
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import zlib


def run(*arguments):
    result = subprocess.run(arguments, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors="replace") +
                           result.stderr.decode(errors="replace"))
    return result.stdout.decode()


def generate(root, ffmpeg, multiple=False, video_first=False, fragmented=False,
             markers=False, duration=40, audio_offset=0):
    video = root / "video.raw"
    if markers:
        with video.open("wb") as stream:
            for frame in range(duration * 25):
                stream.write(bytes(16 + (pixel * 17 + frame * 11 +
                                        (frame // 20) * (pixel % 7)) % 220
                                   for pixel in range(64 * 64)))
                stream.write(bytes([128]) * (64 * 64 // 2))
    else:
        video.write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25))
    for track, frequency in enumerate((400, 800, 1200)):
        audio = root / f"audio{track}.pcm"
        cycle = b"".join(struct.pack("<hh", value, value)
                         for i in range(48000)
                         for value in [int(6000 * math.sin(2 * math.pi * frequency * i / 48000))])
        if markers:
            with audio.open("wb") as stream:
                for second in range(duration):
                    frequency = 400 + track * 1200 + second * 7
                    stream.write(b"".join(struct.pack("<hh", value, value)
                                         for i in range(48000)
                                         for value in [int(6000 * math.sin(
                                             2 * math.pi * frequency * i / 48000))]))
        else:
            audio.write_bytes(cycle * duration)
        inputs = ("-stream_loop", "-1", "-f", "rawvideo", "-pixel_format",
                  "yuv420p", "-video_size", "64x64", "-i", str(video)) if track == 0 else ()
        extension = "m4s" if fragmented else "ts"
        segment_options = ("-hls_segment_type", "fmp4", "-hls_fmp4_init_filename",
                           f"track{track}-init.mp4") if fragmented else ()
        run(ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
            "-i", str(audio), *inputs,
            *(("-map", "1:v") if track == 0 and video_first else ()),
            "-map", "0:a",
            *(("-c:v", "mpeg2video", "-g", "25", "-bf", "0")
              + (() if video_first else ("-map", "1:v"))
              if track == 0 else ()), "-t", str(duration), "-c:a", "aac",
            "-b:a", "96k", "-f", "hls", "-hls_time", "2",
            "-hls_list_size", "0", *segment_options, "-hls_segment_filename",
            str(root / f"track{track}-%03d.{extension}"),
            "-output_ts_offset", str(audio_offset if track == 2 else 0),
            str(root / f"track{track}.m3u8"))
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
        if multiple:
            master = "#EXTM3U\n"
            for quality in range(3):
                group = f"a{quality}"
                for track, language in enumerate(("sk", "en", "de")):
                    # Preserve the track prefix for the sliding-window handler.
                    name = f"track{track}-quality{quality}{suffix}.m3u8"
                    original = root / f"track{track}.m3u8"
                    (root / name.replace("-live", "")).write_bytes(original.read_bytes())
                    uri = f',URI="{name}"' if track else ""
                    master += (f'#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="{group}",'
                               f'NAME="{language}",LANGUAGE="{language}",'
                               f'DEFAULT={"YES" if track == 0 else "NO"},AUTOSELECT=YES{uri}\n')
                master += (f'#EXT-X-STREAM-INF:BANDWIDTH={500000 * (quality + 1)},'
                           f'AUDIO="{group}"\ntrack0-quality{quality}{suffix}.m3u8\n')
            (root / f"master{suffix}.m3u8").write_text(master, encoding="ascii")


class HlsHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        root = self.server.root
        path = self.path.lstrip("/")
        self.server.requested.append(path)
        unavailable = self.server.unavailable.get(path, 0)
        if unavailable:
            # a count of 503 answers before the file is served, -1: always
            self.server.unavailable[path] = unavailable - (unavailable > 0)
            self.send_error(503)
            return
        match = re.fullmatch(r"track\d-(\d+)\.(?:ts|m4s)", path)
        if match:
            self.server.latest_segment = max(self.server.latest_segment, int(match[1]))
        if path.endswith("-live.m3u8") and path.startswith("track"):
            original = (root / path.replace("-live", "")).read_text()
            sequence = int(re.search(r"#EXT-X-MEDIA-SEQUENCE:(\d+)", original)[1])
            segments = re.findall(r"(#EXTINF:[^\n]+\n[^\n]+\n)", original)
            # Advance a shared live window as media is consumed, independently
            # of how quickly the native demuxer reads it.
            if self.server.realtime:
                start = self.server.initial_window + int(
                    (time.monotonic() - self.server.started) / 2)
            else:
                start = max(0, self.server.latest_segment - 3)
            self.server.windows.append(start)
            version = 6 if "#EXT-X-MAP:" in original else 3
            data = (f"#EXTM3U\n#EXT-X-VERSION:{version}\n#EXT-X-TARGETDURATION:3\n"
                    f"#EXT-X-MEDIA-SEQUENCE:{sequence + start}\n"
                    + "".join(re.findall(r"#EXT-X-MAP:[^\n]+\n", original))
                    + "".join(segments[start:start + 5])).encode()
        else:
            file = root / path.split("?", 1)[0]
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


def serve(root, realtime=False, initial_window=0, tls=None):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HlsHandler)
    if tls:
        server.socket = tls.wrap_socket(server.socket, server_side=True)
    server.root = root
    server.requested = []
    server.unavailable = {}
    server.latest_segment = 0
    server.windows = []
    server.realtime = realtime
    server.initial_window = initial_window
    server.started = time.monotonic()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def references(root, driver, audio_offset_us=None):
    result = {}
    for track in range(3):
        output = run(driver, str(root / f"track{track}.m3u8"))
        for kind, fingerprint, pts in re.findall(
                r"reference type=(\d) hash=([0-9a-f]+) pts_us=(-?\d+)", output):
            key = (track if int(kind) else -1, fingerprint)
            result.setdefault(key, set()).add(int(pts))
    if audio_offset_us is not None:
        starts = [min(timestamp for (source, _), timestamps in result.items()
                      if source == track for timestamp in timestamps)
                  for track in range(3)]
        assert abs(starts[2] - starts[0] - audio_offset_us) <= 1, (starts, audio_offset_us)
    return result


def check_timeline(output, reference, initial):
    counts = [0, 0]
    for stage, kind, fingerprint, pts in re.findall(
            r"packet stage=(\d+) type=(\d) hash=([0-9a-f]+) pts_us=(-?\d+)", output):
        track = (initial + int(stage)) % 3 if int(kind) else -1
        expected = reference[(track, fingerprint)]
        error = min(abs(int(pts) - timestamp) for timestamp in expected)
        assert error <= 100000, (stage, kind, pts, sorted(expected), error)
        counts[int(kind)] += 1
    assert min(counts) > 100, counts


def check_dead_key(ffmpeg):
    """An AES-128 key that cannot be loaded is not requested again for every
    try of every segment: a 404 once, a lasting 503 seg_max_retry + 1 times
    and then only at segments further and further apart. A single 503 is
    still retried and the stream then plays, and after three 503s in a row
    (the limit without seg_max_retry) the next segment asks again and the
    rest of the stream plays."""
    retries = 3
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-dead-key-") as directory:
        root = Path(directory)
        (root / "video.raw").write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25 * 10))
        (root / "audio.pcm").write_bytes(bytes(48000 * 4 * 10))
        (root / "aes.key").write_bytes(bytes(range(16)))
        (root / "keyinfo").write_text(f"aes.key\n{root / 'aes.key'}\n", encoding="ascii")
        run(ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
            "-framerate", "25", "-i", str(root / "video.raw"),
            "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.pcm"),
            "-c:v", "mpeg2video", "-g", "25", "-bf", "0", "-c:a", "aac", "-b:a", "96k",
            "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
            "-hls_key_info_file", str(root / "keyinfo"),
            "-hls_segment_filename", str(root / "aes-%03d.ts"), str(root / "aes.m3u8"))
        playlist = (root / "aes.m3u8").read_text()
        assert 'URI="aes.key"' in playlist, playlist
        segments = {file.name for file in root.glob("aes-*.ts")}
        assert len(segments) >= 8, segments
        for name in ("dead", "busy", "flaky", "outage"):
            (root / f"{name}.m3u8").write_text(
                playlist.replace('URI="aes.key"', f'URI="{name}.key"'), encoding="ascii")
        for name in ("busy", "flaky", "outage"):
            (root / f"{name}.key").write_bytes(bytes(range(16)))

        server, thread = serve(root)
        try:
            for name, unavailable, max_retry, success, key_requests, lost in (
                    ("dead", 0, retries, False, 1, 0),
                    # the limit, then segments 1, 2, 4, 8 ... later
                    ("busy", -1, retries, False, retries + 1 + len(segments).bit_length(), 0),
                    ("flaky", 1, 1, True, 2, 0),
                    # before: segment 4 and the rest skipped without a request
                    ("outage", 3, 0, True, 4, 3)):
                server.requested.clear()
                server.unavailable = {f"{name}.key": unavailable} if unavailable else {}
                result = subprocess.run(
                    [ffmpeg, "-nostdin", "-hide_banner", "-v", "warning",
                     "-seg_max_retry", str(max_retry),
                     "-i", f"http://127.0.0.1:{server.server_port}/{name}.m3u8",
                     "-map", "0", "-c", "copy", "-f", "null", "-"],
                    capture_output=True, timeout=60)
                log = result.stderr.decode(errors="replace")[-4000:]
                count = server.requested.count(f"{name}.key")
                # without the limit: one request per try of every segment
                assert 1 <= count <= key_requests if not success else count == key_requests, (
                    name, count, len(segments), log)
                if success:
                    assert result.returncode == 0, (name, log)
                    played = {segment for segment in segments
                              if int(segment[4:7]) >= lost}
                    assert played <= set(server.requested), (
                        name, sorted(played - set(server.requested)), log)
                else:
                    # the error still ends the open, no segment was read
                    assert result.returncode, (name, log)
                    assert not segments & set(server.requested), (name, server.requested)
                    assert "is not requested again" in log, (name, log)
                answers = {0: "404", -1: "always 503", 1: "one 503",
                           3: "three 503"}[unavailable]
                print(f"PASS HLS AES-128 key {name} ({answers}): "
                      f"{count} key request(s) for {len(segments)} segments")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


def check_key_scheme(ffmpeg):
    """key:// gives the AES-128 key as 32 hex digits. Another length or a
    digit that is not hex fails the segments without reading them, before:
    the segments were decrypted with the key of before (zeros) and handed
    to the demuxer as garbage."""
    key = bytes(range(16))
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-key-scheme-") as directory:
        root = Path(directory)
        (root / "video.raw").write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25 * 4))
        (root / "aes.key").write_bytes(key)
        (root / "keyinfo").write_text(f"aes.key\n{root / 'aes.key'}\n", encoding="ascii")
        run(ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
            "-framerate", "25", "-i", str(root / "video.raw"),
            "-c:v", "mpeg2video", "-g", "25", "-bf", "0",
            "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
            "-hls_key_info_file", str(root / "keyinfo"),
            "-hls_segment_filename", str(root / "aes-%03d.ts"), str(root / "aes.m3u8"))
        playlist = (root / "aes.m3u8").read_text()
        assert 'URI="aes.key"' in playlist, playlist
        segments = {file.name for file in root.glob("aes-*.ts")}
        cases = (("good", key.hex(), True),
                 ("short", key.hex()[:30], False),
                 ("nonhex", key.hex()[:30] + "zz", False))
        for name, digits, _ in cases:
            (root / f"{name}.m3u8").write_text(
                playlist.replace('URI="aes.key"', f'URI="key://{digits}"'), encoding="ascii")

        server, thread = serve(root)
        try:
            for name, digits, valid in cases:
                server.requested.clear()
                result = subprocess.run(
                    [ffmpeg, "-nostdin", "-hide_banner", "-v", "warning",
                     "-i", f"http://127.0.0.1:{server.server_port}/{name}.m3u8",
                     "-map", "0", "-c", "copy", "-f", "framemd5", "-"],
                    capture_output=True, timeout=60)
                log = result.stderr.decode(errors="replace")[-4000:]
                read = segments & set(server.requested)
                if valid:
                    assert result.returncode == 0 and read == segments, (name, server.requested, log)
                    assert result.stdout.count(b"\n0,") >= 25, (name, log)
                else:
                    assert result.returncode, (name, log)
                    assert not read, (name, sorted(read), log)
                    assert b"\n0," not in result.stdout, (name, log)
                    assert "Failed to parse key:// key" in log, (name, log)
                    assert digits not in log, (name, log)
                print(f"PASS HLS AES-128 key:// {name} ({len(digits)} digits): "
                      f"{len(read)} of {len(segments)} segments read")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


def check_sequence_wrap(ffmpeg):
    """A live playlist whose media sequence restarts (1000.. -> 10..) without
    a discontinuity while the next segment is prefetched (http_multiple):
    the resync to the live edge requests the segment it resynced to, before
    the prefetched segment of the old sequence was played in its place."""
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-wrap-") as directory:
        root = Path(directory)
        (root / "video.raw").write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25 * 12))
        run(ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
            "-framerate", "25", "-i", str(root / "video.raw"),
            "-c:v", "mpeg2video", "-g", "25", "-bf", "0",
            "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
            "-hls_segment_filename", str(root / "seg-%03d.ts"), str(root / "seg.m3u8"))
        assert len(list(root.glob("seg-*.ts"))) >= 12, sorted(root.iterdir())

        def playlist(prefix, sequence, finished):
            return ("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    f"#EXT-X-MEDIA-SEQUENCE:{sequence}\n"
                    + "".join(f"#EXTINF:1.000000,\n{prefix}-{sequence + index}.ts\n"
                              for index in range(6))
                    + ("#EXT-X-ENDLIST\n" if finished else "")).encode()

        class WrapHandler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                path = self.path.lstrip("/")
                requested.append(path)
                match = re.fullmatch(r"([ab])-(\d+)\.ts", path)
                if path == "live.m3u8":
                    # the first load: sequence 1000.., every reload: 10.. (end)
                    first = requested.count("live.m3u8") == 1
                    data = playlist("a", 1000, False) if first else playlist("b", 10, True)
                elif match:
                    # slow segments, so that the reload falls between two
                    # segments with the next one already prefetched
                    time.sleep(0.3)
                    index = int(match[2]) - 1000 if match[1] == "a" else int(match[2]) - 10 + 6
                    data = (root / f"seg-{index:03d}.ts").read_bytes()
                else:
                    self.send_error(404)
                    return
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

        requested = []
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), WrapHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run(
                [ffmpeg, "-nostdin", "-hide_banner", "-v", "warning",
                 "-http_multiple", "1", "-live_start_index", "0",
                 "-i", f"http://127.0.0.1:{server.server_port}/live.m3u8",
                 "-map", "0", "-c", "copy", "-f", "null", "-"],
                capture_output=True, timeout=60)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        log = result.stderr.decode(errors="replace")[-4000:]
        assert result.returncode == 0, log
        assert "resyncing to live edge (seq 14)" in log, (requested, log)
        new = [path for path in requested if path.startswith("b-")]
        # before: the prefetched a-10xx segment was played as segment 14 and
        # the first new request was b-15
        assert new[:2] == ["b-14.ts", "b-15.ts"], (requested, log)
        print(f"PASS HLS live sequence wrap 1000 -> 10 with prefetch: resynced to {new[0]}")


def check_event_start(ffmpeg):
    """The first segment read from an unfinished playlist of six 1 s
    segments: an EVENT playlist starts on its first segment, unless
    #EXT-X-START applies (prefer_x_start) or live_start_index is set; a
    live playlist starts three segments from the end or at #EXT-X-START,
    a finished playlist on its first segment. Every later load of the
    playlist is finished, so that the reading ends."""
    cases = [("event", None, (), 0),
             ("event", None, ("-live_start_index", "auto"), 0),
             ("event", -2, (), 0),
             ("event", -2, ("-prefer_x_start", "1"), 4),
             ("event", None, ("-live_start_index", "-3"), 3),
             ("event", None, ("-live_start_index", "1"), 1),
             ("live", None, (), 3),
             ("live", None, ("-live_start_index", "-3"), 3),
             ("live", -2, ("-prefer_x_start", "1"), 4),
             ("vod", None, ("-live_start_index", "-3"), 0)]
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-event-") as directory:
        root = Path(directory)
        (root / "video.raw").write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25 * 6))
        run(ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
            "-framerate", "25", "-i", str(root / "video.raw"),
            "-c:v", "mpeg2video", "-g", "25", "-bf", "0",
            "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
            "-hls_segment_filename", str(root / "seg-%03d.ts"), str(root / "seg.m3u8"))
        assert len(list(root.glob("seg-*.ts"))) >= 6, sorted(root.iterdir())

        def playlist(kind, offset, finished):
            return ("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    "#EXT-X-MEDIA-SEQUENCE:0\n"
                    + ("#EXT-X-PLAYLIST-TYPE:EVENT\n" if kind == "event" else "")
                    + (f"#EXT-X-START:TIME-OFFSET={offset}\n" if offset is not None else "")
                    + "".join(f"#EXTINF:1.000000,\nseg-{index:03d}.ts\n" for index in range(6))
                    + ("#EXT-X-ENDLIST\n" if finished else "")).encode()

        class EventHandler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                path = self.path.lstrip("/")
                requested.append(path)
                match = re.fullmatch(r"(\d+)\.m3u8", path)
                if match:
                    kind, offset, _, _ = cases[int(match[1])]
                    data = playlist(kind, offset, kind == "vod" or requested.count(path) > 1)
                elif re.fullmatch(r"seg-\d+\.ts", path):
                    data = (root / path).read_bytes()
                else:
                    self.send_error(404)
                    return
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

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), EventHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        failed = []
        try:
            for number, (kind, offset, options, expected) in enumerate(cases):
                requested = []
                result = subprocess.run(
                    [ffmpeg, "-nostdin", "-hide_banner", "-v", "warning", *options,
                     "-i", f"http://127.0.0.1:{server.server_port}/{number}.m3u8",
                     "-map", "0", "-c", "copy", "-f", "null", "-"],
                    capture_output=True, timeout=60)
                log = result.stderr.decode(errors="replace")[-4000:]
                segments = [int(path[4:7]) for path in requested if path.startswith("seg-")]
                name = (f"{kind}" + (f" EXT-X-START:TIME-OFFSET={offset}" if offset is not None else "")
                        + (" " + " ".join(options) if options else ""))
                if result.returncode or not segments or segments[0] != expected:
                    print(f"FAIL HLS start of {name}: expected segment {expected}, "
                          f"read {segments[:1]} (exit {result.returncode})")
                    failed.append((name, requested, log))
                else:
                    print(f"PASS HLS start of {name}: segment {expected}")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        assert not failed, failed


def check_tls(ffmpeg):
    """The TLS options of the master playlist reach variant playlists, keys and segments."""
    if not shutil.which("openssl") or not re.search(
            r"^\s*https$", run(ffmpeg, "-hide_banner", "-protocols"), re.M):
        print("SKIP HLS over https: no openssl tool or no https protocol")
        return
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-tls-") as directory:
        root = Path(directory)

        def openssl(*arguments):
            run("openssl", *arguments)

        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                "-subj", "/CN=FFmpeg test CA", "-keyout", str(root / "ca.key"),
                "-out", str(root / "ca.pem"),
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        for serial, (name, usage) in enumerate((("server", "serverAuth"),
                                                ("client", "clientAuth")), 1):
            (root / f"{name}.ext").write_text(
                "basicConstraints=CA:FALSE\nsubjectKeyIdentifier=hash\n"
                "authorityKeyIdentifier=keyid,issuer\n"
                f"extendedKeyUsage={usage}\nsubjectAltName=IP:127.0.0.1\n",
                encoding="ascii")
            openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={name}",
                    "-keyout", str(root / f"{name}.key"), "-out", str(root / f"{name}.csr"))
            openssl("x509", "-req", "-days", "2", "-set_serial", str(serial),
                    "-in", str(root / f"{name}.csr"), "-CA", str(root / "ca.pem"),
                    "-CAkey", str(root / "ca.key"), "-extfile", str(root / f"{name}.ext"),
                    "-out", str(root / f"{name}.pem"))

        (root / "video.raw").write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25 * 6))
        (root / "audio.pcm").write_bytes(bytes(48000 * 4 * 6))
        (root / "aes.key").write_bytes(bytes(range(16)))
        (root / "keyinfo").write_text(f"aes.key\n{root / 'aes.key'}\n", encoding="ascii")
        run(ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
            "-framerate", "25", "-i", str(root / "video.raw"),
            "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.pcm"),
            "-c:v", "mpeg2video", "-g", "25", "-bf", "0", "-c:a", "aac", "-b:a", "96k",
            "-f", "hls", "-hls_time", "2", "-hls_list_size", "0",
            "-hls_key_info_file", str(root / "keyinfo"),
            "-hls_segment_filename", str(root / "tls-%03d.ts"), str(root / "tls.m3u8"))
        assert "#EXT-X-KEY:METHOD=AES-128" in (root / "tls.m3u8").read_text()
        (root / "master.m3u8").write_text(
            "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000\ntls.m3u8\n", encoding="ascii")
        expected = {"master.m3u8", "tls.m3u8", "aes.key"} | {
            file.name for file in root.glob("tls-*.ts")}
        assert len(expected) >= 6, expected
        ca = ("-ca_file", str(root / "ca.pem"))
        client = ("-cert_file", str(root / "client.pem"), "-key_file", str(root / "client.key"))

        for client_auth, options, success in ((False, (), False), (False, ca, True),
                                              (True, ca, False), (True, ca + client, True)):
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(root / "server.pem", root / "server.key")
            if client_auth:
                context.verify_mode = ssl.CERT_REQUIRED
                context.load_verify_locations(root / "ca.pem")
            server, thread = serve(root, tls=context)
            try:
                result = subprocess.run(
                    [ffmpeg, "-nostdin", "-hide_banner", "-v", "warning", *options,
                     "-i", f"https://127.0.0.1:{server.server_port}/master.m3u8",
                     "-map", "0", "-c", "copy", "-f", "null", "-"],
                    capture_output=True, timeout=60)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
            state = (client_auth, [option for option in options if option.startswith("-")])
            log = result.stderr.decode(errors="replace")[-4000:]
            if success:
                assert result.returncode == 0, (state, log)
                assert set(server.requested) == expected, (
                    state, sorted(expected - set(server.requested)), log)
            else:
                # the certificate is checked: the master playlist itself fails
                assert result.returncode and not server.requested, (state, server.requested, log)
            print(f"PASS HLS over https client_auth={client_auth} options={state[1]}: "
                  + ("all playlists, keys and segments read" if success else "refused"))


class SlowHandler(http.server.BaseHTTPRequestHandler):
    """Static files with byte ranges; the first request of a file in
    server.slow is sent at server.slow_rate bytes per second."""
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path.lstrip("/")
        file = self.server.root / path
        if not file.is_file():
            self.send_error(404)
            return
        data = file.read_bytes()
        start = 0
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if match:
            start = int(match[1])
        self.server.requested.append((path, start))
        slow = path in self.server.slow and path not in self.server.trickled
        if slow:
            self.server.trickled.add(path)
        self.send_response(206 if match else 200)
        self.send_header("Accept-Ranges", "bytes")
        if match:
            self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
        self.send_header("Content-Length", str(len(data) - start))
        self.end_headers()
        try:
            if not slow:
                self.wfile.write(data[start:])
                return
            step = self.server.slow_rate // 10
            for offset in range(start, len(data), step):
                self.wfile.write(data[offset:offset + step])
                self.wfile.flush()
                time.sleep(0.1)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


def check_low_speed(ffmpeg):
    """low_speed_limit/low_speed_time end a read from a trickling server:
    a reconnect at the current position with reconnect=1, the next segment
    for HLS (the options reach the segment connections); off by default."""
    with tempfile.TemporaryDirectory(prefix="ffmpeg-http-low-speed-") as directory:
        root = Path(directory)
        (root / "video.raw").write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25 * 8))
        (root / "audio.pcm").write_bytes(b"".join(
            struct.pack("<hh", value, value) for i in range(48000 * 8)
            for value in [int(6000 * math.sin(2 * math.pi * 440 * i / 48000))]))
        source = ("-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
                  "-framerate", "25", "-i", str(root / "video.raw"),
                  "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.pcm"),
                  "-c:v", "mpeg2video", "-g", "25", "-bf", "0", "-c:a", "aac", "-b:a", "128k")
        run(ffmpeg, "-y", *source, "-f", "hls", "-hls_time", "2", "-hls_list_size", "0",
            "-hls_segment_filename", str(root / "seg-%03d.ts"), str(root / "seg.m3u8"))
        # raw PCM for the plain file: its demuxer reads straight through and
        # never seeks, so every request after the first is a reconnect
        rate = 20000
        reference = (root / "audio.pcm").read_bytes()[:rate * 5]
        (root / "slow.pcm").write_bytes(reference)
        size = len(reference)
        raw = ("-f", "s16le", "-ar", "48000", "-ac", "2")

        def fetch(path, slow, *options, output=("-map", "0", "-c", "copy", "-f", "framemd5")):
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
            server.daemon_threads = True
            server.root = root
            server.requested = []
            server.slow = {slow}
            server.trickled = set()
            server.slow_rate = rate
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            started = time.monotonic()
            try:
                result = subprocess.run(
                    [ffmpeg, "-nostdin", "-hide_banner", "-v", "warning", *options,
                     "-i", f"http://127.0.0.1:{server.server_port}/{path}",
                     *output, "-"],
                    capture_output=True, timeout=120)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
            log = result.stderr.decode(errors="replace")
            assert result.returncode == 0, (path, options, log[-4000:])
            return (result.stdout, log, server.requested,
                    time.monotonic() - started)

        low_speed = ("-low_speed_limit", str(3 * rate), "-low_speed_time", "1")
        trickle = size / rate
        copy = ("-c", "copy", "-f", "s16le")

        # off by default: the whole file at the slow rate, one request
        output, log, requested, elapsed = fetch("slow.pcm", "slow.pcm", *raw, output=copy)
        assert "Low speed" not in log, log[-4000:]
        assert requested == [("slow.pcm", 0)], requested
        assert output == reference
        assert elapsed > trickle * 0.8, (elapsed, trickle)
        print(f"PASS low speed off: {size} bytes read at {rate} bytes/s in {elapsed:.1f} s")

        # with reconnect: one reconnect at the current position, the same data
        output, log, requested, elapsed = fetch("slow.pcm", "slow.pcm", "-reconnect", "1",
                                                *low_speed, *raw, output=copy)
        assert log.count("Low speed") == 1, log[-4000:]
        assert len(requested) == 2 and requested[0] == ("slow.pcm", 0), requested
        assert requested[1][0] == "slow.pcm" and 0 < requested[1][1] < size, requested
        assert output == reference
        assert elapsed < trickle * 0.6, (elapsed, trickle)
        print(f"PASS low speed with reconnect: reconnected at {requested[1][1]} "
              f"after {elapsed:.1f} s instead of {trickle:.1f} s")

        # HLS: the options of the playlist reach the segment connections; the
        # slow segment is ended and the next one is read
        slow = "seg-001.ts"
        segments = sorted(file.name for file in root.glob("seg-*.ts"))
        assert len(segments) >= 4 and slow in segments, segments
        trickle = (root / slow).stat().st_size / rate
        output, log, requested, elapsed = fetch("seg.m3u8", slow, *low_speed)
        assert log.count("Low speed") == 1, log[-4000:]
        assert set(segments) <= {path for path, _ in requested}, requested
        assert [path for path, _ in requested].count(slow) == 1, requested
        print(f"PASS low speed in HLS segments: {slow} ended after {elapsed:.1f} s "
              f"({trickle:.1f} s at the slow rate), all segments requested")


def check_cenc(ffmpeg):
    """HLS with CENC (cenc-aes-ctr) encrypted fMP4 segments: the key reaches
    the mov demuxer of the segments with seg_format_options."""
    key, kid = "00112233445566778899aabbccddeeff", "0f0e0d0c0b0a09080706050403020100"
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-cenc-") as directory:
        root = Path(directory)
        (root / "video.raw").write_bytes(b"".join(
            bytes([16 + frame * 3 % 200]) * (64 * 64) + bytes([128]) * (64 * 64 // 2)
            for frame in range(25 * 6)))
        (root / "audio.pcm").write_bytes(b"".join(
            struct.pack("<hh", value, value) for i in range(48000 * 6)
            for value in [int(6000 * math.sin(2 * math.pi * 440 * i / 48000))]))
        run(ffmpeg, "-y", "-f", "rawvideo", "-pixel_format", "yuv420p",
            "-video_size", "64x64", "-framerate", "25", "-i", str(root / "video.raw"),
            "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.pcm"),
            "-c:v", "mpeg4", "-g", "50", "-c:a", "aac", "-b:a", "96k", "-f", "mp4",
            "-movflags", "frag_keyframe+empty_moov+default_base_moof",
            "-encryption_scheme", "cenc-aes-ctr", "-encryption_key", key,
            "-encryption_kid", kid, str(root / "cenc.mp4"))

        # split into an initialization section and one segment per fragment
        data = (root / "cenc.mp4").read_bytes()
        init, segments, position = b"", [], 0
        while position < len(data):
            size, kind = struct.unpack(">I4s", data[position:position + 8])
            if size == 1:
                size = struct.unpack(">Q", data[position + 8:position + 16])[0]
            box = data[position:position + size]
            position += size
            if kind in (b"ftyp", b"moov"):
                init += box
            elif kind == b"moof":
                segments.append(box)
            elif kind == b"mdat":
                segments[-1] += box
        assert b"tenc" in init and len(segments) >= 3, (len(init), len(segments))
        assert all(b"senc" in segment for segment in segments)
        (root / "init.mp4").write_bytes(init)
        playlist = ("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n"
                    "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                    '#EXT-X-KEY:METHOD=SAMPLE-AES-CTR,URI="data:text/plain;base64,AA==",'
                    'KEYFORMAT="urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",'
                    'KEYFORMATVERSIONS="1"\n#EXT-X-MAP:URI="init.mp4"\n')
        for index, segment in enumerate(segments):
            (root / f"cenc-{index}.m4s").write_bytes(segment)
            playlist += f"#EXTINF:2.0,\ncenc-{index}.m4s\n"
        (root / "cenc.m3u8").write_text(playlist + "#EXT-X-ENDLIST\n", encoding="ascii")

        # packet hashes with ffprobe: it lists the packets of encrypted
        # streams too, which ffmpeg cannot set up without decoding them
        ffprobe = str(Path(ffmpeg).with_name("ffprobe" + Path(ffmpeg).suffix))
        if not Path(ffprobe).is_file():
            print("SKIP HLS fMP4 cenc-aes-ctr: no ffprobe next to ffmpeg")
            return

        def hashes(source, *options):
            output = run(ffprobe, "-v", "quiet", *options, "-show_data_hash", "MD5",
                         "-show_entries", "packet=data_hash", "-of", "csv=p=0", source)
            return sorted(line.strip() for line in output.splitlines() if line.strip())

        reference = hashes(str(root / "cenc.mp4"), "-decryption_key", key)
        encrypted = hashes(str(root / "cenc.mp4"))
        assert len(reference) > 200 and len(set(reference) & set(encrypted)) < len(reference) // 10, (
            "the file is not encrypted")
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
        server.daemon_threads = True
        server.root = root
        server.requested = []
        server.slow = set()
        server.trickled = set()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}/cenc.m3u8"
        try:
            assert hashes(url) == encrypted, "HLS without a key"
            result = subprocess.run([ffprobe, "-v", "warning", url], capture_output=True, timeout=60)
            log = result.stderr.decode(errors="replace")
            assert result.returncode == 0 and "Segments are encrypted with SAMPLE-AES-CTR (CENC), " \
                "no decryption_key" in log, log[-4000:]
            assert "key file" not in log, log[-4000:]
            print("PASS HLS fMP4 cenc-aes-ctr without a key: the encrypted packets, one warning")
            for option in (f"decryption_key={key}", f"decryption_keys={kid}={key}"):
                assert hashes(url, "-seg_format_options", option) == reference, option
                print(f"PASS HLS fMP4 cenc-aes-ctr with seg_format_options {option.split('=')[0]}: "
                      f"{len(reference)} packets as decrypted from the file")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        requested = {path for path, _ in server.requested}
        assert {"init.mp4", "cenc.m3u8"} | {f"cenc-{index}.m4s" for index in range(len(segments))} \
            <= requested, requested


def main():
    checks = {"dead_key": check_dead_key, "key_scheme": check_key_scheme,
              "sequence_wrap": check_sequence_wrap, "event_start": check_event_start,
              "tls": check_tls,
              "low_speed": check_low_speed, "cenc": check_cenc,
              "mixed": None, "image_head": check_image_head}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--realtime", action="store_true",
                        help="Clock-driven windows, delayed external reactivation and nonzero initial window")
    parser.add_argument("--only", action="append", choices=checks, metavar="CHECK",
                        help="Run only this check, may be repeated (%(choices)s); "
                             "--realtime runs mixed only")
    args = parser.parse_args()
    for name, check in checks.items():
        if args.only and name not in args.only or args.realtime and name != "mixed":
            continue
        if check:
            check(args.ffmpeg)
        else:
            check_mixed(args)


def check_mixed(args):
    """Mixed-TS HLS master playlists, VOD and live: seven audio/video
    selections each, with the packet timeline against the reference."""
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-http-") as directory:
        root = Path(directory)
        generate(root, args.ffmpeg, markers=True, duration=70 if args.realtime else 40,
                 audio_offset=0.18 if args.realtime else 0)
        # A/V MPEG-TS muxing adds one 25-fps video frame of timestamp delay.
        reference = references(root, args.driver, audio_offset_us=140000 if args.realtime else None)
        server, thread = serve(root, realtime=args.realtime,
                               initial_window=3 if args.realtime else 0)
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            for live in (False, True):
                if args.realtime and not live:
                    continue
                for initial in (0, 1):
                    server.started = time.monotonic()
                    server.latest_segment = 0
                    server.windows.clear()
                    suffix = "-live" if live else ""
                    result = subprocess.run(
                        [args.driver, base + f"/master{suffix}.m3u8",
                         str(initial), str(int(live))],
                        env=dict(os.environ, HLS_TRACE_PACKETS="1",
                                 **({"HLS_PACE": "1"} if args.realtime else {})),
                        capture_output=True, timeout=60)
                    output = result.stdout.decode()
                    assert result.returncode == 0, (
                        output[-4000:], result.stderr.decode()[-4000:])
                    check_timeline(output, reference, initial)
                    starts = re.findall(r"stage=(\d+) type=(\d+) first_us=(-?\d+) target_us=(-?\d+)",
                                        output)
                    assert len(starts) == 14, output
                    for stage, kind, first, target in starts:
                        assert abs(int(first) - int(target)) < (250000 if live else 2100000), (
                            stage, kind, first, target)
                    if live:
                        assert max(server.windows) > 0, server.windows
                    print(f"PASS mixed-TS HLS live={live} initial={initial}: "
                          "seven selections with selected audio and video progress")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


def image_heads():
    """Pictures as servers put them in front of MPEG-TS segments, each with
    0x47 bytes 188 apart four times in its data (one packet short of the run
    that marks the TS start)."""
    def decoy(size):
        data = bytearray((i * 7 + 3) % 256 for i in range(size))
        data = bytearray(0x46 if b == 0x47 else b for b in data)
        for k in range(4):
            data[100 + k * 188] = 0x47
        return bytes(data)

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0))
           + chunk(b"tEXt", b"Comment\0" + decoy(1500))
           + chunk(b"IDAT", zlib.compress(b"".join(b"\0" + b"\x80" * 48 for _ in range(16))))
           + chunk(b"IEND", b""))
    payload = decoy(1501) + b"\0"
    webp = (b"RIFF" + struct.pack("<I", 4 + 8 + len(payload)) + b"WEBP"
            + b"VP8L" + struct.pack("<I", 1501) + payload)
    filler = decoy(40000)
    jpeg = (b"\xff\xd8\xff\xe0" + struct.pack(">H", 16) + b"JFIF\0\x01\x01\0\0\x01\0\x01\0\0"
            + b"\xff\xfe" + struct.pack(">H", len(filler) + 2) + filler + b"\xff\xd9")
    return {"PNG": png, "WebP": webp, "JPEG": jpeg}


def check_image_head(ffmpeg):
    """Segments with foreign names and a picture in front of the MPEG-TS data
    are read from the TS data on (skip_image_head), plain and AES-128; the
    packets are those of the same segments without the pictures. Plain TS and
    fMP4 segments are read unchanged, and without the option the pictures
    are not skipped."""
    with tempfile.TemporaryDirectory(prefix="ffmpeg-hls-image-head-") as directory:
        root = Path(directory)
        (root / "video.raw").write_bytes(bytes([96]) * (64 * 64 * 3 // 2 * 25 * 6))
        (root / "audio.pcm").write_bytes(bytes(48000 * 4 * 6))
        sources = ("-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "64x64",
                   "-framerate", "25", "-i", str(root / "video.raw"),
                   "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.pcm"),
                   "-c:v", "mpeg2video", "-g", "25", "-bf", "0", "-c:a", "aac", "-b:a", "96k",
                   "-f", "hls", "-hls_time", "2", "-hls_list_size", "0")
        run(ffmpeg, "-y", *sources, "-hls_segment_filename", str(root / "plain-%03d.ts"),
            str(root / "plain.m3u8"))
        run(ffmpeg, "-y", *sources, "-hls_segment_type", "fmp4",
            "-hls_fmp4_init_filename", "fmp4-init.mp4",
            "-hls_segment_filename", str(root / "fmp4-%03d.m4s"), str(root / "fmp4.m3u8"))
        playlist = (root / "plain.m3u8").read_text()
        segments = re.findall(r"^(plain-\d+\.ts)$", playlist, re.M)
        assert len(segments) >= 3, playlist
        heads = image_heads()
        key, iv = bytes(range(16)), bytes(range(16, 32))
        (root / "aes.key").write_bytes(key)
        openssl = shutil.which("openssl")
        variants = [("js", "PNG", "seg{}.js", False),
                    ("noext", "WebP", "s/{}?token=0123abcd", False),
                    ("jpeg", "JPEG", "video{}.jpeg", False)]
        if openssl:
            variants.append(("aes", "PNG", "img{}.png", True))
        else:
            print("SKIP image head with AES-128: no openssl tool")
        (root / "s").mkdir()
        for name, kind, pattern, encrypted in variants:
            text = playlist
            if encrypted:
                text = text.replace("#EXTINF", f'#EXT-X-KEY:METHOD=AES-128,URI="aes.key",'
                                               f'IV=0x{iv.hex()}\n#EXTINF', 1)
            for number, segment in enumerate(segments):
                uri = pattern.format(f"{name}{number}")
                data = heads[kind] + (root / segment).read_bytes()
                file = root / uri.split("?", 1)[0]
                if encrypted:
                    file.with_suffix(".plain").write_bytes(data)
                    run(openssl, "enc", "-aes-128-cbc", "-K", key.hex(), "-iv", iv.hex(),
                        "-in", str(file.with_suffix(".plain")), "-out", str(file))
                else:
                    file.write_bytes(data)
                text = re.sub(rf"^{re.escape(segment)}$", lambda _: uri, text, flags=re.M)
            (root / f"{name}.m3u8").write_text(text, encoding="ascii")

        server, thread = serve(root)
        base = f"http://127.0.0.1:{server.server_port}"

        def packets(name, *options):
            result = subprocess.run(
                [ffmpeg, "-nostdin", "-hide_banner", "-v", "info", "-extension_picky", "0",
                 *options, "-i", f"{base}/{name}.m3u8", "-map", "0", "-c", "copy",
                 "-f", "framecrc", "-"], capture_output=True, timeout=60)
            lines = [line for line in result.stdout.decode().splitlines()
                     if line and not line.startswith("#")]
            return result.returncode, lines, result.stderr.decode(errors="replace")

        try:
            code, reference, log = packets("plain")
            assert code == 0 and len(reference) > 100, (code, log[-4000:])
            assert "Skipped a" not in log, log[-4000:]
            for name, kind, pattern, encrypted in variants:
                code, lines, log = packets(name)
                assert code == 0 and lines == reference, (name, code, len(lines), log[-4000:])
                skipped = re.findall(r"Skipped a (\w+) image of (\d+) bytes", log)
                assert skipped == [(kind, str(len(heads[kind])))], (name, skipped, log[-4000:])
                code, lines, log = packets(name, "-skip_image_head", "0")
                assert code != 0 or lines != reference, (name, log[-4000:])
                print(f"PASS HLS image head {kind} in front of segments named {pattern}"
                      + (" with AES-128" if encrypted else "")
                      + ": skipped, the packets of the plain segments; not skipped with the option off")
            for name in ("plain", "fmp4"):
                code, on, log = packets(name)
                assert code == 0 and len(on) > 100 and "Skipped a" not in log, (name, code, log[-4000:])
                code, off, log = packets(name, "-skip_image_head", "0")
                assert code == 0 and on == off, (name, code, log[-4000:])
                print(f"PASS HLS {name} segments read unchanged with skip_image_head")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
