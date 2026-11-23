#!/usr/bin/env python3
"""Unpacks a fake-signed PS4 PKG (the game, or an update over it) into a game folder, the same
folder a dump has: eboot.bin, sce_module, sce_sys and the game's data.

    python scripts/pkg_extract.py GAME.pkg OUT_FOLDER [--from 0 --to 1 --label 'Unpacking the game']

Prints `progress <0-1> <text>` lines as it goes (scaled to --from..--to). Only fake-signed
packages open: the image key is decrypted with the fake key set, as shadPS4 does (keys in
pkg_keys.py). Needs the cryptography package (AES); the PS4 format notes are in shadPS4's
src/core/file_format/pkg.cpp.
"""
import argparse
import hashlib
import hmac
import struct
import sys
import zlib
from pathlib import Path

from pkg_keys import DERIVED_KEY3, ENTRY_NAMES, FAKE

PKG_MAGIC = 0x7F434E54
SECTOR = 0x1000  # the XTS unit of the PFS image
ENCRYPTED = 0x80000000  # entry flags1: the entry itself is encrypted
FILE, DIRECTORY, CURRENT, PARENT = 2, 3, 4, 5  # dirent types
INODE_SIZE = 0xA8  # unsigned, 32-bit block numbers: the only kind game packages use


class PkgError(Exception):
    pass


def rsa_decrypt(key, data):
    """RSAES-PKCS1-v1_5 with a private key (modulus, exponent)."""
    modulus, exponent = key
    block = pow(int.from_bytes(data, 'big'), exponent, modulus).to_bytes(256, 'big')
    if block[0] != 0 or block[1] != 2 or 0 not in block[2:]:
        raise PkgError('this is not a fake-signed PKG (its keys do not open with the fake key set)')
    return block[block.index(0, 2) + 1:]


class Pkg:
    def __init__(self, path):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        self.cipher, self.algorithms, self.modes = Cipher, algorithms, modes
        self.path = Path(path)
        self.file = open(path, 'rb')
        header = self.file.read(0x1000)
        if len(header) < 0x440 or struct.unpack_from('>I', header, 0)[0] != PKG_MAGIC:
            raise PkgError(f'{self.path.name} is not a PS4 PKG')
        count, table = struct.unpack_from('>I', header, 0x10)[0], struct.unpack_from('>I', header, 0x18)[0]
        self.content_id = header[0x40:0x64].split(b'\0')[0].decode('ascii', 'replace')
        self.title_id = self.content_id[7:16]
        self.pfs_offset, self.pfs_size = struct.unpack_from('>QQ', header, 0x410)
        size = self.path.stat().st_size
        if self.pfs_offset + self.pfs_size > size:
            raise PkgError(f'{self.path.name} is cut short ({size} bytes): download or copy it again')
        self.file.seek(table)
        raw = self.file.read(count * 32)
        self.entries = [(raw[i:i + 32],) + struct.unpack_from('>6I', raw, i) for i in range(0, len(raw), 32)]

    def entry(self, wanted):
        for raw, number, _, flags1, _, offset, size in self.entries:
            if number == wanted:
                return raw, flags1, offset, size
        raise PkgError(f'{self.path.name} has no entry {wanted:#x}')

    def read(self, offset, size):
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise PkgError(f'{self.path.name} is cut short: download or copy it again')
        return data

    def open_image(self):
        """The keys of the PFS image, and where its compressed container (PFSC) starts."""
        _, _, offset, _ = self.entry(0x10)  # ENTRY_KEYS: seed digest, 7 digests, 7 keys
        self.dk3 = rsa_decrypt(DERIVED_KEY3, self.read(offset + 32 + 7 * 32 + 3 * 256, 256))[:32]
        raw, _, offset, _ = self.entry(0x20)  # IMAGE_KEY
        iv_key = hashlib.sha256(raw + self.dk3).digest()
        decryptor = self.cipher(self.algorithms.AES(iv_key[16:]), self.modes.CBC(iv_key[:16])).decryptor()
        image_key = decryptor.update(self.read(offset, 256)) + decryptor.finalize()
        ekpfs = rsa_decrypt(FAKE, image_key)[:32]
        seed = self.read(self.pfs_offset + 0x370, 16)
        digest = hmac.new(ekpfs, struct.pack('<I', 1) + seed, hashlib.sha256).digest()
        self.xts_key = digest[16:32] + digest[:16]  # data key, tweak key
        for position in range(0x20000, self.pfs_size, 0x10000):
            if self.plain(position, 4) == b'PFSC':
                self.pfsc = position
                break
        else:
            raise PkgError(f'{self.path.name}: no game image found inside (not a game or update package?)')
        header = self.plain(self.pfsc, 0x30)
        block_size, offsets, length = struct.unpack_from('<qq', header, 0x10) + struct.unpack_from('<q', header, 0x28)
        self.block_size = block_size
        blocks = length // block_size
        self.sectors = struct.unpack(f'<{blocks + 1}Q', self.plain(self.pfsc + offsets, (blocks + 1) * 8))

    def plain(self, position, size):
        """Bytes of the PFS image, decrypted (AES-XTS, one tweak per 4 KiB sector)."""
        start, end = position & ~(SECTOR - 1), (position + size + SECTOR - 1) & ~(SECTOR - 1)
        raw = self.read(self.pfs_offset + start, end - start)
        out = bytearray()
        for at in range(0, len(raw), SECTOR):
            tweak = ((start + at) // SECTOR).to_bytes(16, 'little')
            out += self.cipher(self.algorithms.AES(self.xts_key), self.modes.XTS(tweak)).decryptor().update(
                raw[at:at + SECTOR])
        return bytes(out[position - start:position - start + size])

    def block(self, number):
        """Block `number` of the game's file system (inside the PFSC), uncompressed."""
        offset, end = self.sectors[number], self.sectors[number + 1]
        if end == offset:
            return bytes(self.block_size)
        data = self.plain(self.pfsc + offset, end - offset)
        if end - offset >= self.block_size:
            return data[:self.block_size]
        return zlib.decompress(data).ljust(self.block_size, b'\0')

    def files(self):
        """(path, size, first block, block count) of every file, and every folder (size None)."""
        super_block = self.block(0)
        mode = struct.unpack_from('<H', super_block, 0x1C)[0]
        if mode & 1:
            raise PkgError(f'{self.path.name}: signed file system images are not supported')
        inode_count, _, inode_blocks, super_root = struct.unpack_from('<4q', super_block, 0x30)
        inodes = []
        for number in range(1, 1 + inode_blocks):
            data = self.block(number)
            for at in range(0, len(data) - INODE_SIZE + 1, INODE_SIZE):
                inodes.append(data[at:at + INODE_SIZE])
        inodes = inodes[:inode_count]

        def listing(ino):
            count, first = struct.unpack_from('<II', inodes[ino], 0x60)
            for number in range(first, first + count):
                data, at = self.block(number), 0
                while at + 16 <= len(data):
                    entry_ino, kind, name_length, entry_size = struct.unpack_from('<4i', data, at)
                    if entry_ino == 0 or entry_size <= 0:
                        break
                    yield entry_ino, kind, data[at + 16:at + 16 + name_length].decode('utf-8', 'replace')
                    at += entry_size

        root = next((ino for ino, kind, name in listing(super_root) if name == 'uroot'), None)
        if root is None:
            raise PkgError(f'{self.path.name}: the game image has no root folder')
        pending = [(root, '')]
        while pending:
            ino, folder = pending.pop()
            for child, kind, name in listing(ino):
                if kind in (CURRENT, PARENT) or not name or '/' in name or name in ('.', '..'):
                    continue
                path = f'{folder}/{name}' if folder else name
                if kind == DIRECTORY:
                    yield path, None, 0, 0
                    pending.append((child, path))
                elif kind == FILE:
                    node = inodes[child]
                    size = struct.unpack_from('<q', node, 8)[0]
                    count, first = struct.unpack_from('<II', node, 0x60)
                    yield path, size, first, count


def extract(pkg_path, out, start=0.0, end=1.0, label='Unpacking', report=None):
    """Unpacks `pkg_path` into `out`; the progress lines go to `report` (default: printed)."""
    say = report or (lambda line: print(line, flush=True))
    pkg = Pkg(pkg_path)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    say(f'progress {start:.3f} {label}: opening {pkg.path.name}')
    pkg.open_image()
    # sce_sys: the package's named entries (param.sfo, the trophies, the icons, the NP files). The
    # encrypted ones with key 3 (nptitle.dat, npbind.dat) open with the entry key; others are left out.
    for raw, number, _, flags1, flags2, offset, size in pkg.entries:
        name = ENTRY_NAMES.get(number)
        if number < 0x400 or not name:
            continue
        data = pkg.read(offset, size)
        if flags1 & ENCRYPTED:
            if (flags2 >> 12) & 0xF != 3:
                continue
            iv_key = hashlib.sha256(raw + pkg.dk3).digest()
            whole = len(data) & ~15
            decryptor = pkg.cipher(pkg.algorithms.AES(iv_key[16:]), pkg.modes.CBC(iv_key[:16])).decryptor()
            data = decryptor.update(data[:whole]) + data[whole:]
        target = out / 'sce_sys' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    listing = list(pkg.files())
    total = sum(size for _, size, _, _ in listing if size) or 1
    done, shown = 0, -1.0
    for path, size, first, count in listing:
        target = out / path
        if size is None:
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, 'wb') as file:
            left = size
            for number in range(first, first + count):
                if left <= 0:
                    break
                data = pkg.block(number)[:left]
                file.write(data)
                left -= len(data)
                done += len(data)
                fraction = start + (end - start) * done / total
                if fraction - shown >= 0.005:
                    shown = fraction
                    say(f'progress {fraction:.3f} {label}: {done / 2**30:.1f} of {total / 2**30:.1f} GB')
        if left > 0:
            raise PkgError(f'{pkg.path.name}: {path} is shorter than its size')
    say(f'progress {end:.3f} {label}: done')
    return pkg.title_id


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('pkg')
    parser.add_argument('out')
    parser.add_argument('--from', dest='start', type=float, default=0.0)
    parser.add_argument('--to', dest='end', type=float, default=1.0)
    parser.add_argument('--label', default='Unpacking')
    args = parser.parse_args()
    try:
        extract(args.pkg, args.out, args.start, args.end, args.label)
    except (PkgError, OSError, zlib.error) as error:
        print(f'pkg_extract: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
