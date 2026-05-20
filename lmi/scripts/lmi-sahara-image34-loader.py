#!/usr/bin/env python3
import argparse
import os
import select
import struct
import sys
import time

CMD_HELLO = 1
CMD_HELLO_RESP = 2
CMD_READ_DATA = 3
CMD_END_OF_IMAGE = 4
CMD_DONE = 5
CMD_DONE_RESP = 6
CMD_RESET = 7
CMD_READY = 0x0B
CMD_SWITCH_MODE = 0x0C

SAHARA_HELLO_LEN = 48
SAHARA_READ_DATA_LEN = 20
SAHARA_END_OF_IMAGE_LEN = 16
IMAGE_MDMDDR = 34


def log(tag, message=""):
    print(f"[{time.strftime('%H:%M:%S')}] [{tag}] {message}", flush=True)


def unpack_words(data):
    count = len(data) // 4
    if count == 0:
        return ()
    return struct.unpack(f"<{count}I", data[:count * 4])


def dump_packet(tag, data):
    words = unpack_words(data)
    log(tag, f"bytes={len(data)} words=" + " ".join(str(word) for word in words))
    return words


def write_all(fd, data, label):
    written = 0
    while written < len(data):
        chunk = os.write(fd, data[written:])
        if chunk <= 0:
            raise OSError(f"short write while sending {label}")
        written += chunk
    log(f"wrote_{label}", f"bytes={len(data)}")


def read_exact_at(path, offset, length):
    if offset < 0 or length < 0:
        raise ValueError(f"invalid read offset={offset} length={length}")

    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC

    fd = os.open(path, flags)
    try:
        data = os.pread(fd, length, offset)
    finally:
        os.close(fd)

    if len(data) != length:
        raise EOFError(f"short read from {path}: offset={offset} length={length} got={len(data)}")
    return data


def send_hello_resp(fd, words, args):
    version = words[2]
    compatible = args.compat if args.compat is not None else words[3]
    mode = args.mode
    resp = struct.pack(
        "<12I",
        CMD_HELLO_RESP,
        SAHARA_HELLO_LEN,
        version,
        compatible,
        0,
        mode,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    dump_packet("hello_resp", resp)
    write_all(fd, resp, "hello_resp")


def send_done(fd):
    resp = struct.pack("<2I", CMD_DONE, 8)
    dump_packet("done", resp)
    write_all(fd, resp, "done")


def handle_read_data(fd, words, args):
    image, offset, length = words[2], words[3], words[4]
    log("read_data", f"image={image} offset={offset} length={length}")

    if image != IMAGE_MDMDDR:
        return f"stop_unhandled_image_{image}"
    if length > args.max_chunk:
        return f"stop_read_too_large_{length}"

    try:
        payload = read_exact_at(args.image34, offset, length)
    except Exception as exc:
        log("image34_read_error", str(exc))
        return "stop_image34_read_error"

    write_all(fd, payload, "image34_payload")
    return "progress"


def run_session(fd, args, observe_only=False):
    active_image = None
    packet_index = 1
    deadline = time.monotonic() + args.hold_after_done if observe_only else None

    while True:
        if deadline is None:
            timeout = args.timeout
        else:
            timeout = max(0.0, min(args.timeout, deadline - time.monotonic()))
            if timeout == 0.0:
                return "post_done_observe_done"

        ready, _, _ = select.select([fd], [], [], timeout)
        if not ready:
            return "timeout" if deadline is None else "post_done_observe_done"

        try:
            data = os.read(fd, args.read_size)
        except BlockingIOError:
            continue
        except Exception as exc:
            log("read_error", str(exc))
            return "read_error"

        if not data:
            return "eof"

        tag = "post_done_packet" if observe_only else "packet"
        words = dump_packet(f"{tag}{packet_index}", data)
        packet_index += 1
        if len(words) < 2:
            continue

        cmd, pkt_len = words[0], words[1]
        if cmd == CMD_HELLO and pkt_len == SAHARA_HELLO_LEN and len(words) >= 6:
            send_hello_resp(fd, words, args)
            continue

        if cmd == CMD_READ_DATA and pkt_len == SAHARA_READ_DATA_LEN and len(words) >= 5:
            result = handle_read_data(fd, words, args)
            if result != "progress":
                return result
            active_image = words[2]
            continue

        if cmd == CMD_END_OF_IMAGE and pkt_len == SAHARA_END_OF_IMAGE_LEN and len(words) >= 4:
            image, status = words[2], words[3]
            log("end_of_image", f"image={image} status={status}")
            if active_image is not None and image != active_image:
                return f"stop_end_image_mismatch_active_{active_image}_got_{image}"
            if image != IMAGE_MDMDDR:
                return f"stop_end_unhandled_image_{image}"
            if status != 0:
                return f"stop_end_status_{status}"
            send_done(fd)
            if args.hold_after_done > 0 and not observe_only:
                return run_session(fd, args, observe_only=True)
            return "done_sent"

        if cmd == CMD_DONE_RESP:
            log("done_resp", "continue")
            continue

        if cmd in (CMD_RESET, CMD_READY, CMD_SWITCH_MODE):
            log("control_packet", f"cmd={cmd} pkt_len={pkt_len}")
            continue

        return f"stop_unknown_cmd_{cmd}_len_{pkt_len}"


def parse_args():
    parser = argparse.ArgumentParser(description="Minimal read-only Sahara image 34 loader for lmi SDX55M diagnostics")
    parser.add_argument("--dev", default="/dev/mhi_sahara0")
    parser.add_argument("--image34", default="/dev/disk/by-partlabel/mdmddr")
    parser.add_argument("--compat", type=int, default=None)
    parser.add_argument("--mode", type=int, default=0)
    parser.add_argument("--max-chunk", type=int, default=0x8000)
    parser.add_argument("--read-size", type=int, default=0x8000)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--hold-after-done", type=float, default=30.0)
    return parser.parse_args()


def main():
    args = parse_args()
    log("start", f"device={args.dev} image34={args.image34} mode={args.mode} max_chunk={args.max_chunk}")

    flags = os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC

    try:
        fd = os.open(args.dev, flags)
    except Exception as exc:
        log("open_failed", str(exc))
        return 1

    try:
        result = run_session(fd, args)
        log("result", result)
        return 0 if result in ("done_sent", "post_done_observe_done") else 2
    finally:
        os.close(fd)
        log("closed", args.dev)


if __name__ == "__main__":
    sys.exit(main())
