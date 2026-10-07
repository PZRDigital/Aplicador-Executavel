"""Embute a interface (codex_ui.dll compilada) dentro do Codex.exe: nenhum .dll na pasta do cliente.

Uso: python3 embutir.py <Codex.exe gerado pelo WARP> <dll> <saída .exe>

Como funciona:
  - a DLL é "montada" (seções nos seus endereços) numa seção nova do executável (.codex),
    já realocada para o endereço final (o Codex.exe não usa ASLR);
  - as importações da DLL entram na tabela de importação do executável (o Windows resolve);
  - o ponto de entrada do executável passa por um trecho que chama a inicialização da DLL
    e depois segue para o início original do jogo.
A importação "codex_ui.dll" (patch de DLL do WARP), se existir, é retirada.
"""
import struct, sys
import pefile

def align(v, a): return (v + a - 1) // a * a

def main():
    src, dllpath, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    exe = pefile.PE(src)
    if any(s.Name.rstrip(b'\0') == b'.codex' for s in exe.sections):
        raise SystemExit('o executável já tem a interface embutida; use o gerado pelo WARP')
    dll = pefile.PE(dllpath)
    eo, do = exe.OPTIONAL_HEADER, dll.OPTIONAL_HEADER
    if eo.DllCharacteristics & 0x40:
        raise SystemExit('o executável usa ASLR (DYNAMIC_BASE); desative no WARP')

    last = exe.sections[-1]
    sec_rva = align(last.VirtualAddress + max(last.Misc_VirtualSize, last.SizeOfRawData), eo.SectionAlignment)
    new_base = eo.ImageBase + sec_rva

    # 1) imagem da DLL montada e realocada
    img = bytearray(do.SizeOfImage)
    img[:do.SizeOfHeaders] = dll.__data__[:do.SizeOfHeaders]
    for s in dll.sections:
        raw = dll.__data__[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
        n = min(len(raw), max(s.Misc_VirtualSize, s.SizeOfRawData), do.SizeOfImage - s.VirtualAddress)
        img[s.VirtualAddress:s.VirtualAddress + n] = raw[:n]
    delta = (new_base - do.ImageBase) & 0xFFFFFFFF
    nrel = 0
    if hasattr(dll, 'DIRECTORY_ENTRY_BASERELOC'):
        for blk in dll.DIRECTORY_ENTRY_BASERELOC:
            for e in blk.entries:
                if e.type == 3:
                    v = struct.unpack_from('<I', img, e.rva)[0]
                    struct.pack_into('<I', img, e.rva, (v + delta) & 0xFFFFFFFF); nrel += 1
                elif e.type != 0:
                    raise SystemExit('realocação de tipo %d não suportada' % e.type)

    # 2) importações da DLL viram RVAs do executável
    descs = []
    imp = do.DATA_DIRECTORY[1]
    off = imp.VirtualAddress
    while True:
        oft, ts, fc, name, ft = struct.unpack_from('<IIIII', img, off)
        if name == 0: break
        for th in (oft, ft):
            if th == 0: continue
            p = th
            while True:
                v = struct.unpack_from('<I', img, p)[0]
                if v == 0: break
                if not v & 0x80000000:
                    struct.pack_into('<I', img, p, v + sec_rva)
                p += 4
            if oft == ft: break
        descs.append(struct.pack('<IIIII', oft + sec_rva if oft else 0, 0, 0, name + sec_rva, ft + sec_rva))
        off += 20

    # 3) nova tabela de importação: a do exe (sem codex_ui.dll) + a da DLL
    eimp = eo.DATA_DIRECTORY[1]
    old = []
    p = eimp.VirtualAddress
    while True:
        d = exe.get_data(p, 20)
        oft, ts, fc, name, ft = struct.unpack('<IIIII', d)
        if name == 0: break
        nm = exe.get_string_at_rva(name).lower()
        if nm != b'codex_ui.dll':
            old.append(struct.pack('<IIIII', oft, 0, 0, name, ft))
        else:
            print('importação codex_ui.dll retirada')
        p += 20
    table = b''.join(old + descs) + b'\0' * 20
    imp_rva = sec_rva + align(len(img), 16)

    # 4) trecho de entrada: DllMain(base, DLL_PROCESS_ATTACH, 0) e salto ao início original
    stub_rva = align(imp_rva + len(table), 16)
    entry_va = new_base + do.AddressOfEntryPoint
    oep_va = eo.ImageBase + eo.AddressOfEntryPoint
    stub = bytearray()
    stub += b'\x6A\x00\x6A\x01\x68' + struct.pack('<I', new_base)
    call_at = eo.ImageBase + stub_rva + len(stub)
    stub += b'\xE8' + struct.pack('<i', entry_va - (call_at + 5))
    jmp_at = eo.ImageBase + stub_rva + len(stub)
    stub += b'\xE9' + struct.pack('<i', oep_va - (jmp_at + 5))

    body = bytearray(stub_rva - sec_rva + len(stub))
    body[:len(img)] = img
    body[imp_rva - sec_rva:imp_rva - sec_rva + len(table)] = table
    body[stub_rva - sec_rva:] = stub

    # 5) seção nova no arquivo
    data = bytearray(exe.__data__)
    raw_ptr = align(len(data), eo.FileAlignment)
    raw_size = align(len(body), eo.FileAlignment)
    data += b'\0' * (raw_ptr - len(data)) + bytes(body) + b'\0' * (raw_size - len(body))
    sh_off = exe.sections[-1].get_file_offset() + 40
    if sh_off + 40 > exe.sections[0].PointerToRawData:
        raise SystemExit('sem espaço no cabeçalho para mais uma seção')
    data[sh_off:sh_off + 40] = struct.pack('<8sIIIIIIHHI', b'.codex', len(body), sec_rva, raw_size, raw_ptr,
                                           0, 0, 0, 0, 0xE0000060)
    out = pefile.PE(data=bytes(data), fast_load=True)
    oh = out.OPTIONAL_HEADER
    out.FILE_HEADER.NumberOfSections += 1
    oh.SizeOfImage = align(sec_rva + len(body), eo.SectionAlignment)
    oh.AddressOfEntryPoint = stub_rva
    dd = oh.DATA_DIRECTORY
    dd[1].VirtualAddress, dd[1].Size = imp_rva, len(table)
    dd[11].VirtualAddress = dd[11].Size = 0   # bound imports
    dd[4].VirtualAddress = dd[4].Size = 0     # assinatura (inválida após a mudança)
    oh.CheckSum = 0
    out.OPTIONAL_HEADER.CheckSum = out.generate_checksum()
    out.write(dst)
    chk = pefile.PE(dst)
    print('%s: interface embutida em 0x%X (%d KB, %d realocações), importa %d DLLs' % (
        dst, new_base, len(body) // 1024, nrel, len(chk.DIRECTORY_ENTRY_IMPORT)))

if __name__ == '__main__':
    main()
