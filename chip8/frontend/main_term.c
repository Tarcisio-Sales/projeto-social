/**
 * @file main_term.c
 * @brief Frontend de terminal do emulador — roda em qualquer console POSIX.
 *
 * Este frontend não depende de nenhuma biblioteca gráfica: desenha com
 * caracteres de bloco Unicode, lê o teclado em modo cru (`termios`) e
 * "toca" o bipe com o caractere BEL (`\a`). É a versão garantida de
 * funcionar em servidores, contêineres e sessões SSH.
 *
 * ## Como a tela de 64x32 cabe no terminal
 *
 * Cada caractere de texto vira **duas** linhas de pixels usando o bloco
 * superior `▀` (U+2580): a cor de frente pinta o pixel de cima e a cor de
 * fundo pinta o de baixo. Com isso a tela ocupa 64 colunas por 16 linhas,
 * o que cabe num terminal padrão de 80x24 e mantém os pixels aproximadamente
 * quadrados.
 *
 * ## Teclado
 *
 * O teclado hexadecimal do CHIP-8 é mapeado no bloco à esquerda do teclado
 * QWERTY, convenção adotada por praticamente todos os emuladores:
 *
 * ```
 *   CHIP-8            Teclado
 *  1 2 3 C           1 2 3 4
 *  4 5 6 D    <==    Q W E R
 *  7 8 9 E           A S D F
 *  A 0 B F           Z X C V
 * ```
 *
 * **Limitação inerente ao terminal:** um console entrega apenas o evento de
 * *tecla digitada*; não existe evento de "tecla solta". O frontend contorna
 * isso soltando a tecla automaticamente após CHIP8_TERM_KEY_HOLD_MS
 * milissegundos sem repetição. Segurar a tecla funciona graças à repetição
 * automática do teclado, mas jogos que exigem pressionar duas teclas ao mesmo
 * tempo ficam prejudicados — para esses, use o frontend SDL2.
 *
 * ## Uso
 *
 * ```
 * chip8-term [opções] rom.ch8
 *   --ipf N        instruções por quadro (padrão 11, ~660 Hz)
 *   --quirks MODO  vip (padrão) ou schip
 *   --seed N       semente do gerador aleatório (0 = relógio)
 *   --debug        imprime o traço das instruções em stderr
 *   --help         mostra esta ajuda
 * ```
 *
 * Sair: `ESC` ou `Ctrl+C`.
 */

#define _POSIX_C_SOURCE 200809L

#include "chip8.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/** Duração, em ms, do quadro de 60 Hz. */
#define CHIP8_FRAME_MS 16
/** Tempo, em ms, que uma tecla continua "pressionada" após ser digitada. */
#define CHIP8_TERM_KEY_HOLD_MS 220
/** Instruções por quadro adotadas quando o usuário não escolhe. */
#define CHIP8_DEFAULT_IPF 11

/* ------------------------------------------------------------------------ */
/* Estado global mínimo (necessário para o tratador de sinal)                */
/* ------------------------------------------------------------------------ */

/** Vira 1 quando chega SIGINT/SIGTERM, encerrando o laço principal. */
static volatile sig_atomic_t g_quit = 0;

/** Configuração original do terminal, restaurada na saída. */
static struct termios g_saved_termios;
/** Indica que g_saved_termios contém uma configuração válida. */
static int g_termios_saved = 0;

/** @brief Tratador de SIGINT/SIGTERM: apenas sinaliza a saída. */
static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ------------------------------------------------------------------------ */
/* Terminal: modo cru e restauração                                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief Devolve o terminal ao estado normal e mostra o cursor.
 *
 * Registrada com `atexit()`, roda também quando o programa sai por erro.
 */
static void term_restore(void)
{
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_termios_saved = 0;
    }
    /* Mostra o cursor, restaura as cores e sai da tela alternativa. */
    fputs("\033[?25h\033[0m\033[?1049l", stdout);
    fflush(stdout);
}

/**
 * @brief Coloca o terminal em modo cru, sem eco e com leitura não bloqueante.
 *
 * @return 0 em caso de sucesso, -1 se a entrada não for um terminal.
 */
static int term_setup(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO)) {
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) {
        return -1;
    }
    g_termios_saved = 1;
    atexit(term_restore);

    raw = g_saved_termios;
    /* Sem modo canônico (não espera Enter) e sem eco das teclas na tela. */
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    /* `read()` volta imediatamente mesmo sem nada disponível. */
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return -1;
    }

    /* Tela alternativa (preserva o conteúdo anterior do terminal) + cursor
       escondido + tela limpa. */
    fputs("\033[?1049h\033[?25l\033[2J", stdout);
    fflush(stdout);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Relógio                                                                   */
/* ------------------------------------------------------------------------ */

/** @brief Relógio monotônico em milissegundos. */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ------------------------------------------------------------------------ */
/* Teclado                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * @brief Converte uma tecla do teclado físico na tecla hexadecimal do CHIP-8.
 *
 * @param c Caractere lido da entrada padrão.
 * @return Valor de 0x0 a 0xF, ou -1 se a tecla não faz parte do mapa.
 */
static int map_key(int c)
{
    switch (c) {
    case '1': return 0x1;
    case '2': return 0x2;
    case '3': return 0x3;
    case '4': return 0xC;
    case 'q': case 'Q': return 0x4;
    case 'w': case 'W': return 0x5;
    case 'e': case 'E': return 0x6;
    case 'r': case 'R': return 0xD;
    case 'a': case 'A': return 0x7;
    case 's': case 'S': return 0x8;
    case 'd': case 'D': return 0x9;
    case 'f': case 'F': return 0xE;
    case 'z': case 'Z': return 0xA;
    case 'x': case 'X': return 0x0;
    case 'c': case 'C': return 0xB;
    case 'v': case 'V': return 0xF;
    default: return -1;
    }
}

/* ------------------------------------------------------------------------ */
/* Desenho                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * @brief Redesenha a tela inteira usando meio-blocos Unicode.
 *
 * Monta o quadro completo em um buffer e o envia numa única escrita, para
 * evitar o piscar que aparece quando se escreve caractere a caractere.
 *
 * @param vm     Máquina cuja tela será desenhada.
 * @param status Linha de status mostrada abaixo da tela.
 */
static void draw_screen(const chip8_t *vm, const char *status)
{
    /* Pior caso: cada pixel duplo custa um seletor de cor (~20 bytes) mais o
       caractere de bloco (3 bytes). Dimensionado com folga. */
    static char frame[CHIP8_SCREEN_W * (CHIP8_SCREEN_H / 2) * 32 + 512];
    size_t len = 0;
    int y;

    /* Volta o cursor para o canto superior esquerdo sem limpar a tela: a
       sobreposição do quadro novo é o que evita o tremor da imagem. */
    len += (size_t)snprintf(frame + len, sizeof(frame) - len, "\033[H");

    for (y = 0; y < CHIP8_SCREEN_H; y += 2) {
        int x;
        for (x = 0; x < CHIP8_SCREEN_W; x++) {
            int top = chip8_pixel(vm, x, y);
            int bottom = chip8_pixel(vm, x, y + 1);

            /* `▀` acende o pixel de cima com a cor de frente e o de baixo com
               a cor de fundo — dois pixels do CHIP-8 em um caractere. */
            len += (size_t)snprintf(
                frame + len, sizeof(frame) - len, "\033[3%dm\033[4%dm\xe2\x96\x80",
                top ? 7 : 0, bottom ? 7 : 0);
        }
        len += (size_t)snprintf(frame + len, sizeof(frame) - len, "\033[0m\r\n");
    }

    len += (size_t)snprintf(frame + len, sizeof(frame) - len, "\033[0m\r\n%s",
                            status);

    fwrite(frame, 1, len, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------------ */
/* Linha de comando                                                          */
/* ------------------------------------------------------------------------ */

/** @brief Imprime o texto de ajuda. */
static void print_usage(const char *prog)
{
    printf(
        "Emulador CHIP-8 (frontend de terminal)\n"
        "\n"
        "Uso: %s [opcoes] rom.ch8\n"
        "\n"
        "Opcoes:\n"
        "  --ipf N        instrucoes por quadro (padrao %d, ~660 Hz)\n"
        "  --quirks MODO  'vip' (padrao) ou 'schip'\n"
        "  --seed N       semente do gerador aleatorio (0 = relogio)\n"
        "  --debug        imprime o traco das instrucoes em stderr\n"
        "  --help         mostra esta ajuda\n"
        "\n"
        "Teclado:\n"
        "  1 2 3 4        ->  1 2 3 C\n"
        "  Q W E R        ->  4 5 6 D\n"
        "  A S D F        ->  7 8 9 E\n"
        "  Z X C V        ->  A 0 B F\n"
        "\n"
        "  ESC            sai\n"
        "  TAB            reinicia a ROM\n"
        "  ESPACO         pausa / continua\n",
        prog, CHIP8_DEFAULT_IPF);
}

/* ------------------------------------------------------------------------ */
/* Programa principal                                                        */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    chip8_t vm;
    chip8_quirks_t quirks = chip8_quirks_vip();
    const char *rom_path = NULL;
    unsigned ipf = CHIP8_DEFAULT_IPF;
    unsigned long seed = 0;
    int debug = 0;
    int paused = 0;
    int i;
    chip8_err_t err;

    /* Instante em que cada tecla foi digitada pela última vez; serve para
       soltá-la automaticamente (o terminal não avisa quando ela sobe). */
    long long key_time[CHIP8_NUM_KEYS];
    long long next_frame;

    /* --- Argumentos ------------------------------------------------------ */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--ipf") == 0 && i + 1 < argc) {
            ipf = (unsigned)strtoul(argv[++i], NULL, 10);
            if (ipf == 0) {
                ipf = 1;
            }
        } else if (strcmp(argv[i], "--quirks") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "schip") == 0) {
                quirks = chip8_quirks_schip();
            } else if (strcmp(mode, "vip") == 0) {
                quirks = chip8_quirks_vip();
            } else {
                fprintf(stderr, "modo de quirks desconhecido: %s\n", mode);
                return 1;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "opcao desconhecida: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            rom_path = argv[i];
        }
    }

    if (rom_path == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    /* --- Máquina --------------------------------------------------------- */
    chip8_init(&vm, &quirks);
    chip8_seed(&vm, (uint32_t)(seed != 0 ? seed : (unsigned long)time(NULL)));

    err = chip8_load_rom_file(&vm, rom_path);
    if (err != CHIP8_OK) {
        fprintf(stderr, "erro ao carregar '%s': %s\n", rom_path,
                chip8_strerror(err));
        return 1;
    }

    /* --- Terminal -------------------------------------------------------- */
    if (term_setup() != 0) {
        fprintf(stderr,
                "a entrada padrao nao e um terminal interativo; "
                "este frontend precisa de um TTY.\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(key_time, 0, sizeof(key_time));
    next_frame = now_ms();

    /* --- Laço principal: um quadro a cada 16 ms (60 Hz) ------------------ */
    while (!g_quit) {
        long long now;
        char status[160];
        char mnemonic[32];
        int beeping;

        /* 1. Entrada: consome tudo o que o terminal tiver acumulado. */
        for (;;) {
            unsigned char c;
            ssize_t got = read(STDIN_FILENO, &c, 1);
            int key;

            if (got <= 0) {
                break;
            }

            if (c == 27) { /* ESC */
                g_quit = 1;
                break;
            }
            if (c == '\t') { /* TAB: reinicia mantendo a ROM na memória */
                chip8_reset(&vm);
                memset(key_time, 0, sizeof(key_time));
                continue;
            }
            if (c == ' ') {
                paused = !paused;
                continue;
            }

            key = map_key(c);
            if (key >= 0) {
                chip8_key_down(&vm, (uint8_t)key);
                key_time[key] = now_ms();
            }
        }

        now = now_ms();

        /* 2. Solta as teclas que não recebem repetição há tempo suficiente. */
        for (i = 0; i < CHIP8_NUM_KEYS; i++) {
            if (vm.keys[i] && now - key_time[i] > CHIP8_TERM_KEY_HOLD_MS) {
                chip8_key_up(&vm, (uint8_t)i);
            }
        }

        /* 3. Emulação de um quadro. */
        if (!paused) {
            if (debug) {
                /* Antes de executar, mostra o que está em `pc`. */
                uint16_t op = (uint16_t)((vm.mem[vm.pc] << 8) | vm.mem[vm.pc + 1]);
                chip8_disasm(op, mnemonic, sizeof(mnemonic));
                fprintf(stderr, "%04X: %04X  %-18s I=%03X SP=%u\n", vm.pc, op,
                        mnemonic, vm.I, (unsigned)vm.sp);
            }

            err = chip8_run_frame(&vm, ipf);
            if (err != CHIP8_OK) {
                term_restore();
                fprintf(stderr,
                        "\nerro de execucao em pc=%04X (opcode %04X): %s\n",
                        vm.pc, vm.last_opcode, chip8_strerror(err));
                return 1;
            }
        }

        /* 4. Bipe: BEL é o único som disponível num terminal puro. */
        beeping = chip8_beeping(&vm);
        if (beeping) {
            fputc('\a', stdout);
        }

        /* 5. Desenho (só quando algo mudou, ou a cada quadro se pausado). */
        snprintf(status, sizeof(status),
                 "%s | ciclos %llu | pc %04X | ipf %u | %s   ESC sai  TAB "
                 "reinicia  ESPACO pausa",
                 paused ? "PAUSADO" : (vm.waiting_key ? "AGUARDA TECLA" : "RODANDO"),
                 (unsigned long long)vm.cycles, vm.pc, ipf,
                 beeping ? "BIPE" : "    ");

        if (vm.draw_flag || paused) {
            draw_screen(&vm, status);
            vm.draw_flag = 0;
        } else {
            /* Sem mudança na tela, atualiza apenas a linha de status. */
            printf("\033[%dH\033[2K%s", CHIP8_SCREEN_H / 2 + 2, status);
            fflush(stdout);
        }

        /* 6. Sincronismo: dorme o que sobrou do quadro. */
        next_frame += CHIP8_FRAME_MS;
        now = now_ms();
        if (next_frame > now) {
            struct timespec sleep_time;
            long long delta = next_frame - now;
            sleep_time.tv_sec = (time_t)(delta / 1000);
            sleep_time.tv_nsec = (long)((delta % 1000) * 1000000L);
            nanosleep(&sleep_time, NULL);
        } else {
            /* Ficamos para trás (terminal lento): recomeça a contagem para
               não acumular um atraso que nunca será recuperado. */
            next_frame = now;
        }
    }

    term_restore();
    printf("emulacao encerrada apos %llu ciclos\n",
           (unsigned long long)vm.cycles);
    return 0;
}
