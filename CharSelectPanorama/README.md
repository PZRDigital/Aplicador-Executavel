# CharSelectPanorama (NexusRO)

Seleção de personagem panorâmica para o `Codex.exe` (cliente 2025-06-04).
É um **proxy de `d3d9.dll`**: o cliente carrega a DLL da própria pasta, ela repassa
tudo ao `d3d9.dll` do Windows e intercepta o desenho da tela de seleção.

## Distribuição: só Codex.exe + char.grf

A interface vai **embutida no executável** (seção `.codex`) e os dados (mapa 3D, ícones, fontes,
`charselect.ini`, tabelas) ficam na **`char.grf`** (`data\charselect\...`). Nenhuma DLL e nenhuma
pasta `charselect\` no cliente. Arquivos do jogador (favoritos, preferência personalizada/original,
log) ficam em `savedata\charselect\`.

Para atualizar: `tools/publicar.sh` (compila, embute em `base/Codex_warp.exe` e gera a `char.grf`).
- `base/Codex_warp.exe` = executável gerado pelo WARP **sem** a interface. Ao refazer o diff no WARP,
  substitua esse arquivo e rode o script de novo (a importação `codex_ui.dll`, se houver, é retirada).
- `tools/embutir.py`: monta a DLL compilada numa seção nova, realocada para o endereço fixo do exe
  (o Codex.exe não usa ASLR), junta as importações dela às do exe e desvia o ponto de entrada para
  inicializá-la antes do jogo. Compilada com `/Zc:threadSafeInit-` (sem TLS).
- `tools/criar_grf.py`: empacota `dist/charselect` em GRF 0x200 (zlib).
- Desenvolvimento: se existir uma pasta `charselect\` no cliente, os arquivos dela têm prioridade
  sobre a GRF (permite testar sem regerar a GRF). A mesma DLL ainda funciona como `d3d9.dll` (proxy)
  ou `codex_ui.dll` (patch de DLL do WARP).

## Como funciona

| Peça | Onde | O quê |
|---|---|---|
| Slot selecionado | `0x15D0FC2` (byte) | mesmo valor enviado no pacote `0x0066` (CH_SELECT_CHAR) |
| Janela aberta/fechada | vtable `UINewSelectCharWnd` `0xFFD194` | hooks em `OnCreate` (idx 15) e no destrutor (idx 0) |
| Fundo | `IDirect3DDevice9` | o primeiro desenho em tela cheia do frame é trocado pela cena panorâmica |

Os endereços valem **só** para este executável. Na inicialização a DLL confere
assinaturas; se o `Codex.exe` mudar, ela desativa os hooks e registra no log
em vez de travar o cliente.

## Compilar

1. Abra `CharSelectPanorama.vcxproj` no Visual Studio (2022 = toolset v143; em outra
   versão aceite o "Retarget").
2. Selecione **Release | Win32** (tem que ser 32 bits).
3. Compile: sai `bin\Release\d3d9.dll`.

## Instalar no cliente

```
Cliente 2025\
  Codex.exe
  d3d9.dll                  <- bin\Release\d3d9.dll
  charselect\
    charselect.ini          <- dist\charselect\charselect.ini
    scenes\
      teste_grade.jpg
      1@def02.jpg           <- suas capturas do mapa
```

Para desinstalar, basta apagar o `d3d9.dll` da pasta do cliente.

## Primeiro teste (diagnóstico)

Com `debug=1`, abra o cliente, entre na seleção e troque de slot algumas vezes.
O arquivo `charselect\charselect.log` registra:

- se o executável foi reconhecido e os hooks instalados;
- o dump da `UINewSelectCharWnd` e do primeiro `UISlotForSelectWnd` (para a fase 2);
- as chamadas de desenho de alguns frames, marcando `<== FUNDO SUBSTITUIDO`.

Se o fundo não trocar, mande o log: ele mostra qual desenho é o fundo do cliente,
e dá para ajustar pelo `textura_fundo=LxA` no `.ini`, sem recompilar.
`modo=over` desenha a cena por cima de tudo, só para conferir se as imagens carregam.

## Mapa 3D (seção `[mapa3d]`)

Com `ativo=1`, o fundo deixa de ser uma imagem e passa a ser o **mapa de verdade em 3D**
(hoje o 2@exds), desenhado pela própria DLL com câmera livre:

- a câmera fica "deitada" (`inclinacao`), segue o personagem selecionado com molas
  (arranca e freia suave), faz um arco e gira de lado na troca, e orbita/respira devagar parada;
- colisão: se uma árvore/pedra entra entre o personagem e a câmera, ela fica na frente do obstáculo;
- personagens com perspectiva (tamanho pela distância), desenhados do mais longe ao mais perto;
  quem está mais perto da câmera que o selecionado fica translúcido;
- céu noturno com estrelas, nuvens em volta da ilha, halo no sol/lua e brilhos nos emissores do mapa.

Gerar outro mapa (Linux/WSL, Python 3 com numpy e Pillow):

```
python3 tools/mapa3d.py "<pasta do cliente>" <mapa> "<cliente>/charselect/mapa3d/<mapa>"
python3 tools/posicoes3d.py "<cliente>/charselect/mapa3d/<mapa>" 15 22 120 14
```

O primeiro converte RSW/GND/GAT/RSM (1.x e 2.x) numa malha estática com a luz aplicada
(`mapa.bin`, texturas `tNNN.png`, `luz.png`, `gat.bin`, nuvens). O segundo escolhe células
andáveis livres e a direção de câmera de cada slot com linha de visão livre; cole a saída em
`[posicoes3d]` (`slot=x,y,direcao`). `tools/compor3d.py` e `tools/preview3d.py` renderizam
prévias offline (moderngl) com a mesma câmera da DLL.

## Personagem selecionado, balão e criação

- **Modo de combate:** o boneco selecionado usa a ação `acao_selecionado` (32 = postura de combate) e,
  a cada `intervalo_golpe_ms`, dá um golpe (`acao_golpe`, 40). A DLL grava a ação em
  `[UINewSelectCharWnd+0xE8] + slot*0x15C + 0x14C` e desvia `CActRes::GetMotion` (`0x70D9D0`) para o quadro
  dar a volta pelo total de cada camada.
- **Balão:** selo de engrenagem com o ícone da classe (`charselect\icones\<classe>.png`, gerados por
  `tools/icones.py` a partir de `renewalparty\icon_jobs_*.bmp`), nome e classe.
- **Criação:** a janela nativa (`UINewMakeCharWnd`) fica escondida e recebe os cliques da interface nova.
  Estado lido dela: direção `+0x1E0`, penteado `+0x246`, cor `+0x258`, sexo `+0x29E`, nome `[[+0x2A0]+0xD8]`.
  Os penteados são recortados das texturas da própria janela; o personagem novo é desenhado no mapa pela
  rotina da seleção (`0x7AB690` / `0x7ABCA0` / `0x798BC0`) no lugar do slot escolhido.

## Cenas do 1@def02 (modo imagem, `[mapa3d] ativo=0`)

Qualquer PNG/JPG/BMP serve. Duas formas de montar:

- **Uma imagem grande (panorâmica):** todos os slots apontam para o mesmo arquivo
  com focos diferentes, e a câmera desliza pelo mapa entre um slot e outro.
- **Uma imagem por slot:** a troca esmaece de uma cena para a outra, com uma leve
  aproximação.

Formato no `[cenas]`: `slot=arquivo,focoX,focoY,zoom`. A `teste_grade.jpg`
mostra as coordenadas na tela, o que ajuda a escolher os focos.

## Roteiro

- [x] **Fase 1:** fundo panorâmico que muda por slot
- [x] **Fase 2:** interface no tema O Codex. A grade nativa é escondida; o personagem
      selecionado é redesenhado ampliado na cena (camadas nativas: elmos, robe/asa);
      os botões novos acionam os controles nativos por redirecionamento do mouse
- [x] **Fase 3:** mapa 3D de verdade (2@exds) com câmera livre
- [ ] aura dos personagens

## Arquivos da pasta `charselect\`

| Arquivo | Uso |
|---|---|
| `charselect.ini` | configuração (cenas, câmera, interface, posições nativas) |
| `classes.txt`, `mapas.txt` | nomes em pt-BR (gerados do `pcjobnamegender.lub` e do `mapnametable.txt`) |
| `fonts\Cinzel.ttf`, `fonts\Inter.ttf` | fontes do site (licença OFL) |
| `img\logo.png` | logo do O Codex |
| `scenes\` | imagens do mapa (modo imagem) |
| `mapa3d\<mapa>\` | mapa 3D convertido por `tools\mapa3d.py` |
