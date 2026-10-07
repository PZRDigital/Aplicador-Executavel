# Aplicador-Executavel

**Aplicador Codex**: aplica a interface O Codex (login, seleção e criação de personagem, barra de status,
janelas de item, equipamentos, inventário e habilidades, transição rápida) num `Codex.exe` gerado pelo WARP.

## Uso
1. Abra `Aplicador Codex.exe`.
2. Escolha o executável gerado pelo WARP (ou arraste para a janela).
3. Marque as alterações desejadas e clique em **APLICAR INTERFACE**.
4. Copie o conteúdo de `Aplicados\<data e hora>\` (`Codex.exe` + `char.grf`) para a pasta do cliente.

Detalhes, opções e uso pela linha de comando: [LEIA-ME.txt](LEIA-ME.txt).

## Arquivos
- `Aplicador Codex.exe` — o aplicador já compilado (a interface vai embutida nele).
- `char.grf` — dados da interface (mapa 3D, imagens, fontes, tabelas); vai junto com o `Codex.exe` gerado.
- `fonte/` — código do aplicador (Visual Studio 2022, Win32).

## Compilar
- `fonte/` — o aplicador. O `fonte/aplicador.rc` embute a interface compilada
  (`CharSelectPanorama\bin\Release\d3d9.dll`), o logo e as fontes de `CharSelectPanorama\dist\charselect\`.
- `CharSelectPanorama/` — o projeto da interface (C++ em `src/`, ferramentas Python em `tools/`, dados em `dist/charselect/`).
  `base/Codex_warp.exe` é o executável do WARP sem a interface; troque-o ao refazer o diff.

Para gerar tudo de uma vez (interface, `char.grf`, cliente e o próprio aplicador), no WSL:

```
cd CharSelectPanorama
tools/publicar.sh
```

Requer Visual Studio 2022 (MSBuild, Win32), Python 3 com Pillow, numpy e PyYAML, e a estrutura de pastas do NexusRO:
`NexusRO\Aplicador de Executavel\` (este repositório), `NexusRO\Cliente 2025\` e `NexusRO\Emulador\`
(usados para ler as GRFs do cliente e a db do rAthena). Para compilar só o aplicador, use `fonte/Aplicador.vcxproj`
em Release | Win32 depois de compilar a interface (`CharSelectPanorama.vcxproj`).
