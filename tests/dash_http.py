"""Loopback tests for DASH recovery/startup/ranges, HLS discontinuities and CENC key requests."""

import argparse
import base64
from collections import Counter
import http.server
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit
import xml.etree.ElementTree as ET


def ffmpeg_run(*arguments):
    """ffmpeg that may fail: (return code, stdout, stderr)."""
    result = subprocess.run(arguments, capture_output=True, timeout=60)
    return result.returncode, result.stdout.decode(), result.stderr.decode(errors="replace")


def crc_streams(output):
    """framecrc output -> {stream: [(size, crc), ...]}"""
    streams = {}
    for line in output.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        fields = [field.strip() for field in line.split(",")]
        streams.setdefault(int(fields[0]), []).append((int(fields[4]), fields[5]))
    return streams


def pssh_boxes(data):
    """whole pssh boxes -> [(system id hex, KIDs hex or None, data)]"""
    boxes, pos = [], 0
    while pos + 8 <= len(data):
        size = int.from_bytes(data[pos:pos + 4], "big")
        assert data[pos + 4:pos + 8] == b"pssh" and size >= 32, data
        box = data[pos:pos + size]
        version, kids, at = box[8], None, 28
        if version:
            count = int.from_bytes(box[28:32], "big")
            kids = [box[32 + 16 * i:48 + 16 * i].hex() for i in range(count)]
            at = 32 + 16 * count
        length = int.from_bytes(box[at:at + 4], "big")
        boxes.append((box[12:28].hex(), kids, box[at + 4:at + 4 + length]))
        pos += size
    assert pos == len(data), data
    return boxes


def pssh_box(system, payload, kids=None):
    body = (b"\x01\x00\x00\x00" if kids else b"\x00\x00\x00\x00") + bytes.fromhex(system)
    if kids:
        body += len(kids).to_bytes(4, "big") + b"".join(bytes.fromhex(kid) for kid in kids)
    body += len(payload).to_bytes(4, "big") + payload
    return (len(body) + 8).to_bytes(4, "big") + b"pssh" + body


def check_cenc_key_request(ffmpeg, top):
    """decryption_key_url (mov) and cenc_decryption_key_url (DASH, HLS): the
    key of every KID is requested once, through the outer context."""
    root = top / "cenc"
    root.mkdir()
    widevine = "edef8ba979d64acea3c827dcd51d21ed"
    playready = "9a04f07998404286ab92e65be0885f95"
    kid_a, key_a = "00112233445566778899aabbccddeeff", "0f1e2d3c4b5a69788796a5b4c3d2e1f0"
    kid_b, key_b = "ffeeddccbbaa99887766554433221100", "a0b1c2d3e4f5061728394a5b6c7d8e9f"
    kid_t = "1234567890abcdef1234567890abcdef"  # the second track of the MP4
    pcm = root / "input.pcm"
    pcm.write_bytes(bytes((i * 7919 + (i >> 9) * 31) & 0xFF for i in range(48000 * 4 * 3)))
    clear = {}
    for name, rate in (("a", "64k"), ("b", "96k")):
        clear[name] = root / f"clear-{name}.mp4"
        run(ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(pcm),
            "-c:a", "aac", "-b:a", rate, str(clear[name]))
    reference = {name: crc_streams(run(ffmpeg, "-i", str(path), "-c", "copy", "-f", "framecrc", "-"))[0]
                 for name, path in clear.items()}

    # one DASH representation (and HLS playlist) per key: the dash muxer
    # encrypts all its representations with the same key
    ns = {"d": "urn:mpeg:dash:schema:mpd:2011", "cenc": "urn:mpeg:cenc:2013"}
    ET.register_namespace("", ns["d"])
    ET.register_namespace("cenc", ns["cenc"])
    representations = []
    for name, source, kid, key in (("a", "a", kid_a, key_a), ("b", "b", kid_b, key_b),
                                   ("c", "b", kid_a, key_a)):
        directory = root / name
        directory.mkdir()
        run(ffmpeg, "-y", "-i", str(clear[source]), "-c", "copy", "-f", "dash",
            "-seg_duration", "1", "-use_template", "1", "-use_timeline", "1",
            "-init_seg_name", "init.m4s", "-media_seg_name", "seg-$Number%05d$.m4s",
            "-format_options", f"encryption_scheme=cenc-aes-ctr:encryption_key={key}:encryption_kid={kid}",
            "-hls_playlist", "1", str(directory / "stream.mpd"))
        mpd = ET.parse(directory / "stream.mpd").getroot()
        adaptation = mpd.find(".//d:AdaptationSet", ns)
        representation = adaptation.find("d:Representation", ns)
        template = representation.find("d:SegmentTemplate", ns)
        if template is None:
            template = adaptation.find("d:SegmentTemplate", ns)
            representation.append(template)
        template.set("initialization", f"{name}/" + template.get("initialization"))
        template.set("media", f"{name}/" + template.get("media"))
        representation.set("id", name)
        representations.append((mpd, adaptation, representation))
    mpd, adaptation, _ = representations[0]
    for child in list(adaptation):
        if child.tag != f"{{{ns['d']}}}Representation":
            adaptation.remove(child)
    wv_box = pssh_box(widevine, b"ep3 test, any KID")
    pr_box = pssh_box(playready, b"ep3 test, only B", [kid_b])
    protection = ET.Element(f"{{{ns['d']}}}ContentProtection",
                            schemeIdUri=f"urn:uuid:{widevine}")
    ET.SubElement(protection, f"{{{ns['cenc']}}}pssh").text = "\n  " + base64.b64encode(wv_box).decode() + "\n"
    adaptation.insert(0, protection)
    for _, _, representation in representations[1:]:
        adaptation.append(representation)
    protection = ET.Element(f"{{{ns['d']}}}ContentProtection",
                            schemeIdUri=f"urn:uuid:{playready}")
    ET.SubElement(protection, f"{{{ns['cenc']}}}pssh").text = base64.b64encode(pr_box).decode()
    representations[1][2].insert(0, protection)
    (root / "keys.mpd").write_bytes(ET.tostring(mpd))

    # MP4 with two tracks: encrypted with one key, the KID of the second
    # track changed afterwards, so that each track has its own KID. Read
    # over http: the protocol whitelist of a local file ("file,crypto,data")
    # would not let the key request out.
    two = root / "two.mp4"
    run(ffmpeg, "-y", "-i", str(clear["a"]), "-i", str(clear["b"]), "-map", "0", "-map", "1",
        "-c", "copy", "-encryption_scheme", "cenc-aes-ctr", "-encryption_key", key_a,
        "-encryption_kid", kid_a, str(two))
    data = bytearray(two.read_bytes())
    tenc = [m.start() for m in re.finditer(b"tenc", data)]
    assert len(tenc) == 2, tenc
    assert data[tenc[1] + 12:tenc[1] + 28] == bytes.fromhex(kid_a)
    data[tenc[1] + 12:tenc[1] + 28] = bytes.fromhex(kid_t)
    two.write_bytes(bytes(data))

    answers = {}
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            url = urlsplit(self.path)
            status, payload = 200, b""
            if url.path == "/key":
                query = parse_qs(url.query, keep_blank_values=True)
                kid = query["kid"][0]
                pssh = base64.b64decode(query["pssh"][0]) if query.get("pssh", [""])[0] else b""
                requests.append((kid, query.get("type", [None])[0], pssh))
                if kid in answers:
                    payload = answers[kid]
                else:
                    status = 404
            else:
                file = root / url.path.lstrip("/")
                if file.is_file():
                    payload = file.read_bytes()
                else:
                    status = 404
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range") or "")
            if status == 200 and match:
                total = len(payload)
                start = int(match[1])
                end = min(int(match[2]) if match[2] else total - 1, total - 1)
                payload, status = payload[start:end + 1], 206
            self.send_response(status)
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    key_url = base + "/key?kid={kid}&type={type}&pssh={pssh}"

    def read(*options):
        requests.clear()
        code, output, log = ffmpeg_run(ffmpeg, "-hide_banner", *options, "-map", "0",
                                       "-c", "copy", "-f", "framecrc", "-")
        return code, crc_streams(output), log

    def decrypted(packets, name):
        """all packets are clear packets of that source, at most two missing"""
        return set(packets) <= set(reference[name]) and len(packets) >= len(reference[name]) - 2

    try:
        # without a key the packets stay encrypted (the test media is encrypted)
        _, streams, log = read("-i", base + "/keys.mpd")
        assert len(streams) == 3 and not requests, (streams.keys(), requests, log)
        assert all(len(set(packets) & set(reference["a"] + reference["b"])) < 3
                   for packets in streams.values()), log

        # DASH: three representations, two KIDs; the key of A as hex, the
        # one of B as 16 bytes
        answers.update({kid_a: (" " + key_a.upper() + "\r\n").encode(), kid_b: bytes.fromhex(key_b)})
        code, streams, log = read("-cenc_decryption_key_url", key_url, "-i", base + "/keys.mpd")
        assert code == 0, log
        assert len(streams) == 3, (streams.keys(), log)
        assert decrypted(streams[0], "a") and decrypted(streams[1], "b") and decrypted(streams[2], "b"), log
        assert sorted(kid for kid, _, _ in requests) == sorted([kid_a, kid_b]), requests
        for kid, kind, pssh in requests:
            assert kind == "audio", requests
            boxes = pssh_boxes(pssh)
            if kid == kid_a:
                assert boxes == [(widevine, None, b"ep3 test, any KID")], boxes
            else:
                assert boxes == [(widevine, None, b"ep3 test, any KID"),
                                 (playready, [kid_b], b"ep3 test, only B")], boxes
        print("PASS CENC key request, DASH: one request per KID for three representations, "
              "hex and binary answers, MPD pssh boxes per KID")

        # a KID without a key: one request, a clear error; its packets come
        # encrypted with their encryption info, the other streams play on
        for answer, message in ((None, f"No key for KID {kid_b}: "),
                                (b"not a key", f"No key for KID {kid_b}: the answer (9 bytes) is not a key")):
            answers.pop(kid_b, None)
            if answer:
                answers[kid_b] = answer
            requests.clear()
            code, output, log = ffmpeg_run(ffmpeg, "-hide_banner", "-cenc_decryption_key_url", key_url,
                                           "-i", base + "/keys.mpd", "-map", "0", "-c", "copy",
                                           "-f", "framecrc", "-")
            streams = crc_streams(output)
            assert code == 0, log
            assert message in log, log
            assert log.count("No key for KID") == 1, log
            assert [kid for kid, _, _ in requests].count(kid_b) == 1, requests
            assert [kid for kid, _, _ in requests].count(kid_a) <= 1, requests
            assert decrypted(streams[0], "a") and len(streams[1]) >= len(reference["b"]) - 2, log
            assert set(streams[1]).isdisjoint(reference["b"]), "stream without a key decrypted?"
            lines = [line for line in output.splitlines() if line.startswith("1,")]
            assert lines and all("Encryption info" in line for line in lines), lines[:3]
        answers[kid_b] = key_b.encode()
        print("PASS CENC key request, DASH: a KID without a key (404, not a key) is requested once, "
              "clear error, its packets stay encrypted with their encryption info, the others play")

        # HLS fMP4 in CENC (no EXT-X-KEY, the KID is in the init segment)
        code, streams, log = read("-cenc_decryption_key_url", key_url, "-i", base + "/b/master.m3u8")
        assert code == 0, log
        assert len(streams) == 1 and decrypted(streams[0], "b"), (streams.keys(), log)
        assert [(kid, kind, pssh) for kid, kind, pssh in requests] == [(kid_b, "audio", b"")], requests
        print("PASS CENC key request, HLS fMP4: the key of the KID requested once")

        # MP4 with a KID per track; decryption_keys is asked first
        answers[kid_t] = key_a.encode()
        code, streams, log = read("-decryption_key_url", key_url, "-i", base + "/two.mp4")
        assert code == 0, log
        assert len(streams) == 2, (streams.keys(), log)
        assert decrypted(streams[0], "a") and decrypted(streams[1], "b"), log
        assert sorted(kid for kid, _, _ in requests) == sorted([kid_a, kid_t]), requests
        code, streams, log = read("-decryption_keys", f"{kid_a}={key_a}",
                                  "-decryption_key_url", key_url, "-i", base + "/two.mp4")
        assert code == 0 and decrypted(streams[0], "a") and decrypted(streams[1], "b"), log
        assert [kid for kid, _, _ in requests] == [kid_t], requests
        answers[kid_t] = key_b.encode()  # a wrong key only breaks its track
        code, streams, log = read("-decryption_key_url", key_url, "-i", base + "/two.mp4")
        assert decrypted(streams[0], "a") and not decrypted(streams[1], "b"), log
        print("PASS CENC key request, MP4: one request per track KID, decryption_keys first, "
              "the key of a KID only for its track")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def run(*arguments):
    result = subprocess.run(arguments, capture_output=True, timeout=20)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout.decode()


def check_segment_query(ffmpeg, root):
    """url_add_query and inherit_manifest_query reach every segment url."""
    template = (root / "static.mpd").read_text()
    assert 'media="chunk-stream' in template, template
    # a manifest whose segment urls carry the token already
    (root / "tokened.mpd").write_text(re.sub(
        r'((?:media|initialization)="[^"?]+)"', r'\1?token=abc"', template))
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            path, _, query = self.path.partition("?")
            requests.append(self.path)
            file = root / path.lstrip("/")
            status = 200
            if not file.is_file():
                status = 404
            elif path.endswith(".m4s"):
                parts = query.split("&") if query else []
                if any(part not in parts for part in self.server.required):
                    status = 403
            payload = file.read_bytes() if status == 200 else b""
            self.send_response(status)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        for manifest, options, required, expected_query, success in (
            ("static.mpd?token=abc", (), ["token=abc"], "", False),
            ("static.mpd?token=abc", ("-inherit_manifest_query", "1"), ["token=abc"],
             "token=abc", True),
            ("static.mpd", ("-url_add_query", "auth=x%2Fy"), ["auth=x%2Fy"],
             "auth=x%2Fy", True),
            ("static.mpd?token=abc", ("-inherit_manifest_query", "1", "-url_add_query", "?auth=1"),
             ["token=abc", "auth=1"], "token=abc&auth=1", True),
            ("tokened.mpd?token=abc", ("-inherit_manifest_query", "1"), ["token=abc"],
             "token=abc", True),
        ):
            requests.clear()
            server.required = required
            result = subprocess.run(
                [ffmpeg, "-nostdin", "-hide_banner", "-v", "error", "-f", "dash", *options,
                 "-i", f"{base}/{manifest}", "-map", "0", "-c", "copy", "-f", "null", "-"],
                capture_output=True, timeout=60)
            log = result.stderr.decode(errors="replace")[-4000:]
            segments = [path for path in requests if ".m4s" in path]
            assert segments, (manifest, options, requests, log)
            assert all(path.partition("?")[2] == expected_query for path in segments), (
                manifest, options, segments)
            if success:
                assert result.returncode == 0, (manifest, options, log)
                assert len(segments) >= 10, segments
            print(f"PASS DASH segment query manifest={manifest} options={list(options)}: "
                  f"{len(segments)} segment requests with query '{expected_query}'")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


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
        check_segment_query(args.ffmpeg, root)
        run(args.ffmpeg, "-y", "-f", "s16le", "-ar", "48000", "-ac", "2",
            "-i", str(audio), "-t", "3", "-c:a", "aac", "-seg_duration", "1",
            "-single_file", "1", "-f", "dash", str(root / "range.mpd"))
        video = root / "video.raw"
        video.write_bytes(bytes([96]) * (16 * 16 * 3 // 2 * 25))
        raw_video = ("-f", "rawvideo", "-pixel_format", "yuv420p",
                     "-video_size", "16x16", "-i", str(video))
        run(args.ffmpeg, "-y", "-stream_loop", "-1", "-f", "s16le",
            "-ar", "48000", "-ac", "2", "-i", str(audio),
            "-stream_loop", "-1", *raw_video, "-map", "1:v",
            "-map", "0:a", "-map", "0:a", "-t", "40", "-c:a", "aac",
            "-c:v", "mpeg2video", "-g", "12", "-bf", "2",
            "-seg_duration", "1", "-single_file", "1", "-f", "dash",
            str(root / "single.mpd"))
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
        single = ET.parse(root / "single.mpd").getroot()
        for representation in single.findall(".//d:Representation", ns):
            segments = representation.find("d:SegmentList", ns)
            initialization = segments.find("d:Initialization", ns)
            segment_base = ET.Element(f"{{{ns['d']}}}SegmentBase")
            segment_base.append(initialization)
            representation.remove(segments)
            representation.append(segment_base)
        (root / "single.mpd").write_bytes(ET.tostring(single))
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
            output = run(args.driver, "switch", base + "/single.mpd")
            starts = re.findall(r"stage=(\d+) type=(\d+) first_us=(-?\d+)", output)
            assert len(starts) == 10, output
            targets = (0, 12375000, 4375000, 30375000, 32375000)
            for stage, kind, pts in starts:
                # MPEG-2 GOP is 12 frames at 25 fps; AAC may need one
                # preroll frame before the frame containing the seek point.
                tolerance = 480000 if int(kind) == 0 else 43000
                assert abs(int(pts) - targets[int(stage)]) <= tolerance, output
            print("PASS single-file DASH: startup, fractional closed-track seeks and direct reactivation")
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
        check_cenc_key_request(args.ffmpeg, root)


if __name__ == "__main__":
    main()
