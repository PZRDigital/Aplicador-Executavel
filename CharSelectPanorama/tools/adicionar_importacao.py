"""Acrescenta uma DLL (importada pelo ordinal 1) à tabela de importação de um .exe.

Faz o mesmo que o patch de DLLs personalizadas do WARP; serve para testar sem refazer o diff.
Uso: python3 adicionar_importacao.py <entrada.exe> <saída.exe> codex_ui.dll
"""
import struct, sys
import pefile

def align(v, a): return (v + a - 1) // a * a

def main():
    src, dst, dll = sys.argv[1], sys.argv[2], sys.argv[3].encode()
    pe = pefile.PE(src)
    if any(e.dll.lower() == dll.lower() for e in pe.DIRECTORY_ENTRY_IMPORT):
        print('já importa', dll.decode()); pe.write(dst); return
    oh = pe.OPTIONAL_HEADER
    imp_dir = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']]
    old = pe.get_data(imp_dir.VirtualAddress, imp_dir.Size)
    n = len(pe.DIRECTORY_ENTRY_IMPORT)
    descs = old[:n * 20]
    last = pe.sections[-1]
    rva = align(last.VirtualAddress + max(last.Misc_VirtualSize, last.SizeOfRawData), oh.SectionAlignment)
    raw = align(last.PointerToRawData + last.SizeOfRawData, oh.FileAlignment)
    # layout: descritores (n+2)*20 | INT (2*4) | IAT (2*4) | nome
    d_off = 0
    int_off = (n + 2) * 20
    iat_off = int_off + 8
    name_off = iat_off + 8
    body = bytearray(name_off + len(dll) + 1)
    body[0:len(descs)] = descs
    body[n * 20:(n + 1) * 20] = struct.pack('<IIIII', rva + int_off, 0, 0, rva + name_off, rva + iat_off)
    body[int_off:int_off + 8] = struct.pack('<II', 0x80000001, 0)
    body[iat_off:iat_off + 8] = struct.pack('<II', 0x80000001, 0)
    body[name_off:name_off + len(dll)] = dll
    size_raw = align(len(body), oh.FileAlignment)
    data = bytearray(pe.write())
    data[raw:raw] = b''  # garante o tamanho
    if len(data) < raw: data += b'\0' * (raw - len(data))
    data = data[:raw] + bytes(body) + b'\0' * (size_raw - len(body)) + data[raw:]
    # cabeçalho da nova seção
    pe2 = pefile.PE(data=bytes(data))
    sec_off = pe2.sections[-1].get_file_offset() + 40
    hdr = struct.pack('<8sIIIIIIHHI', b'.cximp', len(body), rva, size_raw, raw, 0, 0, 0, 0, 0xC0000040)
    data[sec_off:sec_off + 40] = hdr
    pe3 = pefile.PE(data=bytes(data), fast_load=True)
    pe3.FILE_HEADER.NumberOfSections += 1
    pe3.OPTIONAL_HEADER.SizeOfImage = align(rva + len(body), oh.SectionAlignment)
    d = pe3.OPTIONAL_HEADER.DATA_DIRECTORY
    d[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']].VirtualAddress = rva
    d[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']].Size = (n + 2) * 20
    d[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT']].VirtualAddress = 0
    d[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT']].Size = 0
    pe3.write(dst)
    chk = pefile.PE(dst)
    print([e.dll.decode() for e in chk.DIRECTORY_ENTRY_IMPORT][-3:])

if __name__ == '__main__':
    main()
