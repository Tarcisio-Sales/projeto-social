# Emulador CHIP-8

Emulador completo do CHIP-8 escrito em **C99 puro**, com núcleo sem nenhuma
dependência externa, dois frontends, desmontador, suíte de testes e ROMs de
demonstração próprias.

O CHIP-8 não é um console de verdade: é uma **máquina virtual** criada por
Joseph Weisbecker em 1977 para o microcomputador COSMAC VIP, com o objetivo de
deixar a escrita de jogos mais fácil do que em assembly do RCA 1802. São 35
instruções, 4 KiB de memória e uma tela de 64×32 pixels em preto e branco —
justamente por ser tão pequeno, virou o "olá, mundo" de quem escreve
emuladores.

```
  ████    ▄█   ▀▀▀█  ▀▀▀█        ← a ROM de demonstração `fonte.ch8`
  █  █    █   █▀▀▀  ▀▀▀█           desenhando os 16 dígitos hexadecimais
  ████   ▀▀▀  ▀▀▀▀  ▀▀▀▀           da fonte embutida
```

## O que está incluído

| Componente | Arquivo | Descrição |
|---|---|---|
| Núcleo | `src/chip8.c`, `include/chip8.h` | As 35 instruções, temporizadores, vídeo, teclado. Zero dependências. |
| Desmontador | `src/disasm.c` | Converte opcodes em assembly legível. |
| Frontend de terminal | `frontend/main_term.c` | Roda em qualquer console POSIX, sem bibliotecas. |
| Frontend gráfico | `frontend/main_sdl.c` | SDL2: janela redimensionável, som real, teclas soltas de verdade. |
| Ferramenta `chip8-disasm` | `tools/disasm_main.c` | Lista uma ROM inteira em assembly. |
| Gerador de ROMs | `tools/mkroms.c` | Monta as três ROMs de demonstração. |
| Testes | `tests/test_chip8.c` | 176 verificações cobrindo instruções, quirks e casos de borda. |

## Começando

```bash
cd chip8
make          # compila tudo e gera as ROMs de demonstração
make test     # roda a suíte de testes
make run      # abre a demo da bola no terminal
```

O `make` compila o frontend gráfico apenas se encontrar o SDL2; sem ele, o
resto do projeto compila normalmente e o `make` avisa o que instalar:

```bash
sudo apt install libsdl2-dev     # Debian / Ubuntu
sudo dnf install SDL2-devel      # Fedora
brew install sdl2                # macOS
```

### Rodando uma ROM

```bash
./build/chip8-term jogo.ch8              # no terminal
./build/chip8-sdl  jogo.ch8              # em janela (precisa de SDL2)
./build/chip8-sdl  --quirks schip --ipf 30 jogo.ch8
./build/chip8-disasm jogo.ch8            # lista a ROM em assembly
```

Opções aceitas pelos dois frontends:

| Opção | Efeito |
|---|---|
| `--ipf N` | Instruções por quadro (padrão 11, os ~660 Hz do COSMAC VIP). |
| `--quirks vip\|schip` | Conjunto de compatibilidade — veja [docs/QUIRKS.md](docs/QUIRKS.md). |
| `--seed N` | Semente do gerador aleatório (0 = relógio). Fixar a semente torna a partida reprodutível. |
| `--debug` | Imprime o traço desmontado das instruções em `stderr`. |
| `--scale N` | *(só SDL)* Escala inicial da janela. |
| `--mute` | *(só SDL)* Desliga o som. |

Se um jogo parecer lento demais ou rápido demais, ajuste `--ipf`: não existe
uma velocidade "correta" de CPU no CHIP-8, e cada ROM foi escrita presumindo
uma diferente.

### Teclado

O CHIP-8 usa um teclado hexadecimal de 16 teclas, mapeado no bloco à esquerda
do teclado QWERTY:

```
   CHIP-8              Teclado
  1 2 3 C             1 2 3 4
  4 5 6 D     <==     Q W E R
  7 8 9 E             A S D F
  A 0 B F             Z X C V
```

Controles do emulador: `ESC` sai, `TAB` reinicia a ROM, `ESPAÇO` pausa.
No frontend SDL, `-` e `+` ajustam a velocidade ao vivo.

> **Limitação do frontend de terminal:** um console entrega apenas "tecla
> digitada", nunca "tecla solta". O frontend solta cada tecla automaticamente
> após 220 ms sem repetição, o que funciona para a maioria dos jogos, mas
> atrapalha os que exigem duas teclas ao mesmo tempo. Para esses, use o
> frontend SDL.

## ROMs de demonstração

As ROMs clássicas (PONG, TETRIS, INVADERS…) circulam sem licença clara, então
este projeto não as redistribui. Em vez disso, `make roms` gera três programas
escritos aqui mesmo, comentados instrução por instrução em
[`tools/mkroms.c`](tools/mkroms.c):

| ROM | O que faz | O que exercita |
|---|---|---|
| `roms/fonte.ch8` | Desenha os 16 dígitos hexadecimais em grade 4×4. | `FX29`, `DXYN`, saltos condicionais. |
| `roms/bola.ch8` | Bola quicando nas quatro bordas, com bipe a cada batida. | XOR, `CALL`/`RET`, temporizadores de atraso e de som, aritmética de 8 bits. |
| `roms/teclado.ch8` | Mostra o dígito da última tecla pressionada. | `FX0A` (a única instrução que bloqueia a CPU), `00E0`. |

Qualquer ROM `.ch8` de terceiros funciona normalmente — é só passar o caminho
do arquivo.

## Como funciona, em resumo

O laço principal do frontend roda 60 vezes por segundo e, a cada volta:

1. converte os eventos do teclado em `chip8_key_down()` / `chip8_key_up()`;
2. chama `chip8_run_frame(&vm, ipf)`, que executa `ipf` instruções e decrementa
   os dois temporizadores uma vez;
3. desenha os 64×32 pixels se `vm.draw_flag` estiver ligada;
4. liga ou desliga o som conforme `chip8_beeping(&vm)`.

Toda instrução tem 16 bits e é decodificada pelos mesmos campos fixos:

```
     15..12   11..8    7..4     3..0
    +-------+-------+-------+-------+
    |   H   |   X   |   Y   |   N   |
    +-------+-------+-------+-------+
            \_______________________/   NNN = endereço de 12 bits
                    \_______________/   NN  = constante de 8 bits
```

O interpretador é um `switch` sobre `H` (a família da instrução), com `switch`
aninhado onde a família precisa de mais bits para se desambiguar.

Detalhes completos:

- **[docs/ARQUITETURA.md](docs/ARQUITETURA.md)** — mapa de memória, ciclo de
  execução, modelo de tempo e as decisões de projeto por trás do código.
- **[docs/OPCODES.md](docs/OPCODES.md)** — as 35 instruções, uma a uma, com as
  sutilezas de cada uma.
- **[docs/QUIRKS.md](docs/QUIRKS.md)** — as seis divergências históricas entre
  interpretadores e como configurá-las.

## Estrutura do projeto

```
chip8/
├── include/chip8.h        interface pública, documentada função por função
├── src/
│   ├── chip8.c            o núcleo: busca, decodificação e execução
│   └── disasm.c           desmontador
├── frontend/
│   ├── main_term.c        frontend de terminal (sem dependências)
│   └── main_sdl.c         frontend gráfico (SDL2)
├── tools/
│   ├── mkroms.c           gera as ROMs de demonstração
│   └── disasm_main.c      ferramenta de linha de comando
├── tests/test_chip8.c     suíte de testes
├── docs/                  documentação detalhada
├── roms/                  ROMs geradas por `make roms`
└── Makefile
```

## Desenvolvimento

O projeto compila com um conjunto rígido de avisos (`-Wall -Wextra -Wpedantic
-Wconversion -Wshadow` e outros) e **sem nenhum aviso**. Antes de enviar uma
mudança:

```bash
make clean && make && make test
make sanitize     # roda os testes com AddressSanitizer e UBSan
```

O núcleo é reentrante: todo o estado vive dentro de `chip8_t`, sem nenhuma
variável global, então dá para emular várias máquinas ao mesmo tempo no mesmo
processo.

### Portando para outra plataforma

Para escrever um novo frontend (SDL3, ncurses, web via Emscripten, um
microcontrolador), basta implementar quatro coisas em cima do núcleo:

1. um relógio de 60 Hz que chame `chip8_run_frame()`;
2. desenho do vetor `vm.display` (64×32 bytes, 0 ou 1);
3. tradução do teclado para `chip8_key_down()` / `chip8_key_up()`;
4. um bipe ligado enquanto `chip8_beeping()` for verdadeiro.

O núcleo não usa alocação dinâmica, ponto flutuante nem chamadas de sistema —
só `<stdio.h>` para a função de carregar ROM de arquivo, que pode ser
dispensada usando `chip8_load_rom()` com os bytes já em memória.

## Limitações conhecidas

- Implementa o **CHIP-8 original**, não as extensões SUPER-CHIP (tela de
  128×64, `SCRL`, sprites de 16×16) nem XO-CHIP (cores, áudio ampliado). Os
  quirks do SUPER-CHIP relevantes para ROMs comuns estão disponíveis via
  `--quirks schip`.
- `0NNN` (`SYS addr`) é ignorada de propósito: emular seria emular o
  processador RCA 1802 inteiro, e nenhuma ROM moderna usa.
- `FX1E` não altera `VF` em estouro. Só o interpretador do Amiga fazia isso, e
  quase nenhuma ROM depende disso.
- O frontend de terminal não detecta teclas soltas (veja a limitação acima).

## Licença

Mesma licença do repositório que o hospeda — veja o arquivo `LICENSE` na raiz.
