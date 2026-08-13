# Arquitetura do emulador

Este documento explica **como o emulador foi construído** e por quê. Para a
referência das instruções, veja [OPCODES.md](OPCODES.md); para as divergências
entre interpretadores, [QUIRKS.md](QUIRKS.md).

## A máquina emulada

O CHIP-8 é uma máquina virtual de 1977, projetada por Joseph Weisbecker para o
COSMAC VIP. Ela nunca existiu em silício: era um interpretador de ~512 bytes
rodando no RCA 1802, e é por isso que o programa do usuário começa em `0x200` —
os primeiros 512 bytes da memória eram o próprio interpretador.

| Recurso | Especificação |
|---|---|
| Memória | 4096 bytes, endereços `0x000`–`0xFFF` |
| Registradores | `V0`..`VF`, 8 bits cada (`VF` é o registrador de flag) |
| Índice `I` | 16 bits (12 úteis: a memória só vai até `0xFFF`) |
| Pilha | 16 níveis, guarda apenas endereços de retorno |
| Temporizadores | `delay` e `sound`, 8 bits, contando para trás a 60 Hz |
| Vídeo | 64×32 pixels monocromáticos, desenho por XOR |
| Entrada | 16 teclas hexadecimais |
| Instruções | 35, todas de 16 bits |

## Mapa de memória

```
0x000 ┌────────────────────────────────────┐
      │  área do interpretador original     │
0x050 ├────────────────────────────────────┤
      │  fonte hexadecimal (16 x 5 bytes)   │  ← instalada por chip8_init()
0x0A0 ├────────────────────────────────────┤
      │  livre                              │
0x200 ├────────────────────────────────────┤  ← pc inicial; ROM carregada aqui
      │                                     │
      │  programa + dados + sprites         │
      │  (o programa gerencia esse espaço)  │
      │                                     │
0xFFF └────────────────────────────────────┘
```

Não há proteção de memória, nem separação entre código e dados: uma ROM pode
escrever por cima de si mesma, e sprites ficam misturados às instruções. O
núcleo valida apenas os limites dos 4 KiB — acesso fora disso vira
`CHIP8_ERR_BAD_ADDRESS` em vez de corromper a memória do processo.

## Separação núcleo / frontend

A decisão estrutural mais importante do projeto: **o núcleo não conhece o mundo
exterior**.

```
        ┌──────────────────────────────────────────┐
        │  frontend (terminal, SDL2, web, ...)     │
        │  relógio · desenho · teclado · som       │
        └───────────────┬──────────────────────────┘
                        │  chip8_run_frame()
                        │  chip8_key_down/up()
                        │  vm.display, chip8_beeping()
        ┌───────────────▼──────────────────────────┐
        │  núcleo (src/chip8.c) — C99 puro          │
        │  sem alocação, sem globais, sem I/O       │
        └──────────────────────────────────────────┘
```

O núcleo não desenha, não lê teclado e não toca som: ele emula a máquina e
expõe o resultado (o vetor `display`, o `sound_timer`, o estado do teclado). O
frontend faz a ponte com o hardware real. Isso rende três coisas concretas:

- **testabilidade**: a suíte roda a máquina inteira sem tela nem teclado;
- **portabilidade**: um novo frontend precisa implementar só quatro coisas
  (relógio de 60 Hz, desenho, teclado, bipe);
- **reentrância**: todo o estado vive dentro de `chip8_t`, sem nenhuma variável
  global, então dá para emular várias máquinas no mesmo processo.

## O ciclo de execução

### Um passo: `chip8_step()`

```
  ┌─ busca ─────────────────────────────────────────────┐
  │  opcode = (mem[pc] << 8) | mem[pc+1]                 │  big-endian
  │  pc += 2                                             │  antes de executar
  └──────────────────────────────────────────────────────┘
  ┌─ decodificação ─────────────────────────────────────┐
  │  extrai X, Y, N, NN, NNN das posições fixas          │
  └──────────────────────────────────────────────────────┘
  ┌─ execução ──────────────────────────────────────────┐
  │  switch (opcode & 0xF000)  →  switch aninhado onde   │
  │  a família precisa de mais bits                      │
  └──────────────────────────────────────────────────────┘
```

Incrementar `pc` **antes** de executar é o que faz saltos e skips ficarem
simples: `JP` só sobrescreve `pc`, e um skip é `pc += 2` a mais. Em caso de
erro, `pc` é restaurado para a instrução problemática, para que a mensagem de
erro aponte o lugar certo.

O `switch` sobre o primeiro nibble é o suficiente para 10 das 16 famílias; as
outras seis (`0`, `5`, `8`, `9`, `E`, `F`) precisam de um segundo nível. Uma
tabela de despacho por ponteiro de função seria mais "elegante", mas o `switch`
é mais rápido (o compilador gera uma jump table), mais fácil de ler e mantém
cada instrução ao lado do comentário que a explica.

### Um quadro: `chip8_run_frame()`

O CHIP-8 tem **dois relógios independentes**: os temporizadores correm a 60 Hz
fixos, e a CPU corre a uma velocidade que a especificação nunca definiu. Cada
ROM foi escrita presumindo a velocidade da máquina de quem a escreveu.

O modelo adotado aqui — o mesmo da maioria dos emuladores — é ancorar tudo no
quadro de 60 Hz:

```c
for (i = 0; i < ipf; i++) {
    err = chip8_step(vm);
    if (err) break;
    if (vm->waiting_key || vm->vblank_wait) break;  /* nada a ganhar em seguir */
}
chip8_tick_timers(vm);   /* exatamente um tique por quadro */
```

`ipf` (instruções por quadro) é o único botão de velocidade: 11 dá os ~660 Hz
do COSMAC VIP, e valores entre 10 e 30 cobrem a maioria das ROMs. Alternativas
como contar ciclos por instrução não fariam sentido aqui, porque o custo real
dependia do RCA 1802, não do CHIP-8.

O laço termina cedo em dois casos:

- **`waiting_key`**: a CPU está parada em `FX0A` e nenhum ciclo adicional muda
  algo (mas os temporizadores continuam correndo, como no hardware);
- **`vblank_wait`**: com o quirk `display_wait`, `DXYN` sincroniza com o
  retraço vertical — no máximo um sprite por quadro.

## Decisões de projeto

### Framebuffer de um byte por pixel

`display` é um vetor de 2048 bytes (`0` ou `1`), não um bitmap compactado de
256 bytes. Gasta 8× mais memória — irrelevante em qualquer máquina atual — e em
troca elimina as máscaras de bit tanto no desenho quanto na leitura pelo
frontend. Num emulador cujo maior gerador de bugs é a instrução de desenho,
essa simplicidade vale mais que os 1,8 KiB.

### Gerador aleatório próprio

`CXNN` usa um xorshift32 embutido em vez de `rand()`. Duas razões: `rand()`
depende de estado global da libc (o que quebraria a reentrância) e varia entre
plataformas. Com o gerador próprio, `chip8_seed()` torna qualquer partida
reprodutível — inclusive nos testes, que verificam que a mesma semente produz a
mesma sequência.

### Erros reportados, não ignorados

Interpretadores reais não tinham como reportar erro: um `RET` a mais
simplesmente corrompia a memória. Aqui, cada situação anômala vira um
`chip8_err_t` específico (pilha, endereço, opcode) que sobe até o frontend, com
`pc` e opcode preservados. É a diferença entre "o jogo travou" e "estouro de
pilha em `pc=0x24A`".

### Struct pública

`chip8_t` é exposta na interface em vez de escondida atrás de um ponteiro
opaco. Frontends precisam ler `display`, `draw_flag` e `sound_timer` a cada
quadro, e depuradores precisam inspecionar registradores; encapsular tudo isso
significaria uma dúzia de getters sem ganho real. O contrato documentado é:
leia à vontade, escreva pelas funções (a exceção é `draw_flag`, que o frontend
limpa após desenhar).

### Quirks configuráveis em vez de escolhidos

As seis divergências históricas entre interpretadores viram campos de
`chip8_quirks_t`, com dois conjuntos prontos (`vip` e `schip`). Fixar um
comportamento faria uma parte das ROMs funcionar e outra quebrar, sem meio
termo. Veja [QUIRKS.md](QUIRKS.md).

## Modelo de tempo do frontend

Os dois frontends seguem o mesmo laço, com fontes de relógio diferentes:

| | Terminal | SDL2 |
|---|---|---|
| Relógio | `clock_gettime(CLOCK_MONOTONIC)` + `nanosleep` | `SDL_GetPerformanceCounter()` + `SDL_Delay` |
| Desenho | meio-blocos `▀` (64×16 caracteres) | textura ARGB escalada |
| Teclas soltas | temporizador de 220 ms (aproximação) | eventos reais `SDL_KEYUP` |
| Som | caractere BEL (`\a`) | onda quadrada de 440 Hz gerada em tempo real |

Os dois acumulam o instante do próximo quadro em vez de dormir 16 ms a cada
volta — assim o tempo gasto emulando e desenhando não vai somando atraso. Se a
máquina ficar lenta a ponto de perder o passo, a contagem é reancorada no
presente, para não tentar "recuperar" quadros que já passaram.

O frontend SDL **não** usa o vsync como relógio, apenas como forma de evitar
rasgos na imagem. Amarrar a emulação ao vsync deixaria a velocidade do jogo à
mercê do monitor: num painel de 144 Hz tudo rodaria 2,4 vezes mais rápido, e
num renderizador por software (sem vsync algum) a milhares de quadros por
segundo. Quem dita o compasso é o acumulador de 60 Hz, medido com o contador
de alta resolução do SDL.

A limitação da tecla solta é do console, não do emulador: um TTY entrega
apenas "tecla digitada". A repetição automática do teclado mantém a tecla
pressionada enquanto o dedo estiver nela, e o temporizador de 220 ms a solta
quando a repetição cessa.

## Como o projeto é testado

A suíte ([`tests/test_chip8.c`](../tests/test_chip8.c)) tem 176 verificações,
sem framework externo: uma macro `CHECK`, um contador de falhas, e verificações
que **não** abortam na primeira falha — assim uma única execução mostra todos os
problemas.

O padrão é sempre o mesmo: máquina limpa, uma instrução escrita em `0x200`, um
`chip8_step()`, conferência do estado.

Cobertura:

- as 35 instruções, incluindo casos de borda como `8FY4` (destino = `VF`);
- os seis quirks, cada um verificado nos dois estados;
- desenho de sprites: colisão, XOR apagando, módulo da posição inicial, corte e
  reaparecimento nas bordas;
- erros: estouro e esvaziamento de pilha, endereço inválido, opcode inexistente;
- `FX0A` em todas as fases (pressionar não libera, soltar libera, soltar outra
  tecla não libera);
- determinismo do gerador aleatório;
- o desmontador, inclusive com buffer pequeno demais;
- um teste de integração que roda a ROM `fonte.ch8` inteira e confere os pixels
  resultantes.

```bash
make test        # execução normal
make sanitize    # com AddressSanitizer e UBSan, em diretório separado
```

O projeto compila com `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
-Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wcast-align` e sem
nenhum aviso.
