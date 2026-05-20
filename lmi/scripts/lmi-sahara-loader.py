#!/usr/bin/env python3
import argparse
import errno
import glob
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
SAHARA_MODE_IMAGE_TX_PENDING = 0
SAHARA_MODE_IMAGE_TX_COMPLETE = 1

IMAGE_PATHS = {
    6: "image/sdx55m/apps.mbn",
    8: "image/sdx55m/qdsp6sw.mbn",
    21: "image/sdx55m/sbl1.mbn",
    23: "image/sdx55m/aop.mbn",
    25: "image/sdx55m/tz.mbn",
    29: "image/sdx55m/acdb.mbn",
    33: "image/sdx55m/hyp.mbn",
    34: "image/sdx55m/mdmddr.mbn",
    36: "image/sdx55m/multi_image_qti.mbn",
    37: "image/sdx55m/multi_image.mbn",
    38: "image/sdx55m/xbl_cfg.elf",
    40: "image/sdx55m/apdp.mbn",
    41: "image/sdx55m/devcfg.mbn",
    42: "image/sdx55m/sec.elf",
}


def now():
    return time.strftime("%H:%M:%S")


def log(tag, msg):
    print(f"[{now()}] {tag}: {msg}", flush=True)


def read_diag_state(args):
    if not args.diag_state_path:
        return None
    paths = sorted(glob.glob(args.diag_state_path))
    if not paths and not glob.has_magic(args.diag_state_path):
        paths = [args.diag_state_path]
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8") as file:
                return path, file.read().strip()
        except OSError:
            continue
    return None


def log_diag_state(args, stage):
    state = read_diag_state(args)
    if state:
        path, text = state
        log("diag_state", f"{stage} {path}: {text}")


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


class FatEntry:
    def __init__(self, name, short_name, attr, cluster, size):
        self.name = name
        self.short_name = short_name
        self.attr = attr
        self.cluster = cluster
        self.size = size

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)


class Fat16:
    def __init__(self, path):
        flags = os.O_RDONLY
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        self.fd = os.open(path, flags)
        self.path = path
        bpb = os.pread(self.fd, 512, 0)
        if len(bpb) < 64:
            raise RuntimeError("short FAT BPB")
        self.bytes_per_sector = u16(bpb, 11)
        self.sectors_per_cluster = bpb[13]
        self.reserved_sectors = u16(bpb, 14)
        self.num_fats = bpb[16]
        self.root_entries = u16(bpb, 17)
        self.total_sectors = u16(bpb, 19) or u32(bpb, 32)
        self.sectors_per_fat = u16(bpb, 22)
        if self.bytes_per_sector == 0 or self.sectors_per_cluster == 0 or self.sectors_per_fat == 0:
            raise RuntimeError("invalid FAT16 geometry")
        self.cluster_size = self.bytes_per_sector * self.sectors_per_cluster
        self.fat_offset = self.reserved_sectors * self.bytes_per_sector
        root_dir_sectors = ((self.root_entries * 32) + (self.bytes_per_sector - 1)) // self.bytes_per_sector
        self.root_offset = (self.reserved_sectors + self.num_fats * self.sectors_per_fat) * self.bytes_per_sector
        self.root_size = self.root_entries * 32
        self.data_offset = self.root_offset + root_dir_sectors * self.bytes_per_sector
        self.entry_cache = {}
        self.chain_cache = {}
        log("fat", f"opened {path} cluster_size={self.cluster_size} root_entries={self.root_entries}")

    def close(self):
        os.close(self.fd)

    def fat_next(self, cluster):
        return u16(os.pread(self.fd, 2, self.fat_offset + cluster * 2), 0)

    def cluster_offset(self, cluster):
        if cluster < 2:
            raise RuntimeError(f"invalid cluster {cluster}")
        return self.data_offset + (cluster - 2) * self.cluster_size

    def cluster_chain(self, first_cluster):
        if first_cluster in self.chain_cache:
            return self.chain_cache[first_cluster]
        chain = []
        seen = set()
        cluster = first_cluster
        while 2 <= cluster < 0xFFF8:
            if cluster in seen:
                raise RuntimeError(f"FAT loop at cluster {cluster}")
            seen.add(cluster)
            chain.append(cluster)
            cluster = self.fat_next(cluster)
        self.chain_cache[first_cluster] = chain
        return chain

    def read_cluster_chain(self, first_cluster):
        chunks = []
        for cluster in self.cluster_chain(first_cluster):
            chunks.append(os.pread(self.fd, self.cluster_size, self.cluster_offset(cluster)))
        return b"".join(chunks)

    def lfn_text(self, entry):
        raw = entry[1:11] + entry[14:26] + entry[28:32]
        chars = []
        for i in range(0, len(raw), 2):
            code = u16(raw, i)
            if code in (0x0000, 0xFFFF):
                break
            chars.append(chr(code))
        return "".join(chars)

    def short_name(self, entry):
        name = entry[0:8].decode("ascii", "ignore").rstrip()
        ext = entry[8:11].decode("ascii", "ignore").rstrip()
        if ext:
            return f"{name}.{ext}"
        return name

    def parse_dir(self, data):
        entries = []
        lfn_parts = []
        for off in range(0, len(data), 32):
            entry = data[off:off + 32]
            if len(entry) < 32:
                break
            first = entry[0]
            if first == 0x00:
                break
            if first == 0xE5:
                lfn_parts = []
                continue
            attr = entry[11]
            if attr == 0x0F:
                seq = entry[0] & 0x1F
                lfn_parts.append((seq, self.lfn_text(entry)))
                continue
            short = self.short_name(entry)
            if short in (".", ".."):
                lfn_parts = []
                continue
            if lfn_parts:
                name = "".join(part for _, part in sorted(lfn_parts, key=lambda item: item[0]))
            else:
                name = short
            cluster = u16(entry, 26)
            size = u32(entry, 28)
            entries.append(FatEntry(name, short, attr, cluster, size))
            lfn_parts = []
        return entries

    def entries_for_dir(self, entry):
        if entry is None:
            return self.parse_dir(os.pread(self.fd, self.root_size, self.root_offset))
        if not entry.is_dir:
            raise RuntimeError(f"not a directory: {entry.name}")
        return self.parse_dir(self.read_cluster_chain(entry.cluster))

    def find_child(self, parent, name):
        target = name.lower()
        for entry in self.entries_for_dir(parent):
            if entry.name.lower() == target or entry.short_name.lower() == target:
                return entry
        raise FileNotFoundError(name)

    def resolve(self, path):
        key = path.lower().strip("/")
        if key in self.entry_cache:
            return self.entry_cache[key]
        parent = None
        entry = None
        for part in key.split("/"):
            entry = self.find_child(parent, part)
            parent = entry
        self.entry_cache[key] = entry
        return entry

    def iter_range(self, path, offset, length, chunk_size):
        entry = self.resolve(path)
        if entry.is_dir:
            raise RuntimeError(f"requested directory {path}")
        if offset < 0 or length < 0 or offset + length > entry.size:
            raise EOFError(f"range outside {path}: offset={offset} length={length} size={entry.size}")
        remaining = length
        absolute = offset
        chain = self.cluster_chain(entry.cluster)
        while remaining:
            cluster_index = absolute // self.cluster_size
            if cluster_index >= len(chain):
                raise EOFError(f"cluster outside {path}: offset={absolute}")
            cluster = chain[cluster_index]
            cluster_off = absolute % self.cluster_size
            take = min(remaining, self.cluster_size - cluster_off, chunk_size)
            data = os.pread(self.fd, take, self.cluster_offset(cluster) + cluster_off)
            if len(data) != take:
                raise EOFError(f"short FAT read {path}: requested={take} got={len(data)}")
            yield data
            absolute += take
            remaining -= take

    def file_size(self, path):
        return self.resolve(path).size


def pack_words(data):
    padded = data + b"\x00" * ((4 - len(data) % 4) % 4)
    return list(struct.unpack("<" + "I" * (len(padded) // 4), padded))


def write_all(fd, data, label, timeout):
    view = memoryview(data)
    done = 0
    while done < len(view):
        try:
            n = os.write(fd, view[done:])
        except BlockingIOError:
            _, writable, _ = select.select([], [fd], [], timeout)
            if not writable:
                raise TimeoutError(f"write timeout for {label}")
            continue
        except OSError as exc:
            if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                _, writable, _ = select.select([], [fd], [], timeout)
                if not writable:
                    raise TimeoutError(f"write timeout for {label}")
                continue
            raise
        if n == 0:
            raise RuntimeError(f"zero-byte write for {label}")
        done += n


def send_hello_resp(fd, words, args, mode_override=None):
    version = words[2] if len(words) > 2 else 2
    compatible = words[3] if len(words) > 3 else 1
    target_mode = words[5] if len(words) > 5 else 0
    mode = mode_override if mode_override is not None else target_mode if args.echo_hello_mode else args.hello_mode
    resp = struct.pack("<12I", CMD_HELLO_RESP, SAHARA_HELLO_LEN, version, compatible, 0, mode, 0, 0, 0, 0, 0, 0)
    write_all(fd, resp, "hello_resp", args.write_timeout)
    override = " override=1" if mode_override is not None else ""
    log("tx", f"HELLO_RESP version={version} compatible={compatible} target_mode={target_mode} mode={mode}{override}")


def send_done(fd, timeout):
    write_all(fd, struct.pack("<2I", CMD_DONE, 8), "done", timeout)
    log("tx", "DONE")


def iter_raw_range(path, offset, length, chunk_size):
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    fd = os.open(path, flags)
    try:
        remaining = length
        absolute = offset
        while remaining:
            take = min(remaining, chunk_size)
            data = os.pread(fd, take, absolute)
            if len(data) != take:
                raise EOFError(f"short raw read {path}: requested={take} got={len(data)}")
            yield data
            absolute += take
            remaining -= take
    finally:
        os.close(fd)


def raw_image_path(image, args):
    if image == 34:
        return args.image34
    if image == 40:
        return args.image40
    return None


def read_keep_prepared(args):
    if not args.keep_prepared_param:
        return None
    try:
        with open(args.keep_prepared_param, "r", encoding="ascii") as param:
            return param.read().strip()
    except OSError as exc:
        log("keep_prepared", f"read_failed {args.keep_prepared_param}: {exc}")
        return None


def write_keep_prepared(args, value):
    if not args.keep_prepared_param:
        return False
    try:
        with open(args.keep_prepared_param, "w", encoding="ascii") as param:
            param.write(f"{value}\n")
    except OSError as exc:
        log("keep_prepared", f"write_failed {args.keep_prepared_param}: {exc}")
        return False
    log("keep_prepared", f"{args.keep_prepared_param}={value}")
    return True


def enable_keep_prepared(args):
    return write_keep_prepared(args, "Y")


def restore_keep_prepared(args, value):
    if value is None or not args.keep_prepared_param:
        return
    try:
        write_keep_prepared(args, value)
    except OSError as exc:
        log("keep_prepared", f"restore_failed {args.keep_prepared_param}: {exc}")


def stream_image(fd, fat, image, offset, length, args):
    if image not in IMAGE_PATHS:
        raise RuntimeError(f"unhandled image id {image}")
    if length > args.max_request:
        raise RuntimeError(f"request too large image={image} length={length} max={args.max_request}")
    raw_path = raw_image_path(image, args)
    sent = 0
    if raw_path and os.path.exists(raw_path):
        log("read_data", f"image={image} offset={offset} length={length} source={raw_path} raw=1")
        for chunk in iter_raw_range(raw_path, offset, length, args.chunk_size):
            write_all(fd, chunk, f"image{image}", args.write_timeout)
            sent += len(chunk)
        log("tx", f"image={image} bytes={sent}")
        return sent
    path = IMAGE_PATHS[image]
    size = fat.file_size(path)
    if raw_path:
        log("source_fallback", f"image={image} missing_raw={raw_path} source={path}")
    log("read_data", f"image={image} offset={offset} length={length} source={path} size={size}")
    for chunk in fat.iter_range(path, offset, length, args.chunk_size):
        write_all(fd, chunk, f"image{image}", args.write_timeout)
        sent += len(chunk)
    log("tx", f"image={image} bytes={sent}")
    return sent


def handle_packet(fd, fat, data, args, hello_mode_override=None):
    words = pack_words(data)
    cmd = words[0] if words else 0
    pkt_len = words[1] if len(words) > 1 else len(data)
    log("rx", f"len={len(data)} cmd={cmd} pkt_len={pkt_len} words={' '.join(str(x) for x in words[:12])}")
    if cmd == 0 and pkt_len == 0 and not any(data):
        log("rx_empty", f"len={len(data)}")
        return "empty", 0, None
    if cmd == CMD_HELLO:
        if len(data) < SAHARA_HELLO_LEN:
            raise RuntimeError(f"short HELLO len={len(data)}")
        target_mode = words[5] if len(words) > 5 else 0
        send_hello_resp(fd, words, args, hello_mode_override)
        if target_mode == 3 and args.mode3_close_delay >= 0:
            return "mode3_close", 0, None
        return "restart", 0, None
    if cmd == CMD_READ_DATA:
        if len(words) < 5:
            raise RuntimeError("short READ_DATA")
        image = words[2]
        offset = words[3]
        sent = stream_image(fd, fat, image, offset, words[4], args)
        if (args.diag_after_read_data_image < 0 or
                args.diag_after_read_data_image == image) and \
                offset >= args.diag_after_read_data_min_offset:
            log_diag_state(args, f"after_read_data image={image} offset={offset}")
        follow_image = args.read_data_follow_image
        if args.read_data_follow_timeout > 0 and \
           (follow_image < 0 or follow_image == image) and \
           offset >= args.read_data_follow_min_offset:
            return "read_data_follow", sent, image
        return "restart", sent, image
    if cmd == CMD_END_OF_IMAGE:
        image = words[2] if len(words) > 2 else None
        status = words[3] if len(words) > 3 else None
        log("end_of_image", f"image={image} status={status}")
        if status == 0:
            send_done(fd, args.write_timeout)
            if args.keep_prepared_after_done or not args.unsafe_done_restart:
                enable_keep_prepared(args)
            if args.ks_pending_timeout > 0:
                return "ks_pending_wait_done_resp", 0, None
            if args.done_wait_timeout > 0:
                return "done_wait", 0, None
            if args.unsafe_done_restart:
                return "done_restart", 0, image
            return "stop", 0, None
        return "stop", 0, None
    if cmd == CMD_DONE_RESP:
        image_tx_pending = words[2] if len(words) > 2 else None
        if image_tx_pending == SAHARA_MODE_IMAGE_TX_PENDING:
            meaning = "pending"
        elif image_tx_pending == SAHARA_MODE_IMAGE_TX_COMPLETE:
            meaning = "complete"
        else:
            meaning = "unknown"
        log("done_resp", f"image_tx_pending={image_tx_pending} meaning={meaning}")
        if image_tx_pending == SAHARA_MODE_IMAGE_TX_PENDING:
            if args.keep_prepared_after_done_resp or not args.unsafe_done_restart:
                enable_keep_prepared(args)
            if args.ks_pending_timeout > 0:
                return "ks_pending_wait_hello", 0, None
            if args.done_resp_follow_timeout > 0:
                return "done_resp_follow", 0, None
            if args.unsafe_done_restart:
                return "done_resp_restart", 0, None
            return "stop", 0, None
        if image_tx_pending == SAHARA_MODE_IMAGE_TX_COMPLETE and args.done_resp_follow_timeout > 0:
            return "done_resp_follow", 0, None
        return "stop", 0, None
    if cmd == CMD_READY:
        log("ready", "received READY")
        return "progress", 0, None
    if cmd == CMD_SWITCH_MODE:
        mode = words[2] if len(words) > 2 else None
        log("switch_mode", f"mode={mode}")
        return "progress", 0, None
    if cmd == CMD_RESET:
        log("reset", "received RESET")
        return "stop", 0, None
    raise RuntimeError(f"unhandled cmd {cmd}")


def next_deadline(default_timeout, hold_deadline):
    if hold_deadline is None:
        return time.monotonic() + default_timeout, "idle"
    return hold_deadline, "hold"


def run_session(fat, args, index):
    flags = os.O_RDWR | os.O_NONBLOCK
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    log("session", f"{index} open {args.dev}")
    fd = os.open(args.dev, flags)
    packets = 0
    bytes_sent = 0
    mode3_deadline = None
    done_wait_deadline = None
    done_resp_deadline = None
    read_data_deadline = None
    read_data_follow_image = None
    restart_deadline = None
    restart_action = None
    restart_image = None
    ks_pending_deadline = None
    ks_pending_phase = None
    hold_deadline = None
    close_reason = "idle"
    keep_prepared_enabled = False
    try:
        deadline = time.monotonic() + args.idle_timeout
        while True:
            now_mono = time.monotonic()
            active_deadline = deadline
            if mode3_deadline is not None:
                active_deadline = min(active_deadline, mode3_deadline)
            if done_wait_deadline is not None:
                active_deadline = done_wait_deadline
            if done_resp_deadline is not None:
                active_deadline = done_resp_deadline
            if read_data_deadline is not None:
                active_deadline = read_data_deadline
            if restart_deadline is not None:
                active_deadline = restart_deadline
            if ks_pending_deadline is not None:
                active_deadline = ks_pending_deadline
            if hold_deadline is not None:
                active_deadline = max(active_deadline, hold_deadline)
            remaining = active_deadline - now_mono
            if remaining <= 0:
                if mode3_deadline is not None and mode3_deadline <= now_mono:
                    log("session", f"{index} mode3_close packets={packets} bytes_sent={bytes_sent}")
                    return packets, bytes_sent, "mode3_close"
                if done_wait_deadline is not None and done_wait_deadline <= now_mono:
                    log("session", f"{index} done_wait_timeout packets={packets} bytes_sent={bytes_sent}")
                    return packets, bytes_sent, "done_wait"
                if done_resp_deadline is not None and done_resp_deadline <= now_mono:
                    log("session", f"{index} done_resp_follow_timeout packets={packets} bytes_sent={bytes_sent}")
                    return packets, bytes_sent, "done_resp_follow"
                if read_data_deadline is not None and read_data_deadline <= now_mono:
                    log_diag_state(args, f"read_data_follow_timeout image={read_data_follow_image}")
                    log("session", f"{index} read_data_follow_timeout image={read_data_follow_image} packets={packets} bytes_sent={bytes_sent}")
                    return packets, bytes_sent, "read_data_follow"
                if restart_deadline is not None and restart_deadline <= now_mono:
                    log("session", f"{index} {restart_action} packets={packets} bytes_sent={bytes_sent}")
                    return packets, bytes_sent, restart_action
                if ks_pending_deadline is not None and ks_pending_deadline <= now_mono:
                    log("session", f"{index} ks_pending_{ks_pending_phase}_timeout packets={packets} bytes_sent={bytes_sent}")
                    return packets, bytes_sent, f"ks_pending_{ks_pending_phase}"
                if hold_deadline is not None and active_deadline == hold_deadline:
                    close_reason = "hold_after_image"
                log("session", f"{index} {close_reason}_timeout packets={packets} bytes_sent={bytes_sent}")
                return packets, bytes_sent, close_reason
            readable, _, _ = select.select([fd], [], [], min(remaining, 1.0))
            if not readable:
                continue
            try:
                data = os.read(fd, args.read_size)
            except BlockingIOError:
                continue
            if not data:
                log("session", f"{index} eof packets={packets} bytes_sent={bytes_sent}")
                return packets, bytes_sent, "eof"
            packets += 1
            hello_mode_override = None
            if restart_deadline is not None and restart_action == "done_restart" and \
               args.done_hello_mode >= 0 and args.done_hello_mode_image >= -1 and \
               (args.done_hello_mode_image < 0 or args.done_hello_mode_image == restart_image):
                hello_mode_override = args.done_hello_mode
            action, sent, image = handle_packet(fd, fat, data, args, hello_mode_override)
            bytes_sent += sent
            if action == "empty":
                continue
            deadline = time.monotonic() + args.idle_timeout
            if restart_deadline is not None and restart_action in ("done_restart", "done_resp_restart") and action == "restart" and sent == 0 and image is None:
                if restart_action == "done_restart" and args.done_hello_close_image >= -1 and \
                   (args.done_hello_close_image < 0 or args.done_hello_close_image == restart_image):
                    restart_action = "done_hello_restart"
                    restart_deadline = time.monotonic() + args.done_hello_close_delay
                    log("session", f"{index} close_after_done_hello image={restart_image} delay={args.done_hello_close_delay}")
                    continue
                log("session", f"{index} continue_after_{restart_action}_packet")
                restart_deadline = None
                restart_action = None
                restart_image = None
                continue
            if restart_deadline is not None:
                log("session", f"{index} cancel_{restart_action}_after_packet")
                restart_deadline = None
                restart_action = None
                restart_image = None
            if done_wait_deadline is not None and action == "restart" and sent == 0 and image is None:
                log("session", f"{index} continue_after_done_wait_packet")
                done_wait_deadline = None
                continue
            if done_wait_deadline is not None and action != "done_wait":
                log("session", f"{index} cancel_done_wait_after_packet")
                done_wait_deadline = None
            if done_resp_deadline is not None and action == "restart" and sent == 0 and image is None:
                log("session", f"{index} continue_after_done_resp_packet")
                done_resp_deadline = None
                continue
            if done_resp_deadline is not None and action != "done_resp_follow":
                log("session", f"{index} cancel_done_resp_follow_after_packet")
                done_resp_deadline = None
            if read_data_deadline is not None and action != "read_data_follow":
                log("session", f"{index} cancel_read_data_follow_after_packet image={read_data_follow_image}")
                read_data_deadline = None
                read_data_follow_image = None
            if ks_pending_deadline is not None and action == "restart" and sent == 0 and image is None and ks_pending_phase == "hello":
                log("session", f"{index} continue_after_ks_pending_hello")
                ks_pending_deadline = None
                ks_pending_phase = None
                continue
            if ks_pending_deadline is not None and action not in ("ks_pending_wait_done_resp", "ks_pending_wait_hello"):
                log("session", f"{index} cancel_ks_pending_after_packet phase={ks_pending_phase}")
                ks_pending_deadline = None
                ks_pending_phase = None
            if sent and mode3_deadline is not None:
                log("session", f"{index} cancel_mode3_close_after_payload")
                mode3_deadline = None
            if sent and image == args.keep_prepared_after_image and not keep_prepared_enabled:
                keep_prepared_enabled = enable_keep_prepared(args)
            if sent and image == args.hold_after_image and args.hold_after_image_timeout > 0:
                hold_deadline = time.monotonic() + args.hold_after_image_timeout
                close_reason = "hold_after_image"
                if not keep_prepared_enabled:
                    keep_prepared_enabled = enable_keep_prepared(args)
                log("session", f"{index} hold_after_image={image} timeout={args.hold_after_image_timeout}")
            if action == "mode3_close":
                mode3_deadline = time.monotonic() + args.mode3_close_delay
                log("session", f"{index} mode3_close_delay={args.mode3_close_delay}")
            if action == "done_wait":
                done_wait_deadline = time.monotonic() + args.done_wait_timeout
                log("session", f"{index} done_wait_timeout={args.done_wait_timeout}")
            if action == "done_resp_follow":
                done_resp_deadline = time.monotonic() + args.done_resp_follow_timeout
                log("session", f"{index} done_resp_follow_timeout={args.done_resp_follow_timeout}")
            if action == "read_data_follow":
                read_data_follow_image = image
                read_data_deadline = time.monotonic() + args.read_data_follow_timeout
                log("session", f"{index} read_data_follow_timeout={args.read_data_follow_timeout} image={image}")
            if action == "ks_pending_wait_done_resp":
                ks_pending_phase = "done_resp"
                ks_pending_deadline = time.monotonic() + args.ks_pending_timeout
                log("session", f"{index} ks_pending_wait_done_resp_timeout={args.ks_pending_timeout}")
            if action == "ks_pending_wait_hello":
                ks_pending_phase = "hello"
                ks_pending_deadline = time.monotonic() + args.ks_pending_timeout
                log("session", f"{index} ks_pending_wait_hello_timeout={args.ks_pending_timeout}")
            if action == "stop":
                return packets, bytes_sent, "stop"
            if action in ("restart", "done_restart", "done_resp_restart") and hold_deadline is None:
                delay = args.restart_after_packet_delay
                if action == "done_restart" and args.done_restart_delay >= 0:
                    delay = args.done_restart_delay
                if action == "done_resp_restart" and args.done_resp_restart_delay >= 0:
                    delay = args.done_resp_restart_delay
                if delay > 0:
                    restart_action = action
                    restart_image = image
                    restart_deadline = time.monotonic() + delay
                    log("session", f"{index} {action}_delay={delay}")
                    continue
                log("session", f"{index} {action} packets={packets} bytes_sent={bytes_sent}")
                return packets, bytes_sent, action
    finally:
        os.close(fd)
        log("session", f"{index} close")


def parse_args():
    parser = argparse.ArgumentParser(description="Read-only multi-image Sahara loader for lmi SDX55M diagnostics")
    parser.add_argument("--dev", default="/dev/mhi_sahara0")
    parser.add_argument("--modem", default="/dev/disk/by-partlabel/modem")
    parser.add_argument("--sessions", type=int, default=16)
    parser.add_argument("--idle-timeout", type=float, default=90.0)
    parser.add_argument("--between-sessions", type=float, default=2.0)
    parser.add_argument("--restart-after-packet-delay", type=float, default=1.0)
    parser.add_argument("--done-restart-delay", type=float, default=-1.0)
    parser.add_argument("--done-hello-close-image", type=int, default=-2)
    parser.add_argument("--done-hello-close-delay", type=float, default=1.0)
    parser.add_argument("--done-hello-mode-image", type=int, default=-2)
    parser.add_argument("--done-hello-mode", type=int, default=-1)
    parser.add_argument("--done-wait-timeout", type=float, default=0.0)
    parser.add_argument("--done-resp-restart-delay", type=float, default=-1.0)
    parser.add_argument("--done-resp-follow-timeout", type=float, default=0.0)
    parser.add_argument("--unsafe-done-restart", action="store_true")
    parser.add_argument("--read-data-follow-timeout", type=float, default=0.0)
    parser.add_argument("--read-data-follow-image", type=int, default=-1)
    parser.add_argument("--read-data-follow-min-offset", type=int, default=0)
    parser.add_argument("--diag-state-path", default="/sys/bus/pci/devices/*/sdx55m_esoc_diag_state")
    parser.add_argument("--diag-after-read-data-image", type=int, default=-1)
    parser.add_argument("--diag-after-read-data-min-offset", type=int, default=0)
    parser.add_argument("--ks-pending-timeout", type=float, default=0.0)
    parser.add_argument("--empty-limit", type=int, default=4)
    parser.add_argument("--read-size", type=int, default=4096)
    parser.add_argument("--chunk-size", type=int, default=65536)
    parser.add_argument("--max-request", type=int, default=268435456)
    parser.add_argument("--write-timeout", type=float, default=30.0)
    parser.add_argument("--image34", default="/dev/disk/by-partlabel/mdmddr")
    parser.add_argument("--image40", default="/dev/disk/by-partlabel/msadp")
    parser.add_argument("--hello-mode", type=int, default=0)
    parser.add_argument("--echo-hello-mode", action="store_true")
    parser.add_argument("--mode3-close-delay", type=float, default=2.0)
    parser.add_argument("--after-mode3-delay", type=float, default=34.0)
    parser.add_argument("--hold-after-image", type=int, default=-1)
    parser.add_argument("--hold-after-image-timeout", type=float, default=0.0)
    parser.add_argument("--keep-prepared-after-image", type=int, default=-1)
    parser.add_argument("--keep-prepared-after-done", action="store_true")
    parser.add_argument("--keep-prepared-after-done-resp", action="store_true")
    parser.add_argument("--keep-prepared-param", default="/sys/module/mhi_sahara_diag/parameters/keep_prepared_on_release")
    return parser.parse_args()


def main():
    args = parse_args()
    initial_keep_prepared = read_keep_prepared(args)
    fat = Fat16(args.modem)
    total_packets = 0
    total_bytes = 0
    empty = 0
    try:
        for image, path in sorted(IMAGE_PATHS.items()):
            try:
                log("map", f"image={image} path={path} size={fat.file_size(path)}")
            except Exception as exc:
                log("map_error", f"image={image} path={path} error={exc}")
        for index in range(1, args.sessions + 1):
            packets, bytes_sent, reason = run_session(fat, args, index)
            total_packets += packets
            total_bytes += bytes_sent
            if packets == 0:
                empty += 1
            else:
                empty = 0
            if empty >= args.empty_limit:
                log("summary", f"stop_after_empty_sessions={empty}")
                break
            if reason == "stop":
                log("summary", "stop_after_terminal_packet")
                break
            if index != args.sessions:
                delay = args.after_mode3_delay if reason == "mode3_close" else args.between_sessions
                if delay > 0:
                    log("session", f"{index} sleep={delay} reason={reason}")
                    time.sleep(delay)
    finally:
        fat.close()
        restore_keep_prepared(args, initial_keep_prepared)
    log("summary", f"packets={total_packets} bytes_sent={total_bytes}")
    return 0 if total_packets else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        log("fatal", repr(exc))
        raise
