import argparse
import fnmatch
import os
import struct
import sys

SECTOR = 2048


class Layer:
    def __init__(self, index, base, pvd):
        self.index = index
        self.base = base
        self.volume = pvd[40:72].decode('ascii', 'replace').strip()
        self.sectors = struct.unpack_from('<I', pvd, 80)[0]
        self.root = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]


class Image:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.size = os.path.getsize(path)
        self.layers = [Layer(0, 0, self.pvd(16))]
        first = self.layers[0].sectors
        for sector in range(first, min(first + 64, self.size // SECTOR)):
            if self.read(sector, 1)[:6] == b'\x01CD001':
                self.layers.append(Layer(1, sector - 16, self.read(sector, 1)))
                break

    def read(self, sector, count):
        self.file.seek(sector * SECTOR)
        return self.file.read(count * SECTOR)

    def pvd(self, sector):
        data = self.read(sector, 1)
        if data[:6] != b'\x01CD001':
            sys.exit('not an ISO 9660 image: no primary volume descriptor at sector %d' % sector)
        return data

    def files(self):
        for layer in self.layers:
            yield from self.walk(layer, layer.root[0], layer.root[1], '')

    def walk(self, layer, lba, size, prefix):
        data = self.read(layer.base + lba, (size + SECTOR - 1) // SECTOR)
        offset = 0
        while offset < size:
            length = data[offset]
            if length == 0:
                offset = (offset // SECTOR + 1) * SECTOR
                continue
            record = data[offset:offset + length]
            offset += length
            extent, extent_size = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
            name = record[33:33 + record[32]]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('ascii', 'replace').split(';')[0]
            path = prefix + name
            if record[25] & 2:
                yield from self.walk(layer, extent, extent_size, path + '/')
            else:
                yield path, layer, layer.base + extent, extent_size

    def find(self, path):
        wanted = path.replace('\\', '/').strip('/').upper()
        for entry in self.files():
            if entry[0].upper() == wanted:
                return entry
        return None

    def copy(self, entry, destination):
        path, _, sector, size = entry
        os.makedirs(os.path.dirname(destination) or '.', exist_ok=True)
        self.file.seek(sector * SECTOR)
        with open(destination, 'wb') as out:
            left = size
            while left:
                chunk = self.file.read(min(left, 1 << 22))
                out.write(chunk)
                left -= len(chunk)


def info(image):
    for layer in image.layers:
        print('layer %d: volume %s, base sector %d, %d sectors' % (layer.index, layer.volume, layer.base, layer.sectors))
    entry = image.find('SYSTEM.CNF')
    if entry:
        image.file.seek(entry[2] * SECTOR)
        print(image.file.read(entry[3]).decode('ascii', 'replace').strip())


def listing(image):
    for path, layer, sector, size in image.files():
        print('%d %10d %12d  %s' % (layer.index, sector, size, path))


def extract(image, destination, patterns, limit):
    count = 0
    for entry in image.files():
        path, _, _, size = entry
        if patterns and not any(fnmatch.fnmatch(path.upper(), p.upper()) for p in patterns):
            continue
        if not patterns and limit and size > limit:
            print('left in the image: %s (%d MiB)' % (path, size >> 20))
            continue
        image.copy(entry, os.path.join(destination, path))
        count += 1
    print('%d files extracted to %s' % (count, destination))


def main():
    parser = argparse.ArgumentParser(description='Read files from PS2 CD and DVD images, including both layers of a DVD9.')
    parser.add_argument('command', choices=['info', 'list', 'extract'])
    parser.add_argument('image')
    parser.add_argument('destination', nargs='?')
    parser.add_argument('patterns', nargs='*', help='paths or globs to extract, everything below --limit if omitted')
    parser.add_argument('--limit', type=int, default=64, help='MiB; larger files stay in the image unless named')
    args = parser.parse_args()
    image = Image(args.image)
    if args.command == 'info':
        info(image)
    elif args.command == 'list':
        listing(image)
    else:
        if not args.destination:
            parser.error('extract needs a destination folder')
        extract(image, args.destination, args.patterns, args.limit << 20)


if __name__ == '__main__':
    main()
