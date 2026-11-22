"""AC-4 decoder: crafted frames that used to crash or corrupt memory.

Every case is a raw AC-4 stream (sync frames) built bit by bit. Each one
must decode without the process dying (abort, assertion, segfault) and
must reach the code path it is meant for: either the decoder reports the
expected error, or it decodes a complete frame.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

FRAME_LEN = 2048            # frame_rate_index 2
OUT_BYTES = FRAME_LEN * 2   # one mono frame as s16


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        for i in reversed(range(count)):
            self.bits.append((value >> i) & 1)

    def code(self, text):
        for char in text:
            self.bits.append(int(char))

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def data(self):
        self.align()
        return bytes(int("".join(map(str, self.bits[i:i + 8])), 2)
                     for i in range(0, len(self.bits), 8))


def variable_bits(w, chunks, bits):
    """chunks: values of the successive groups, all but the last extended"""
    for i, value in enumerate(chunks):
        w.put(value, bits)
        w.put(i + 1 < len(chunks), 1)


def emdf_info(w):
    w.put(0, 2)                 # emdf_version
    w.put(0, 3)                 # key_id
    w.put(0, 1)                 # b_emdf_payloads_substream_info
    w.put(0, 4)                 # protection_length_primary/secondary


def frame_rate_multiply_info(w, frame_rate_index):
    if frame_rate_index in (0, 1, 2, 3, 4, 7, 8, 9):
        w.put(0, 1)             # b_multiplier


def toc(w, frame_rate_index=2, version=2, substream_index=None):
    """one presentation with one mono substream; version 0 or 2"""
    w.put(version, 2)           # bitstream_version
    w.put(0, 10)                # sequence_counter
    w.put(0, 1)                 # b_wait_frames
    w.put(1, 1)                 # fs_index: 48 kHz
    w.put(frame_rate_index, 4)
    w.put(1, 1)                 # b_iframe_global
    w.put(1, 1)                 # b_single_presentation
    w.put(0, 1)                 # b_payload_base
    if version == 0:
        # ac4_presentation_info()
        w.put(1, 1)             # b_single_substream
        w.put(0, 1)             # presentation_version (unary)
        w.put(0, 3)             # mdcompat
        w.put(0, 1)             # b_presentation_id
        frame_rate_multiply_info(w, frame_rate_index)
        emdf_info(w)
        # ac4_substream_info()
        w.code("0")             # channel_mode: mono
        w.put(0, 1)             # b_sf_multiplier
        w.put(0, 1)             # b_bitrate_info
        w.put(0, 1)             # b_content_type
        w.put(1, 1)             # b_iframe
        if substream_index is None:
            w.put(0, 2)
        else:
            w.put(3, 2)
            variable_bits(w, substream_index, 2)
        w.put(0, 1)             # b_pre_virtualized
        w.put(0, 1)             # b_add_emdf_substreams
    else:
        w.put(0, 1)             # b_program_id
        # ac4_presentation_v1_info()
        w.put(1, 1)             # b_single_substream_group
        w.put(0, 1)             # presentation_version (unary)
        w.put(0, 3)             # mdcompat
        w.put(0, 1)             # b_presentation_id
        frame_rate_multiply_info(w, frame_rate_index)
        if 5 <= frame_rate_index <= 12:
            w.put(0, 1)         # b_frame_rate_fraction
        emdf_info(w)
        w.put(0, 1)             # b_presentation_filter
        w.put(0, 3)             # group_index
        w.put(0, 1)             # b_pre_virtualized
        w.put(0, 1)             # b_add_emdf_substreams
        w.put(0, 1)             # b_alternative
        w.put(0, 1)             # b_pres_ndot
        w.put(1, 2)             # presentation substream index (unused)
        # ac4_substream_group_info()
        w.put(1, 1)             # b_substreams_present
        w.put(0, 1)             # b_hsf_ext
        w.put(1, 1)             # b_single_substream
        w.put(1, 1)             # b_channel_coded
        # ac4_substream_info_chan()
        w.code("0")             # channel_mode: mono
        w.put(0, 1)             # b_sf_multiplier
        w.put(0, 1)             # b_bitrate_info
        w.put(1, 1)             # b_iframe
        w.put(0, 2)             # substream_index
        w.put(0, 1)             # b_content_type
    # substream_index_table()
    w.put(1, 2)                 # n_substreams
    w.put(0, 1)                 # b_size_present
    w.align()


def mono_core(w):
    w.put(0, 1)                 # spec_frontend: ASF
    w.put(1, 1)                 # b_long_frame
    w.put(0, 6)                 # max_sfb[0]
    w.put(100, 8)               # reference_scale_factor
    w.put(0, 1)                 # b_snf_data_exists


def aspx_config(w, start, stop, scale, noise_sbg):
    w.put(0, 1)                 # aspx_quant_mode_env
    w.put(start, 3)
    w.put(stop, 2)
    w.put(scale, 1)             # aspx_master_freq_scale
    w.put(0, 1)                 # aspx_interpolation
    w.put(0, 1)                 # aspx_preflat
    w.put(0, 1)                 # aspx_limiter
    w.put(noise_sbg, 2)
    w.put(0, 1)                 # aspx_num_env_bits_fixfix
    w.put(3, 2)                 # aspx_freq_res_mode: always high resolution


def num_sbg(start, stop, scale, xover):
    return (22 if scale else 20) - 2 * start - 2 * stop - xover


def mono_aspx(w, start, stop, scale, noise_sbg, xover, num_noise):
    w.put(1, 1)                 # codec_mode: ASPX
    aspx_config(w, start, stop, scale, noise_sbg)
    w.put(1, 1)                 # b_compand_on
    mono_core(w)
    # aspx_data_1ch()
    w.put(xover, 3)             # aspx_xover_subband_offset
    w.code("0")                 # aspx_int_class: FIXFIX
    w.put(0, 1)                 # one envelope
    w.put(0, 1)                 # aspx_sig_delta_dir
    w.put(0, 1)                 # aspx_noise_delta_dir
    w.put(0, 2 * num_noise)     # aspx_tna_mode[]
    w.put(0, 3)                 # no add_harmonic / fic / tic
    sig = num_sbg(start, stop, scale, xover)
    w.code("0000")              # signal F0
    for _ in range(sig - 1):
        w.code("00")            # signal DF, delta 0
    w.code("0")                 # noise F0
    for _ in range(num_noise - 1):
        w.code("0")             # noise DF, delta 0


def frame(audio, **toc_args):
    w = BitWriter()
    toc(w, **toc_args)
    a = BitWriter()
    audio(a)
    payload = a.data()
    w.put(len(payload), 15)     # audio_size
    w.put(0, 1)                 # b_more_bits
    w.align()
    body = w.data() + payload + bytes(8)
    return b"\xac\x40" + len(body).to_bytes(2, "big") + body


def mono_simple(w):
    w.put(0, 1)                 # codec_mode: SIMPLE
    mono_core(w)


CASES = [
    # (name, frames, expected message or None, minimum output bytes)
    ("reserved frame_rate_index",
     [frame(mono_simple, frame_rate_index=14)],
     "reserved frame_rate_index", 0),
    ("variable_bits overflow in substream_index",
     [frame(mono_simple, version=0, substream_index=[1] + [0] * 15)],
     None, 0),
    ("five A-SPX patches",
     [frame(lambda w: mono_aspx(w, 0, 0, 0, 2, 0, 4))],
     None, OUT_BYTES),
    ("five A-SPX noise subband groups",
     [frame(lambda w: mono_aspx(w, 0, 0, 1, 3, 0, 5))],
     None, OUT_BYTES),
    ("A-SPX crossover offset beyond the master table",
     [frame(lambda w: mono_aspx(w, 0, 0, 0, 2, 0, 4)),
      frame(lambda w: mono_aspx(w, 7, 3, 0, 0, 1, 1))],
     "invalid aspx xover subband offset", OUT_BYTES),
]


def run_case(ffmpeg, directory, name, frames, message, min_bytes):
    path = directory / (name.replace(" ", "_") + ".ac4")
    path.write_bytes(b"".join(frames))
    result = subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-v", "error",
                             "-f", "ac4", "-i", str(path),
                             "-f", "s16le", "-"],
                            capture_output=True, timeout=60)
    log = result.stderr.decode(errors="replace")
    problems = []
    # decode errors may end ffmpeg with an error status, a crash is a signal
    if result.returncode < 0:
        problems.append(f"killed by signal {-result.returncode}")
    if "Assertion" in log:
        problems.append("assertion")
    if message and message not in log:
        problems.append(f"no {message!r} in the log")
    if len(result.stdout) < min_bytes:
        problems.append(f"{len(result.stdout)} bytes decoded, expected {min_bytes}")
    status = "FAIL " + ", ".join(problems) if problems else "ok"
    print(f"{name}: {status}")
    if problems:
        print("    " + log.strip().replace("\n", "\n    "))
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg")
    parser.add_argument("--write", metavar="DIR",
                        help="only write the crafted streams to DIR")
    args = parser.parse_args()
    if args.write:
        directory = Path(args.write)
        directory.mkdir(parents=True, exist_ok=True)
        for name, frames, _, _ in CASES:
            (directory / (name.replace(" ", "_") + ".ac4")).write_bytes(b"".join(frames))
        return
    if not args.ffmpeg:
        parser.error("--ffmpeg is required")
    with tempfile.TemporaryDirectory(prefix="ffmpeg-ac4dec-") as directory:
        results = [run_case(args.ffmpeg, Path(directory), *case) for case in CASES]
    if not all(results):
        raise SystemExit("ac4dec: FAILED")
    print("ac4dec: OK")


if __name__ == "__main__":
    main()
