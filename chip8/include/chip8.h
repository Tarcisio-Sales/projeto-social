/**
 * @file chip8.h
 * @brief Interface pública do núcleo do emulador CHIP-8.
 *
 * O núcleo é escrito em C99 puro, sem nenhuma dependência externa além da
 * biblioteca padrão. Ele não desenha na tela, não lê o teclado e não emite
 * som: apenas emula a máquina virtual CHIP-8 e expõe o estado resultante
 * (framebuffer, temporizador de som, pedido de tecla). Quem faz a ponte com
 * o mundo real é o *frontend* (veja `frontend/main_term.c` e
 * `frontend/main_sdl.c`).
 *
 * ## Fluxo de uso típico
 *
 * ```c
 * chip8_t vm;
 * chip8_init(&vm, NULL);                    // NULL = quirks padrão (COSMAC VIP)
 * chip8_load_rom_file(&vm, "jogo.ch8");
 *
 * for (;;) {                                // 60 vezes por segundo
 *     chip8_key_down(&vm, 0x5);             // eventos do frontend
 *     chip8_run_frame(&vm, 11);             // ~660 instruções/s
 *     desenha(vm.display);                  // se vm.draw_flag != 0
 *     if (chip8_beeping(&vm)) toca_som();
 * }
 * ```
 *
 * ## Modelo de máquina emulado
 *
 * | Recurso        | Tamanho                                          |
 * |----------------|--------------------------------------------------|
 * | Memória        | 4096 bytes (0x000–0xFFF)                         |
 * | Programa       | carregado em 0x200 (512)                         |
 * | Registradores  | V0..VF, 8 bits (VF é o registrador de flag)      |
 * | Índice `I`     | 16 bits (na prática 12 bits úteis)               |
 * | Pilha          | 16 níveis de endereço de retorno                 |
 * | Temporizadores | `delay_timer` e `sound_timer`, 8 bits, 60 Hz     |
 * | Vídeo          | 64x32 pixels monocromáticos, desenho por XOR     |
 * | Teclado        | 16 teclas hexadecimais (0x0..0xF)                |
 *
 * @author Emulador CHIP-8 do projeto-social
 * @note Todas as funções são reentrantes: o estado inteiro vive dentro de
 *       `chip8_t`, e nada é global. É seguro emular várias máquinas ao mesmo
 *       tempo, desde que cada uma tenha sua própria struct.
 */

#ifndef CHIP8_H
#define CHIP8_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Constantes da máquina                                                     */
/* ------------------------------------------------------------------------ */

/** Tamanho total da memória endereçável, em bytes. */
#define CHIP8_MEM_SIZE 4096
/** Endereço onde as ROMs são carregadas e onde a execução começa. */
#define CHIP8_PROG_START 0x200
/** Quantidade de registradores de uso geral (V0 a VF). */
#define CHIP8_NUM_REGS 16
/** Profundidade máxima da pilha de chamadas (instruções `CALL`/`RET`). */
#define CHIP8_STACK_SIZE 16
/** Largura da tela, em pixels. */
#define CHIP8_SCREEN_W 64
/** Altura da tela, em pixels. */
#define CHIP8_SCREEN_H 32
/** Total de pixels do framebuffer. */
#define CHIP8_SCREEN_PIXELS (CHIP8_SCREEN_W * CHIP8_SCREEN_H)
/** Quantidade de teclas do teclado hexadecimal. */
#define CHIP8_NUM_KEYS 16
/** Endereço onde a fonte hexadecimal embutida é instalada. */
#define CHIP8_FONT_ADDR 0x50
/** Tamanho da fonte embutida: 16 caracteres de 5 bytes cada. */
#define CHIP8_FONT_SIZE 80
/** Altura, em bytes/linhas, de cada caractere da fonte embutida. */
#define CHIP8_FONT_CHAR_H 5
/** Frequência, em Hz, dos temporizadores de atraso e de som. */
#define CHIP8_TIMER_HZ 60
/** Maior ROM que cabe na memória (de 0x200 até 0xFFF). */
#define CHIP8_MAX_ROM_SIZE (CHIP8_MEM_SIZE - CHIP8_PROG_START)

/* ------------------------------------------------------------------------ */
/* Códigos de erro                                                           */
/* ------------------------------------------------------------------------ */

/**
 * @brief Resultado de uma operação do núcleo.
 *
 * Toda função que pode falhar devolve um destes valores. Use
 * chip8_strerror() para obter uma mensagem legível.
 */
typedef enum chip8_err {
    CHIP8_OK = 0,                /**< Operação concluída com sucesso.        */
    CHIP8_ERR_UNKNOWN_OPCODE,    /**< Instrução não reconhecida.             */
    CHIP8_ERR_STACK_OVERFLOW,    /**< `CALL` além de CHIP8_STACK_SIZE níveis.*/
    CHIP8_ERR_STACK_UNDERFLOW,   /**< `RET` sem chamada correspondente.      */
    CHIP8_ERR_BAD_ADDRESS,       /**< Acesso fora dos 4 KiB de memória.      */
    CHIP8_ERR_ROM_TOO_BIG,       /**< ROM não cabe a partir de 0x200.        */
    CHIP8_ERR_IO,                /**< Falha ao abrir/ler o arquivo da ROM.   */
    CHIP8_ERR_INVALID_ARG        /**< Argumento nulo ou fora da faixa.       */
} chip8_err_t;

/* ------------------------------------------------------------------------ */
/* Quirks (variações de comportamento entre intérpretes)                     */
/* ------------------------------------------------------------------------ */

/**
 * @brief Ajustes de compatibilidade entre as várias implementações de CHIP-8.
 *
 * O CHIP-8 nunca teve uma especificação formal: o que existe é o
 * comportamento do interpretador original do COSMAC VIP (1977) e o de
 * dezenas de reimplementações que discordam em pontos específicos. Uma ROM
 * escrita para um interpretador pode quebrar em outro. Estes campos deixam
 * cada divergência configurável — são os mesmos pontos testados pela suíte
 * "CHIP-8 test suite" de Timendus.
 *
 * Cada campo é um booleano (0 = desligado, diferente de 0 = ligado).
 * Use chip8_quirks_vip() para o comportamento histórico do COSMAC VIP e
 * chip8_quirks_schip() para o do SUPER-CHIP / emuladores modernos.
 */
typedef struct chip8_quirks {
    /**
     * As instruções lógicas `8XY1` (OR), `8XY2` (AND) e `8XY3` (XOR) zeram
     * VF como efeito colateral.
     * - COSMAC VIP: ligado (o hardware original sujava VF).
     * - SUPER-CHIP e a maioria dos emuladores: desligado.
     */
    int vf_reset;

    /**
     * `FX55` e `FX65` (salvar/carregar registradores) incrementam `I` em
     * `X + 1` ao terminar, em vez de deixá-lo intacto.
     * - COSMAC VIP: ligado.
     * - SUPER-CHIP: desligado.
     */
    int memory_increment_i;

    /**
     * `DXYN` espera o próximo pulso de sincronismo vertical antes de
     * desenhar, limitando o desenho a um sprite por quadro (60 Hz). É o que
     * evita o "tearing" em ROMs antigas.
     * - COSMAC VIP: ligado.
     * - SUPER-CHIP: desligado.
     */
    int display_wait;

    /**
     * Sprites que ultrapassam a borda direita ou inferior são cortados
     * (clipping) em vez de reaparecerem do outro lado (wrapping).
     * A posição *inicial* do sprite sempre sofre módulo, independente deste
     * campo — isso é consenso entre todos os intérpretes.
     * - Comportamento correto na maioria das ROMs: ligado.
     */
    int clipping;

    /**
     * `8XY6` (SHR) e `8XYE` (SHL) deslocam VX no lugar, ignorando VY.
     * - COSMAC VIP: desligado (copia VY para VX e desloca).
     * - SUPER-CHIP: ligado.
     */
    int shift_vx_in_place;

    /**
     * `BNNN` é interpretado como `BXNN`: salta para `NN + VX` em vez de
     * `NNN + V0`.
     * - COSMAC VIP: desligado.
     * - SUPER-CHIP: ligado.
     */
    int jump_with_offset_vx;
} chip8_quirks_t;

/* ------------------------------------------------------------------------ */
/* Estado da máquina                                                         */
/* ------------------------------------------------------------------------ */

/**
 * @brief Estado completo de uma máquina virtual CHIP-8.
 *
 * A struct é exposta de propósito: frontends precisam ler `display`,
 * `sound_timer` e afins, e depuradores precisam inspecionar registradores.
 * Leia à vontade; escreva apenas através das funções desta interface, com
 * a exceção documentada de `draw_flag` (que o frontend limpa após desenhar).
 */
typedef struct chip8 {
    /* --- Memória e registradores --- */

    uint8_t mem[CHIP8_MEM_SIZE];      /**< RAM de 4 KiB, com a fonte em 0x50. */
    uint8_t V[CHIP8_NUM_REGS];        /**< Registradores V0..VF.              */
    uint16_t I;                       /**< Registrador de índice (endereços). */
    uint16_t pc;                      /**< Contador de programa.              */

    uint16_t stack[CHIP8_STACK_SIZE]; /**< Pilha de endereços de retorno.     */
    uint8_t sp;                       /**< Topo da pilha (0 = pilha vazia).   */

    uint8_t delay_timer;              /**< Decrementa a 60 Hz até zerar.      */
    uint8_t sound_timer;              /**< Idem; enquanto > 0, há bipe.       */

    /* --- Periféricos --- */

    /**
     * Framebuffer de 64x32. Um byte por pixel (0 = apagado, 1 = aceso),
     * indexado como `display[y * CHIP8_SCREEN_W + x]`. Gasta 2 KiB em vez
     * dos 256 bytes de um bitmap compactado, e em troca o desenho e a
     * leitura pelo frontend ficam triviais.
     */
    uint8_t display[CHIP8_SCREEN_PIXELS];

    /**
     * Sinaliza que `display` mudou desde o último quadro desenhado. O núcleo
     * liga esta flag; o frontend a desliga depois de redesenhar a tela.
     */
    uint8_t draw_flag;

    /**
     * Estado do teclado hexadecimal: `keys[k]` é diferente de zero enquanto
     * a tecla `k` estiver pressionada. Atualize com chip8_key_down() e
     * chip8_key_up().
     */
    uint8_t keys[CHIP8_NUM_KEYS];

    /* --- Estado interno da execução --- */

    /**
     * Diferente de zero quando a instrução `FX0A` está bloqueada esperando
     * uma tecla. Enquanto isso, chip8_step() não avança o programa.
     */
    uint8_t waiting_key;
    /** Registrador de destino do `FX0A` em curso. */
    uint8_t waiting_reg;
    /** Tecla já pressionada durante um `FX0A`, aguardando ser solta. */
    uint8_t waiting_pressed_key;
    /** Diferente de zero se `waiting_pressed_key` é válido. */
    uint8_t waiting_has_key;

    /**
     * Ligado por `DXYN` quando o quirk `display_wait` está ativo, para que
     * chip8_run_frame() encerre o quadro após o desenho.
     */
    uint8_t vblank_wait;

    /** Estado do gerador pseudoaleatório usado por `CXNN`. */
    uint32_t rng_state;

    /** Total de instruções executadas desde chip8_init(). */
    uint64_t cycles;

    /** Última instrução de 16 bits buscada (útil em mensagens de erro). */
    uint16_t last_opcode;

    /** Configuração de compatibilidade em vigor. */
    chip8_quirks_t quirks;
} chip8_t;

/* ------------------------------------------------------------------------ */
/* Conjuntos de quirks prontos                                               */
/* ------------------------------------------------------------------------ */

/**
 * @brief Devolve os quirks do interpretador original do COSMAC VIP (1977).
 *
 * É o padrão usado por chip8_init() quando `quirks` é `NULL`, e o que a
 * maioria das ROMs clássicas espera.
 */
chip8_quirks_t chip8_quirks_vip(void);

/**
 * @brief Devolve os quirks do SUPER-CHIP / emuladores modernos.
 *
 * Necessário para ROMs escritas nos anos 1990 em diante, que assumem
 * deslocamento no lugar e `FX55`/`FX65` sem incremento de `I`.
 */
chip8_quirks_t chip8_quirks_schip(void);

/* ------------------------------------------------------------------------ */
/* Ciclo de vida                                                             */
/* ------------------------------------------------------------------------ */

/**
 * @brief Zera a máquina e a deixa pronta para receber uma ROM.
 *
 * Limpa memória, registradores, pilha, tela e teclado; instala a fonte
 * hexadecimal em CHIP8_FONT_ADDR; aponta `pc` para CHIP8_PROG_START e
 * semeia o gerador aleatório com um valor fixo (use chip8_seed() para
 * variar).
 *
 * @param vm     Máquina a inicializar. Não pode ser `NULL`.
 * @param quirks Configuração desejada, ou `NULL` para chip8_quirks_vip().
 */
void chip8_init(chip8_t *vm, const chip8_quirks_t *quirks);

/**
 * @brief Reinicia a execução preservando o conteúdo já carregado na memória.
 *
 * Equivale ao botão *reset* do console: registradores, pilha, tela,
 * temporizadores e teclado voltam ao estado inicial, mas a ROM continua
 * carregada. Os quirks em vigor também são preservados.
 *
 * @param vm Máquina a reiniciar. Não pode ser `NULL`.
 */
void chip8_reset(chip8_t *vm);

/**
 * @brief Define a semente do gerador pseudoaleatório usado por `CXNN`.
 *
 * O gerador é um xorshift32 determinístico embutido no núcleo: a mesma
 * semente produz sempre a mesma sequência, o que torna os testes
 * reprodutíveis. Frontends costumam semear com o relógio.
 *
 * @param vm   Máquina alvo. Não pode ser `NULL`.
 * @param seed Semente. O valor 0 é substituído por uma constante, porque
 *             xorshift não sai do zero.
 */
void chip8_seed(chip8_t *vm, uint32_t seed);

/* ------------------------------------------------------------------------ */
/* Carga de ROM                                                              */
/* ------------------------------------------------------------------------ */

/**
 * @brief Copia uma ROM já em memória para dentro da máquina, em 0x200.
 *
 * @param vm   Máquina alvo, já inicializada. Não pode ser `NULL`.
 * @param data Bytes da ROM. Não pode ser `NULL` se `len` > 0.
 * @param len  Tamanho da ROM em bytes; no máximo CHIP8_MAX_ROM_SIZE.
 *
 * @retval CHIP8_OK              ROM carregada.
 * @retval CHIP8_ERR_INVALID_ARG Ponteiro nulo.
 * @retval CHIP8_ERR_ROM_TOO_BIG ROM maior que a memória disponível.
 */
chip8_err_t chip8_load_rom(chip8_t *vm, const uint8_t *data, size_t len);

/**
 * @brief Lê uma ROM do disco e a carrega em 0x200.
 *
 * @param vm   Máquina alvo, já inicializada. Não pode ser `NULL`.
 * @param path Caminho do arquivo `.ch8`. Não pode ser `NULL`.
 *
 * @retval CHIP8_OK              ROM carregada.
 * @retval CHIP8_ERR_IO          Arquivo inexistente ou ilegível.
 * @retval CHIP8_ERR_ROM_TOO_BIG Arquivo maior que CHIP8_MAX_ROM_SIZE.
 */
chip8_err_t chip8_load_rom_file(chip8_t *vm, const char *path);

/* ------------------------------------------------------------------------ */
/* Execução                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * @brief Executa exatamente uma instrução (busca, decodifica, executa).
 *
 * Não mexe nos temporizadores — eles andam a 60 Hz, num ritmo independente
 * da velocidade da CPU; use chip8_tick_timers() ou chip8_run_frame().
 *
 * Se a máquina estiver parada em `FX0A` esperando uma tecla, a chamada
 * consome o ciclo sem avançar `pc` e devolve CHIP8_OK.
 *
 * @param vm Máquina em execução. Não pode ser `NULL`.
 *
 * @retval CHIP8_OK                   Instrução executada.
 * @retval CHIP8_ERR_UNKNOWN_OPCODE   Opcode inválido; `pc` fica na instrução.
 * @retval CHIP8_ERR_STACK_OVERFLOW   Pilha cheia em um `CALL`.
 * @retval CHIP8_ERR_STACK_UNDERFLOW  Pilha vazia em um `RET`.
 * @retval CHIP8_ERR_BAD_ADDRESS      Acesso à memória fora dos 4 KiB.
 */
chip8_err_t chip8_step(chip8_t *vm);

/**
 * @brief Executa um quadro: até `ipf` instruções e um tique dos timers.
 *
 * Chamada 60 vezes por segundo pelo frontend, `ipf` (instruções por quadro)
 * define a velocidade percebida da CPU: 11 equivale aos ~660 Hz do COSMAC
 * VIP, e valores entre 10 e 30 servem para a maioria das ROMs.
 *
 * Com o quirk `display_wait` ligado, o laço para mais cedo se uma instrução
 * `DXYN` for executada — é assim que o hardware original sincronizava o
 * desenho com o retraço vertical.
 *
 * @param vm  Máquina em execução. Não pode ser `NULL`.
 * @param ipf Instruções por quadro. Zero apenas atualiza os temporizadores.
 *
 * @return CHIP8_OK, ou o primeiro erro encontrado (o laço para nele).
 */
chip8_err_t chip8_run_frame(chip8_t *vm, unsigned ipf);

/**
 * @brief Decrementa `delay_timer` e `sound_timer` em uma unidade cada.
 *
 * Deve ser chamada a 60 Hz. Timers já em zero permanecem em zero.
 * Também libera a espera de `DXYN` imposta pelo quirk `display_wait`.
 *
 * @param vm Máquina em execução. Não pode ser `NULL`.
 */
void chip8_tick_timers(chip8_t *vm);

/* ------------------------------------------------------------------------ */
/* Entrada e saída                                                           */
/* ------------------------------------------------------------------------ */

/**
 * @brief Marca uma tecla como pressionada.
 *
 * Se a máquina estiver bloqueada em `FX0A`, registra a tecla; o valor só é
 * entregue ao programa quando ela for solta (chip8_key_up()), que é o
 * comportamento do interpretador original.
 *
 * @param vm  Máquina alvo. Não pode ser `NULL`.
 * @param key Tecla de 0x0 a 0xF. Valores fora da faixa são ignorados.
 */
void chip8_key_down(chip8_t *vm, uint8_t key);

/**
 * @brief Marca uma tecla como solta.
 *
 * Se um `FX0A` estava esperando justamente esta tecla, o registrador de
 * destino recebe o valor e a execução é retomada.
 *
 * @param vm  Máquina alvo. Não pode ser `NULL`.
 * @param key Tecla de 0x0 a 0xF. Valores fora da faixa são ignorados.
 */
void chip8_key_up(chip8_t *vm, uint8_t key);

/**
 * @brief Informa se o alto-falante deve estar tocando neste instante.
 *
 * O CHIP-8 tem um único som: um tom fixo que soa enquanto `sound_timer`
 * for maior que zero.
 *
 * @param vm Máquina consultada. Não pode ser `NULL`.
 * @return Diferente de zero se há bipe.
 */
int chip8_beeping(const chip8_t *vm);

/**
 * @brief Lê o pixel na posição (x, y).
 *
 * @param vm Máquina consultada. Não pode ser `NULL`.
 * @param x  Coluna, de 0 a CHIP8_SCREEN_W - 1.
 * @param y  Linha, de 0 a CHIP8_SCREEN_H - 1.
 * @return 1 se o pixel está aceso, 0 se apagado ou fora da tela.
 */
int chip8_pixel(const chip8_t *vm, int x, int y);

/* ------------------------------------------------------------------------ */
/* Utilidades de depuração                                                   */
/* ------------------------------------------------------------------------ */

/**
 * @brief Traduz um código de erro para uma frase legível em português.
 *
 * @param err Código devolvido por qualquer função do núcleo.
 * @return Ponteiro para string estática — nunca `NULL`, nunca precisa ser
 *         liberada.
 */
const char *chip8_strerror(chip8_err_t err);

/**
 * @brief Desmonta uma instrução para texto em sintaxe CHIP-8 clássica.
 *
 * Exemplo: `0xD015` vira `"DRW V0, V1, 5"`. Opcodes desconhecidos viram
 * `"DW #XXXX"` (dados). Usado pelo modo de depuração dos frontends e pela
 * ferramenta `chip8-disasm`.
 *
 * @param opcode Instrução de 16 bits.
 * @param buf    Destino do texto. Não pode ser `NULL`.
 * @param n      Tamanho de `buf`; 32 bytes bastam para qualquer instrução.
 * @return Quantidade de caracteres escritos (sem contar o terminador).
 */
size_t chip8_disasm(uint16_t opcode, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* CHIP8_H */
