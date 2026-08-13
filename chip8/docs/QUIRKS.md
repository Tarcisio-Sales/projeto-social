# Quirks: por que duas ROMs corretas exigem emuladores diferentes

O CHIP-8 nunca teve especificação formal. O que existe é o comportamento do
interpretador original do COSMAC VIP (1977) e o de dezenas de reimplementações
que discordam em pontos específicos. Uma ROM escrita para um interpretador pode
simplesmente não funcionar em outro — não por bug do emulador, mas porque os
dois estão certos segundo tradições diferentes.

Este emulador não escolhe um lado: as seis divergências conhecidas são campos
de `chip8_quirks_t`, com dois conjuntos prontos.

```bash
./build/chip8-term --quirks vip    jogo.ch8   # padrão: COSMAC VIP (1977)
./build/chip8-term --quirks schip  jogo.ch8   # SUPER-CHIP / emuladores modernos
```

| Quirk | `vip` | `schip` |
|---|---|---|
| `vf_reset` | ligado | desligado |
| `memory_increment_i` | ligado | desligado |
| `display_wait` | ligado | desligado |
| `clipping` | ligado | ligado |
| `shift_vx_in_place` | desligado | ligado |
| `jump_with_offset_vx` | desligado | ligado |

**Sintoma típico:** se uma ROM roda mas se comporta de forma errada — sprites no
lugar errado, pontuação embaralhada, jogo travando após alguns segundos —,
trocar o conjunto de quirks é a primeira coisa a tentar.

---

## 1. `vf_reset` — as operações lógicas zeram `VF`

**Instruções afetadas:** `8XY1` (OR), `8XY2` (AND), `8XY3` (XOR).

No COSMAC VIP, essas três instruções deixavam `VF` zerado como efeito colateral
do hardware. A maioria dos emuladores modernos não faz isso.

```c
V0 = 0x0F;  V1 = 0xF0;  VF = 1;
OR V0, V1;
/* VIP:   V0 = 0xFF, VF = 0   */
/* SCHIP: V0 = 0xFF, VF = 1   */
```

**Quando importa:** ROMs que fazem uma operação lógica logo depois de uma
aritmética e leem `VF` em seguida. Raro, mas quando quebra, quebra feio.

---

## 2. `memory_increment_i` — `FX55`/`FX65` incrementam `I`

**Instruções afetadas:** `FX55` (salvar registradores), `FX65` (carregar).

No VIP, `I` terminava valendo `I + X + 1`; no SUPER-CHIP, ficava intacto.

```c
I = 0x400;
LD [I], V3;
/* VIP:   I = 0x404  */
/* SCHIP: I = 0x400  */
```

**Quando importa:** muito. ROMs do VIP costumam salvar blocos em sequência
contando com o incremento automático:

```
LD [I], V3      ; grava 4 bytes e avança I
LD [I], V3      ; grava os 4 seguintes
```

Com o quirk errado, o segundo bloco sobrescreve o primeiro. É a causa mais
comum de "a ROM roda, mas os dados saem errados".

---

## 3. `display_wait` — `DXYN` espera o retraço vertical

**Instrução afetada:** `DXYN`.

No hardware original, o desenho acontecia durante o *vertical blank*: a CPU
ficava parada até o próximo quadro, limitando o desenho a um sprite por quadro
(60 sprites por segundo).

**Quando importa:** ROMs antigas que dependem disso para não piscar. Sem a
espera, um jogo que apaga e redesenha um sprite dentro do mesmo quadro produz
tremulação visível. Por outro lado, ROMs modernas que desenham muitos sprites
por quadro ficam lentíssimas *com* a espera ligada.

No núcleo, isso é implementado com a flag `vblank_wait`: `DXYN` a liga, e
`chip8_run_frame()` encerra o quadro ao vê-la.

---

## 4. `clipping` — sprites cortados ou reaparecendo do outro lado

**Instrução afetada:** `DXYN`.

Um sprite desenhado perto da borda direita ou inferior pode ser **cortado**
(o excedente some) ou **reaparecer** do lado oposto.

```
clipping ligado:                 clipping desligado:
  ..............####               ####..........####
                  ↑ cortado          ↑ reapareceu à esquerda
```

**Atenção a uma sutileza:** a **posição inicial** do sprite sempre sofre módulo
(`VX % 64`, `VY % 32`), independentemente deste quirk — nisso todos os
interpretadores concordam. O quirk controla apenas o que acontece com a parte
do sprite que ultrapassa a borda.

**Quando importa:** quase todas as ROMs esperam corte. É por isso que ambos os
conjuntos prontos o deixam **ligado**. O reaparecimento existe principalmente
para ROMs de teste e demos.

---

## 5. `shift_vx_in_place` — `8XY6`/`8XYE` ignoram `VY`

**Instruções afetadas:** `8XY6` (SHR), `8XYE` (SHL).

Essa é a divergência mais famosa, e nasceu de um erro de documentação nos anos
1990 que se espalhou por praticamente todos os emuladores.

```c
/* VIP: copia VY, desloca, guarda em VX */
VX = VY >> 1;    VF = VY & 1;

/* SCHIP: desloca o próprio VX, ignorando VY */
VX = VX >> 1;    VF = VX & 1;
```

**Quando importa:** sempre que a ROM usa deslocamento. ROMs escritas para o VIP
tipicamente fazem `SHR V0, V1` esperando operar sobre `V1`; com o quirk errado,
`V1` é ignorado e o resultado é lixo. ROMs modernas fazem `SHR V0, V0` (com
`X == Y`), que funciona **nos dois modos** — foi assim que a divergência passou
tanto tempo despercebida.

---

## 6. `jump_with_offset_vx` — `BNNN` vira `BXNN`

**Instrução afetada:** `BNNN`.

```c
/* VIP:   BNNN — pc = NNN + V0  */
/* SCHIP: BXNN — pc = NN + VX   */
```

No SUPER-CHIP, o nibble que seria parte do endereço passou a selecionar o
registrador, o que reduz o alcance do salto mas permite escolher o índice.

**Quando importa:** pouquíssimas ROMs usam `BNNN`, mas as que usam quebram
imediatamente e de forma espetacular (salto para um endereço aleatório) com o
quirk errado.

---

## Verificando o emulador

A "CHIP-8 test suite" de Timendus é a referência para conferir cada um desses
comportamentos, e testa exatamente os seis pontos acima. Este projeto não
redistribui essas ROMs, mas o emulador foi escrito contra os comportamentos que
elas verificam, e a suíte de testes em
[`tests/test_chip8.c`](../tests/test_chip8.c) cobre cada quirk nos dois estados:

```bash
make test
```

```
- quirk: VF zerado pelas operacoes logicas
- quirk: deslocamento no lugar
- quirk: BNNN x BXNN
- quirk: corte x reaparecimento nas bordas
- quirk: DXYN espera o sincronismo vertical
```

## Configurando de dentro do código

Os conjuntos prontos são pontos de partida; cada campo é ajustável
individualmente:

```c
chip8_t vm;
chip8_quirks_t q = chip8_quirks_vip();

q.display_wait = 0;   /* VIP, mas sem esperar o vsync: acelera o desenho */
q.clipping = 0;       /* e com sprites reaparecendo do outro lado        */

chip8_init(&vm, &q);
```
