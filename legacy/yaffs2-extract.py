#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
yaffs2-extract.py - extract a yaffs2 image (mkyaffs2image style) into a tree.

The Android 1.6 (Donut) SDK ships its emulator system image as a real yaffs2
filesystem, which is useless to a modern Linux kernel (yaffs2 has been
out-of-tree since 3.14 and linux-3ds does not carry it).  The port needs an
ext2/ext4 /system, so this walker converts one into the other offline: no
kernel yaffs2 support, no unyaffs dependency.

Layout assumed (auto-detected, and what the SDK images actually use):

    chunk = 2048 bytes of data + 64 bytes of spare, stride 2112
    spare[0:16] = struct { u32 sequence, u32 object_id, u32 chunk_id,
                           u32 byte_count }   (little endian)

Object id 1 is the root directory.  Every object has a header chunk (chunk_id
0) holding type/parent/name/mode/uid/gid/size, followed by its data chunks
(chunk_id 1..N).  Directory listings are *not* needed: the tree is rebuilt from
the parent ids and names in the headers, which is how yaffs itself does it.

Usage: yaffs2-extract.py <system.img> <outdir>
"""

import os
import stat
import struct
import sys

TYPES = {0: "unknown", 1: "file", 2: "symlink", 3: "dir", 4: "hardlink",
         5: "special"}


def find_layout(path, size):
    """Return (stride, data_size, spare_size, tag_off) or die."""
    candidates = []
    for stride in (2112, 2064, 528, 4096 + 128, 8192 + 256):
        if size % stride == 0:
            candidates.append(stride)
    for stride in candidates:
        data = 2048 if stride >= 2064 else 512
        spare = stride - data
        with open(path, "rb") as f:
            good = 0
            for i in range(min(16, size // stride)):
                f.seek(i * stride + data)
                sp = f.read(spare)
                for off in (0, 8):
                    if off + 16 > len(sp):
                        continue
                    seq, obj, cid, nbytes = struct.unpack_from("<IIII", sp, off)
                    if 0 < obj < (1 << 24) and cid < (1 << 24) and \
                       (nbytes <= data or nbytes == 0xffff):
                        good += 1
                        break
            if good >= min(16, size // stride):
                return stride, data, spare, off
    raise SystemExit("could not determine the yaffs2 chunk layout")


def hardlink_target(objs, oid):
    """Resolve a yaffs hard link (type 4) to the real object's path."""
    o = objs.get(oid)
    if not o:
        return None
    eq = o.get("equivalent")
    if not eq:
        return None
    t = objs.get(eq)
    return t.get("path") if t else None


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    img, outdir = sys.argv[1], sys.argv[2]
    size = os.path.getsize(img)
    stride, data_sz, spare_sz, tag_off = find_layout(img, size)
    chunks = size // stride
    print(f"{img}: {size} bytes, {chunks} chunks, stride {stride} "
          f"(data {data_sz}, spare {spare_sz}, tags at +{tag_off})")

    objs = {}        # id -> dict(header/datachunks)
    order = []       # chunk ids of data chunks per object

    with open(img, "rb") as f:
        for i in range(chunks):
            f.seek(i * stride)
            chunk = f.read(stride)
            if len(chunk) < stride:
                break
            seq, obj, cid, nbytes = struct.unpack_from(
                "<IIII", chunk, data_sz + tag_off)
            if obj == 0 or obj == 0xffffffff:
                continue
            if cid == 0:
                # object header
                d = chunk[:data_sz]
                otype, parent = struct.unpack_from("<II", d, 0)
                name = d[10:10 + 256].split(b"\x00", 1)[0]
                # Object header offsets, verified byte by byte against this
                # image.  The u16 after parent_obj_id is followed by 2 bytes
                # of struct padding, so everything from st_mode on sits 2
                # bytes later than a naive field sum suggests:
                #   mode@268 uid@272 gid@276 atime@280 mtime@284 ctime@288
                #   file_size@292 equivalent_obj_id@296 alias[]@300
                # e.g. /system/bin/dd: mode 0xa1ed, gid 2000, alias "toolbox"
                mode, uid, gid, atime, mtime, ctime, fsize, equiv = \
                    struct.unpack_from("<8I", d, 268)
                alias = d[300:300 + 4 * 159].split(b"\x00", 1)[0]
                objs[obj] = {
                    "type": otype, "parent": parent,
                    "name": name.decode("utf-8", "replace"),
                    "mode": mode, "uid": uid, "gid": gid, "size": fsize,
                    "equivalent": equiv, "alias": alias,
                    "chunks": {},
                }
                order.append(obj)
            else:
                o = objs.get(obj)
                if o is None:
                    continue          # deleted / stale chunk
                o["chunks"][cid] = chunk[:min(data_sz, nbytes)]

    # resolve paths
    root = objs.get(1)
    if not root:
        raise SystemExit("no root object (id 1) in the image")

    def path_of(oid, depth=0):
        if oid == 1 or depth > 64:
            return ""
        o = objs.get(oid)
        if o is None:
            return None
        p = path_of(o["parent"], depth + 1)
        if p is None:
            return None
        return os.path.join(p, o["name"]) if p else o["name"]

    counts = {}
    total = 0
    skipped = []
    os.makedirs(outdir, exist_ok=True)

    for oid, o in sorted(objs.items()):
        rel = path_of(oid)
        if rel is None or not rel:
            continue
        o["path"] = os.path.join(outdir, rel)
        dst = os.path.join(outdir, rel)
        kind = TYPES.get(o["type"], f"type{o['type']}")
        counts[kind] = counts.get(kind, 0) + 1
        payload = b"".join(o["chunks"][k] for k in sorted(o["chunks"]))

        if o["type"] == 3:                                  # directory
            os.makedirs(dst, exist_ok=True)
        elif o["type"] == 2:                                # symlink
            # yaffs stores the target in the header's alias[] field (verified
            # against this image: /system/bin/dd -> "toolbox" at offset 298).
            target = o.get("alias", b"") or payload.split(b"\x00", 1)[0]
            if not target:
                skipped.append((rel, "empty symlink target"))
                continue
            os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
            if os.path.lexists(dst):
                os.remove(dst)
            try:
                os.symlink(target.decode("utf-8", "replace"), dst)
            except OSError as exc:
                skipped.append((rel, str(exc)))
                continue
        elif o["type"] == 1:                                # file
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as g:
                g.write(payload)
            total += len(payload)
        elif o["type"] == 4:                                # hard link
            src = hardlink_target(objs, oid)
            if src is None:
                skipped.append((rel, "unresolved hard link"))
                continue
            os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
            if os.path.lexists(dst):
                os.remove(dst)
            try:
                os.link(src, dst)
            except OSError:
                try:
                    with open(src, "rb") as a, open(dst, "wb") as b:
                        b.write(a.read())
                except OSError as e:
                    skipped.append((rel, str(e)))
        try:
            os.chmod(dst, stat.S_IMODE(o["mode"]))
            os.chown(dst, o["uid"], o["gid"])
        except (OSError, KeyError):
            pass

    print(f"objects: {len(objs)}, extracted: {counts}, file bytes: {total}")
    if skipped:
        print(f"skipped {len(skipped)} entries, first few:")
        for rel, why in skipped[:5]:
            print(f"   {rel}: {why}")


if __name__ == "__main__":
    main()
