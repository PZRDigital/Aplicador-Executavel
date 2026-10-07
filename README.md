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
O `fonte/aplicador.rc` embute arquivos do projeto da interface, que **não está neste repositório**:
`..\..\CharSelectPanorama\bin\Release\d3d9.dll` (a interface compilada) e o logo e as fontes de
`..\..\CharSelectPanorama\dist\charselect\`. Para recompilar, deixe a pasta `CharSelectPanorama`
ao lado desta (em `NexusRO\`) e compile `fonte/Aplicador.vcxproj` em Release | Win32; o executável sai na raiz.
