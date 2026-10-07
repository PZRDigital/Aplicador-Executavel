"""Leitor mínimo de GRF (0x200 e 0x300) com a ordem de prioridade do DATA.INI."""
import os, struct, zlib, lzma

class Grf:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        h = self.f.read(46)
        if h[42:46] not in (b'\x00\x02\x00\x00', b'\x00\x03\x00\x00'):
            raise ValueError('não é GRF: ' + path)
        self.version = struct.unpack_from('<I', h, 42)[0]
        self.entries = {}
        if self.version == 0x300:
            off = struct.unpack_from('<Q', h, 30)[0]
            self.f.seek(off + 46)
            self.f.read(4)
        else:
            off = struct.unpack_from('<I', h, 30)[0]
            self.f.seek(off + 46)
        clen, rlen = struct.unpack('<II', self.f.read(8))
        table = zlib.decompress(self.f.read(clen))
        p = 0
        while p < len(table):
            e = table.index(b'\0', p)
            name = table[p:e]
            p = e + 1
            if self.version == 0x300:
                cl, cla, rl, fl, of = struct.unpack_from('<IIIBQ', table, p); p += 21
            else:
                cl, cla, rl, fl, of = struct.unpack_from('<IIIBI', table, p); p += 17
            if fl & 1:
                self.entries[name.lower().replace(b'/', b'\\')] = (cl, cla, rl, fl, of, name)

    def read(self, key):
        cl, cla, rl, fl, of, name = self.entries[key]
        self.f.seek(of + 46)
        data = self.f.read(cla)
        if fl & 6:
            import grfdes
            data = grfdes.decode(data, fl, cl)
        if cl == rl and data[:cl] and False:
            return data[:rl]
        try:
            return zlib.decompress(data[:cl])
        except zlib.error:
            pass
        # GRF Editor: LZMA
        try:
            return lzma.decompress(data[:cl])
        except Exception:
            pass
        try:
            props = data[0]; dsz = struct.unpack_from('<I', data, 1)[0]
            filt = lzma._decode_filter_properties(lzma.FILTER_LZMA1, data[0:5])
            return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[filt]).decompress(data[5:cl])[:rl]
        except Exception as ex:
            raise ValueError('não consegui descomprimir %r: %s' % (name, ex))

class Vfs:
    """Pasta data\\ primeiro, depois as GRFs na ordem do DATA.INI."""
    def __init__(self, client_dir, grfs=None):
        self.dir = client_dir
        if grfs is None:
            grfs = []
            for line in open(os.path.join(client_dir, 'DATA.INI'), encoding='latin-1'):
                if '=' in line and line.split('=')[0].strip().isdigit():
                    grfs.append(line.split('=', 1)[1].strip())
        self.grfs = [Grf(os.path.join(client_dir, g)) for g in grfs]

    @staticmethod
    def key(name):
        if isinstance(name, str):
            name = name.encode('cp949')
        return name.lower().replace(b'/', b'\\')

    def read(self, name):
        k = self.key(name)
        local = os.path.join(self.dir, k.decode('cp949').replace('\\', os.sep))
        if os.path.isfile(local):
            return open(local, 'rb').read()
        errs = []
        for g in self.grfs:
            if k in g.entries:
                try:
                    return g.read(k)
                except ValueError as ex:
                    errs.append(str(ex))
        raise KeyError('%s %s' % (k.decode('cp949', 'replace'), errs))

    def exists(self, name):
        k = self.key(name)
        return any(k in g.entries for g in self.grfs)
