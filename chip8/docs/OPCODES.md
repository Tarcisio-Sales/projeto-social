# As 35 instruções do CHIP-8

Referência completa do conjunto de instruções, na ordem em que aparecem no
`switch` de [`src/chip8.c`](../src/chip8.c).

## Como ler os opcodes

Toda instrução ocupa **16 bits** (2 bytes), armazenados em *big-endian* — o
byte mais significativo primeiro. Os campos são sempre extraídos das mesmas
posições:

```
     15..12   11..8    7..4     3..0
    +-------+-------+-------+-------+
    |   H   |   X   |   Y   |   N   |
    +-------+-------+-------+-------+
            \_______________________/   NNN = endereço de 12 bits
                    \_______________/   NN  = constante de 8 bits
```

| Campo | Bits | Significado |
|---|---|---|
| `H` | 15–12 | Família da instrução. |
| `X` | 11–8 | Índice de um registrador (`V0`..`VF`). |
| `Y` | 7–4 | Índice de um segundo registrador. |
| `N` | 3–0 | Constante de 4 bits. |
| `NN` | 7–0 | Constante de 8 bits. |
| `NNN` | 11–0 | Endereço de 12 bits. |

Um erro fácil de cometer ao ler código CHIP-8: em `DXYN`, o registrador da
coordenada Y é o **terceiro** nibble. `D011` é `DRW V0, V1, 1`; `D001` é
`DRW V0, V0, 1` — ambos válidos, com efeitos bem diferentes.

## Tabela resumida

| Opcode | Mnemônico | Efeito |
|---|---|---|
| `00E0` | `CLS` | Limpa a tela. |
| `00EE` | `RET` | Retorna da sub-rotina. |
| `0NNN` | `SYS addr` | Chamada nativa do RCA 1802 — **ignorada**. |
| `1NNN` | `JP addr` | `pc = NNN`. |
| `2NNN` | `CALL addr` | Empilha o retorno e salta. |
| `3XNN` | `SE Vx, NN` | Pula a próxima instrução se `VX == NN`. |
| `4XNN` | `SNE Vx, NN` | Pula a próxima se `VX != NN`. |
| `5XY0` | `SE Vx, Vy` | Pula a próxima se `VX == VY`. |
| `6XNN` | `LD Vx, NN` | `VX = NN`. |
| `7XNN` | `ADD Vx, NN` | `VX += NN` (**sem** afetar `VF`). |
| `8XY0` | `LD Vx, Vy` | `VX = VY`. |
| `8XY1` | `OR Vx, Vy` | `VX \|= VY`. |
| `8XY2` | `AND Vx, Vy` | `VX &= VY`. |
| `8XY3` | `XOR Vx, Vy` | `VX ^= VY`. |
| `8XY4` | `ADD Vx, Vy` | `VX += VY`; `VF` = houve transporte. |
| `8XY5` | `SUB Vx, Vy` | `VX -= VY`; `VF` = **não** houve empréstimo. |
| `8XY6` | `SHR Vx, Vy` | Desloca 1 bit à direita; `VF` = bit perdido. |
| `8XY7` | `SUBN Vx, Vy` | `VX = VY - VX`; `VF` = não houve empréstimo. |
| `8XYE` | `SHL Vx, Vy` | Desloca 1 bit à esquerda; `VF` = bit perdido. |
| `9XY0` | `SNE Vx, Vy` | Pula a próxima se `VX != VY`. |
| `ANNN` | `LD I, addr` | `I = NNN`. |
| `BNNN` | `JP V0, addr` | `pc = NNN + V0`. |
| `CXNN` | `RND Vx, NN` | `VX = aleatório & NN`. |
| `DXYN` | `DRW Vx, Vy, N` | Desenha sprite por XOR; `VF` = colisão. |
| `EX9E` | `SKP Vx` | Pula a próxima se a tecla `VX` está pressionada. |
| `EXA1` | `SKNP Vx` | Pula a próxima se a tecla `VX` está solta. |
| `FX07` | `LD Vx, DT` | `VX = delay_timer`. |
| `FX0A` | `LD Vx, K` | **Bloqueia** até uma tecla ser digitada. |
| `FX15` | `LD DT, Vx` | `delay_timer = VX`. |
| `FX18` | `LD ST, Vx` | `sound_timer = VX` (bipe enquanto > 0). |
| `FX1E` | `ADD I, Vx` | `I += VX`. |
| `FX29` | `LD F, Vx` | `I` = endereço do sprite do dígito em `VX`. |
| `FX33` | `LD B, Vx` | Grava `VX` em BCD nos 3 bytes a partir de `I`. |
| `FX55` | `LD [I], Vx` | Salva `V0`..`VX` a partir de `I`. |
| `FX65` | `LD Vx, [I]` | Carrega `V0`..`VX` a partir de `I`. |

## Detalhes que importam

### Fluxo de controle

**`2NNN` / `00EE` — chamadas e retornos.** A pilha guarda apenas endereços de
retorno; não existe passagem de argumentos nem quadro de pilha. São 16 níveis:
a 17ª chamada aninhada é um erro (`CHIP8_ERR_STACK_OVERFLOW`), e um `RET` sem
`CALL` correspondente também (`CHIP8_ERR_STACK_UNDERFLOW`). Interpretadores
reais simplesmente corrompiam a memória nesses casos; aqui o erro é reportado,
o que ajuda muito a depurar uma ROM quebrada.

**`3XNN`, `4XNN`, `5XY0`, `9XY0`, `EX9E`, `EXA1` — "pular a próxima".** O
CHIP-8 não tem desvio condicional: tem *skip*. O padrão idiomático é

```
SE V0, 0        ; se V0 == 0, pula o JP abaixo
JP outro_lugar  ; executado só quando V0 != 0
...             ; caminho do V0 == 0
```

Um detalhe importante: o skip avança `pc` em 2 bytes fixos. Se a instrução
pulada fosse de 4 bytes (como no SUPER-CHIP com `F000 NNNN`), o salto cairia no
meio dela — situação que não ocorre no CHIP-8 original.

**`BNNN` — salto com deslocamento.** Comportamento divergente entre
interpretadores; veja [QUIRKS.md](QUIRKS.md).

### Aritmética

Todos os registradores têm **8 bits** e todas as operações são módulo 256.
`7XNN` propositalmente **não** mexe em `VF`, ao contrário de `8XY4`.

O `VF` funciona como registrador de flag, mas continua sendo um registrador
comum: `8FY4` (soma com destino em `VF`) é legal, e nesse caso vale o **flag**,
não a soma — porque o flag é escrito depois do resultado. O núcleo respeita
essa ordem, e há um teste específico para isso.

A convenção de empréstimo em `8XY5`/`8XY7` costuma confundir: `VF = 1` significa
que **não** houve empréstimo. É o oposto de "houve borrow".

A negação de um valor, que o CHIP-8 não tem como instrução, se faz explorando o
estouro de 8 bits — é como a ROM `bola.ch8` inverte a direção:

```
LD  V6, 0
SUB V6, V2      ; V6 = 0 - V2  (1 vira 255, 255 vira 1)
LD  V2, V6
```

### Vídeo

**`DXYN` — a instrução mais complexa do conjunto.** Desenha um sprite de 8
pixels de largura e `N` linhas, lido a partir de `I`, com estas regras:

1. **XOR, não sobrescrita.** Desenhar duas vezes no mesmo lugar apaga — é assim
   que jogos movem objetos sem precisar redesenhar a tela inteira.
2. **`VF` = colisão.** Vale 1 se *qualquer* pixel aceso foi apagado. `VF` é
   zerado antes do desenho, então um `DXYN` sem colisão sempre deixa 0. Jogos
   usam esse flag para detectar acertos.
3. **A posição inicial sempre sofre módulo:** `VX % 64` e `VY % 32`, em todos os
   interpretadores conhecidos.
4. **O que ultrapassa a borda** é cortado ou reaparece do outro lado, conforme o
   quirk `clipping`.
5. Com o quirk `display_wait`, a CPU espera o próximo quadro depois de desenhar.

Um sprite tem sempre 8 pixels de largura. Para desenhar algo mais estreito,
deixam-se os bits de baixo apagados — a bola de 4×4 da demo é `60 F0 F0 60`:

```
0x60 = 0110 0000   .##.
0xF0 = 1111 0000   ####
0xF0 = 1111 0000   ####
0x60 = 0110 0000   .##.
```

**`00E0` — `CLS`.** Apaga tudo e liga `draw_flag`.

### Teclado

**`FX0A` é a única instrução que bloqueia a CPU.** Ela não é um laço: o
interpretador realmente para até uma tecla ser digitada. E o valor só é entregue
quando a tecla é **solta**, não quando é pressionada — foi assim que o
interpretador original evitou que uma única batida de tecla fosse consumida por
vários `FX0A` seguidos.

No núcleo, isso vira o estado `waiting_key`: `chip8_step()` consome o ciclo sem
fazer nada enquanto ele estiver ligado, e `chip8_key_up()` é quem escreve o
registrador e libera a execução. Os temporizadores continuam correndo durante a
espera, como no hardware.

`EX9E`/`EXA1` leem o estado instantâneo do teclado e não bloqueiam. Note que
elas usam apenas os 4 bits baixos de `VX`: um valor maior que 15 é reduzido em
vez de ser tratado como erro.

### Memória e temporizadores

**`FX29` — fonte embutida.** Os sprites dos 16 dígitos hexadecimais ficam em
`0x50`, com 5 bytes cada. `FX29` calcula `I = 0x50 + (VX & 0xF) * 5`. Cada
caractere tem 4 pixels de largura (só os 4 bits altos de cada byte são usados).

**`FX33` — BCD.** Decompõe `VX` em centena, dezena e unidade, gravando um dígito
por byte a partir de `I`. É como jogos mostram pontuação: `FX33` seguido de
`FX65` e três `FX29`+`DXYN`.

**`FX55`/`FX65` — bloco de registradores.** Salvam/carregam `V0` até `VX`
**inclusive** — `F065` movimenta 1 registrador, não zero. Se `I` incrementa ao
final depende do quirk `memory_increment_i`.

**`FX15`/`FX07`/`FX18` — temporizadores.** Ambos contam para trás a 60 Hz,
independentemente da velocidade da CPU. O `delay_timer` é o cronômetro que os
jogos usam para controlar velocidade; o `sound_timer` produz um bipe fixo
enquanto for maior que zero. Não há controle de altura nem de timbre: o CHIP-8
tem um som só.

**`FX1E`** não altera `VF` nesta implementação. O interpretador do Amiga ligava
`VF` quando `I` passava de `0x0FFF`, e uma única ROM conhecida (*Spacefight
2091!*) depende disso.

### Instruções ausentes de propósito

**`0NNN` (`SYS addr`)** chamava código de máquina do RCA 1802 no COSMAC VIP.
Emular isso significaria emular outro processador inteiro; nenhuma ROM moderna
usa. O núcleo trata como no-op em vez de erro, porque algumas ROMs começam com
um `0000` inofensivo.

## Verificando o comportamento

Para conferir uma ROM instrução por instrução:

```bash
./build/chip8-disasm roms/bola.ch8      # listagem estática
./build/chip8-term --debug roms/bola.ch8 2> traco.txt   # traço da execução
```

Vale lembrar que código e dados dividem a mesma memória e o formato `.ch8` não
tem cabeçalho: numa listagem completa, sprites e tabelas aparecem como
instruções estranhas ou como `DW #XXXX`. Isso é característica do formato, não
defeito da ferramenta.
