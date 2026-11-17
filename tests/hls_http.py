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
    try of every segment: a 404 once, a lasting 503 at most seg_max_retry + 1
    times. A single 503 is still retried and the stream then plays."""
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
        for name in ("dead", "busy", "flaky"):
            (root / f"{name}.m3u8").write_text(
                playlist.replace('URI="aes.key"', f'URI="{name}.key"'), encoding="ascii")
        (root / "busy.key").write_bytes(bytes(range(16)))
        (root / "flaky.key").write_bytes(bytes(range(16)))

        server, thread = serve(root)
        try:
            for name, unavailable, max_retry, success, key_requests in (
                    ("dead", 0, retries, False, 1),
                    ("busy", -1, retries, False, retries + 1),
                    ("flaky", 1, 1, True, 2)):
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
                    assert segments <= set(server.requested), (
                        name, sorted(segments - set(server.requested)), log)
                else:
                    # the error still ends the open, no segment was read
                    assert result.returncode, (name, log)
                    assert not segments & set(server.requested), (name, server.requested)
                    assert "is not requested again" in log, (name, log)
                answers = {0: "404", -1: "always 503", 1: "one 503"}[unavailable]
                print(f"PASS HLS AES-128 key {name} ({answers}): "
                      f"{count} key request(s) for {len(segments)} segments")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--realtime", action="store_true",
                        help="Clock-driven windows, delayed external reactivation and nonzero initial window")
    args = parser.parse_args()
    if not args.realtime:
        check_dead_key(args.ffmpeg)
        check_tls(args.ffmpeg)
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


if __name__ == "__main__":
    main()
