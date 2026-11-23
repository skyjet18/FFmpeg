"""Loopback regression for segmented, sparse HLS subtitles."""

import argparse
import http.server
from pathlib import Path
import subprocess
import tempfile
import threading
import time


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        super().do_GET()

    def log_message(self, *_):
        pass


def command(process, text, expect_error=False):
    process.stdin.write(text + "\n")
    process.stdin.flush()
    result = []
    while True:
        line = process.stdout.readline()
        if not line:
            raise RuntimeError("subtitle test driver exited")
        if line.startswith("done "):
            failed = line.strip() != "done 0"
            if failed != expect_error:
                raise RuntimeError(line.strip())
            return result
        result.append(line.strip())


def run_player(executable, *args):
    with tempfile.TemporaryFile(mode="w+") as stdout, \
         tempfile.TemporaryFile(mode="w+") as stderr:
        process = subprocess.Popen([executable, *args], stdin=subprocess.PIPE,
                                   stdout=stdout, stderr=stderr, text=True)
        try:
            process.wait(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
        stderr.seek(0)
        output = stderr.read()
        assert process.returncode == 0, (process.returncode, output[-4000:])
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--player", help="Optional exteplayer3 host build")
    parser.add_argument("--empty", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hls-subtitle-") as directory:
        root = Path(directory)
        subprocess.run([args.ffmpeg, "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=64x64:rate=10",
                        "-t", "24", "-c:v", "mpeg2video", "-g", "20",
                        "-f", "hls", "-hls_time", "2", "-hls_list_size", "0",
                        "-hls_segment_filename", str(root / "video-%03d.ts"),
                        str(root / "video.m3u8")], check=True, timeout=30)
        (root / "master.m3u8").write_text(
            '#EXTM3U\n'
            '#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="s",NAME="Czech",LANGUAGE="cs",'
            'DEFAULT=NO,AUTOSELECT=YES,URI="sub0.m3u8"\n'
            '#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="s",NAME="English",LANGUAGE="en",'
            'DEFAULT=NO,AUTOSELECT=YES,URI="sub1.m3u8"\n'
            '#EXT-X-STREAM-INF:BANDWIDTH=200000,SUBTITLES="s"\nvideo.m3u8\n',
            encoding="ascii")
        (root / "master-live.m3u8").write_text(
            (root / "master.m3u8").read_text().replace(
                "sub0.m3u8", "sub0-live.m3u8").replace(
                "sub1.m3u8", "sub1-live.m3u8").replace(
                "video.m3u8", "video-live.m3u8"), encoding="ascii")
        (root / "video-live.m3u8").write_text(
            (root / "video.m3u8").read_text().replace("#EXT-X-ENDLIST\n", ""),
            encoding="ascii")
        for track in range(2):
            playlist = '#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n'
            live_playlist = playlist
            for index in range(601):
                name = f"sub{track}-{index:03d}.webvtt"
                playlist += f"#EXTINF:2.000,\n{name}\n"
                if index < 12:
                    live_playlist += f"#EXTINF:2.000,\n{name}\n"
                cue = ""
                if not args.empty and index in (1, 3, 7, 9, 10, 11):
                    second = index * 2
                    cue = (f"00:00:{second:02d}.500 --> 00:00:{second:02d}.900\n"
                           f"cue-{track}-{index}\n\n")
                (root / name).write_text(
                    "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:126000\n\n"
                    + cue, encoding="ascii")
            (root / f"sub{track}.m3u8").write_text(playlist + "#EXT-X-ENDLIST\n",
                                                    encoding="ascii")
            (root / f"sub{track}-live.m3u8").write_text(live_playlist, encoding="ascii")
        server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), lambda *a, **k: Handler(*a, directory=directory, **k))
        server.requests = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        process = subprocess.Popen(
            [args.driver, f"http://127.0.0.1:{server.server_port}/master.m3u8"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        try:
            opened = process.stdout.readline()
            assert opened.startswith("opened "), opened
            assert not any("sub0" in p or "sub1" in p for p in server.requests), server.requests
            command(process, "probe")
            after_probe = list(server.requests)
            assert not any("sub0" in p or "sub1" in p for p in after_probe), after_probe
            streams = command(process, "streams")
            assert sum(" cs" in line for line in streams) == 1, streams
            assert sum(" en" in line for line in streams) == 1, streams
            command(process, "select 0")
            packets = command(process, "read 100")
            requests = list(server.requests)
            assert not any("sub1" in p for p in requests), requests
            segments = [p for p in requests if "sub0-" in p and ".webvtt" in p]
            assert len(segments) <= 7, segments
            if args.empty:
                assert not any(p.startswith("packet 3 ") for p in packets), packets
                print("PASS 601 empty subtitle segments remain bounded")
                return
            assert any("cue-0-1" in p for p in packets), packets
            command(process, "select -1")
            count = len(server.requests)
            command(process, "read 20")
            assert not any("sub0-" in p for p in server.requests[count:])
            command(process, "select 1")
            packets = command(process, "read 80")
            assert any("cue-1-7" in p for p in packets), packets
            assert not any("sub0-" in p for p in server.requests[count:])
            command(process, "seekany 4")
            packets = command(process, "read 60")
            assert any("cue-1-1" in p for p in packets), packets
            assert next(int(p.split()[2]) for p in packets if "cue-1-1" in p) == 3900000, packets
            assert any("cue-1-3" in p for p in packets), packets
            command(process, "select 0")
            command(process, "seek 2")
            packets = command(process, "read 50")
            assert any("cue-0-1" in p for p in packets), packets
            assert command(process, "streams") == streams
            # a missing or broken subtitle segment loses its cues, not the
            # playback: the video and the next cues go on
            command(process, "seek 2")
            (root / "sub0-001.webvtt").unlink()
            packets = command(process, "read 80")
            assert not any("cue-0-1" in p for p in packets), packets
            assert any("cue-0-3" in p for p in packets), packets
            assert sum(p.startswith("packet 0 ") for p in packets) >= 30, packets
            (root / "sub0-001.webvtt").write_text("not WebVTT\n", encoding="ascii")
            command(process, "seek 2")
            packets = command(process, "read 80")
            assert not any("cue-0-1" in p for p in packets), packets
            assert any("cue-0-3" in p for p in packets), packets
            (root / "sub0-001.webvtt").write_text(
                "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:126000\n\n"
                "00:00:02.500 --> 00:00:02.900\ncue-0-1\n\n", encoding="ascii")
            command(process, "seek 2")
            packets = command(process, "read 80")
            assert any("cue-0-1" in p for p in packets), packets
            # segments in another format (MP4): the rendition is
            # dropped after the first one, the playback goes on
            sub1 = [root / f"sub1-{i:03d}.webvtt" for i in range(12)]
            saved = [p.read_bytes() for p in sub1]
            subprocess.run([args.ffmpeg, "-loglevel", "error", "-y",
                            "-f", "lavfi", "-i", "testsrc2=size=64x64:rate=10",
                            "-t", "1", "-c:v", "mpeg2video", "-f", "mp4",
                            str(root / "fmp4.bin")], check=True, timeout=30)
            for p in sub1:
                p.write_bytes((root / "fmp4.bin").read_bytes())
            command(process, "select 1")
            command(process, "seek 0")
            count = len(server.requests)
            packets = command(process, "read 80")
            loaded = [p for p in server.requests[count:] if "sub1-" in p]
            assert len(loaded) == 1, loaded
            assert not any(p.startswith("packet 3 ") for p in packets), packets
            assert sum(p.startswith("packet 0 ") for p in packets) == 80, packets
            for p, data in zip(sub1, saved):
                p.write_bytes(data)
            process.stdin.close()
            assert process.wait(timeout=10) == 0
            if args.player:
                url = f"http://127.0.0.1:{server.server_port}/master.m3u8"
                server.requests.clear()
                output = run_player(args.player, url)
                assert "S_TEXT/WEBVTT" in output, output[-4000:]
                assert not any("sub0" in p or "sub1" in p for p in server.requests), server.requests
                print("PASS exteplayer3 leaves unselected subtitles unloaded")
                server.requests.clear()
                output = run_player(args.player, "-9", "0", url)
                assert "cue-0-1" in output, output[-4000:]
                assert not any("sub1" in p for p in server.requests), server.requests
                assert sum("sub0-" in p and ".webvtt" in p
                           for p in server.requests) < 20, server.requests
                print("PASS exteplayer3 startup subtitle selection")
            server.requests.clear()
            process = subprocess.Popen(
                [args.driver, f"http://127.0.0.1:{server.server_port}/master-live.m3u8"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1)
            assert process.stdout.readline().startswith("opened ")
            command(process, "probe")
            assert not any("sub0" in p or "sub1" in p for p in server.requests)
            command(process, "select 0")
            packets = command(process, "read 35")
            segments = [p for p in server.requests if "sub0-" in p and ".webvtt" in p]
            assert segments and int(segments[0].split("-")[-1].split(".")[0]) >= 8, segments
            assert any("cue-0-9" in p or "cue-0-10" in p for p in packets), packets
            live = ('#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:4\n'
                    + "".join(f'#EXTINF:2.000,\nsub0-{i:03d}.webvtt\n'
                              for i in range(4, 12)))
            (root / "sub0-live.m3u8").write_text(live, encoding="ascii")
            time.sleep(2.2)
            packets = command(process, "read 20")
            assert server.requests.count("/sub0-live.m3u8") >= 2, server.requests
            assert any("cue-0-11" in p for p in packets), packets
            print("PASS lazy HLS subtitle selection, VOD seeks and live reload")
        finally:
            process.stdin.close()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            server.shutdown()
            server.server_close()
            thread.join()
            assert process.returncode == 0, (process.returncode, process.stderr.read())


if __name__ == "__main__":
    main()
