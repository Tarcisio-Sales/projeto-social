/**
 * @file main_sdl.c
 * @brief Frontend gráfico do emulador, baseado em SDL2.
 *
 * Comparado ao frontend de terminal, este oferece o que um console não
 * consegue entregar:
 *
 * - **eventos reais de tecla solta**, então jogos que dependem de segurar ou
 *   combinar teclas funcionam corretamente;
 * - **janela redimensionável** com escala por inteiro e pixels quadrados;
 * - **som de verdade**: uma onda quadrada gerada em tempo real enquanto o
 *   `sound_timer` do CHIP-8 for maior que zero.
 *
 * Só é compilado quando o `Makefile` encontra o SDL2 via `pkg-config`; sem
 * ele, o projeto continua compilando o frontend de terminal normalmente.
 *
 * ## Uso
 *
 * ```
 * chip8-sdl [opções] rom.ch8
 *   --ipf N        instruções por quadro (padrão 11, ~660 Hz)
 *   --quirks MODO  vip (padrão) ou schip
 *   --scale N      escala inicial da janela (padrão 12 => 768x384)
 *   --seed N       semente do gerador aleatório (0 = relógio)
 *   --mute         desliga o som
 *   --debug        imprime o traço das instruções em stderr
 *   --help         mostra esta ajuda
 * ```
 *
 * Teclas de controle: `ESC` sai, `TAB` reinicia, `ESPAÇO` pausa,
 * `-`/`+` diminuem e aumentam as instruções por quadro.
 */

#include "chip8.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Instruções por quadro adotadas quando o usuário não escolhe. */
#define CHIP8_DEFAULT_IPF 11
/** Escala inicial de cada pixel do CHIP-8, em pixels da tela. */
#define CHIP8_DEFAULT_SCALE 12
/** Frequência do tom do bipe, em Hz (um lá agudo, próximo do original). */
#define CHIP8_BEEP_FREQ 440
/** Taxa de amostragem do áudio. */
#define CHIP8_AUDIO_RATE 44100
/** Amplitude da onda quadrada (16 bits com sobra para não estourar). */
#define CHIP8_BEEP_AMPLITUDE 3000

/* ------------------------------------------------------------------------ */
/* Áudio                                                                     */
/* ------------------------------------------------------------------------ */

/**
 * @brief Estado do gerador de som, compartilhado com a thread de áudio.
 *
 * O SDL chama audio_callback() de outra thread. Só dois campos cruzam essa
 * fronteira: `beeping`, escrito pelo laço principal e lido pelo callback
 * (uma escrita atômica de int, sem risco prático de leitura parcial), e
 * `phase`, que pertence exclusivamente ao callback.
 */
typedef struct beeper {
    int beeping;   /**< Diferente de zero enquanto o bipe deve soar.        */
    double phase;  /**< Fase atual da onda, de 0 a 1 (só a thread de áudio).*/
} beeper_t;

/**
 * @brief Preenche o buffer de áudio com uma onda quadrada (ou silêncio).
 *
 * Onda quadrada é a forma mais próxima do bipe do hardware original: o
 * COSMAC VIP ligava e desligava um alto-falante piezoelétrico, o que produz
 * exatamente esse timbre.
 *
 * @param userdata Ponteiro para o beeper_t.
 * @param stream   Buffer a preencher, em amostras de 16 bits com sinal.
 * @param len      Tamanho do buffer, em bytes.
 */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    beeper_t *beeper = (beeper_t *)userdata;
    Sint16 *samples = (Sint16 *)stream;
    int count = len / (int)sizeof(Sint16);
    double step = (double)CHIP8_BEEP_FREQ / (double)CHIP8_AUDIO_RATE;
    int i;

    if (!beeper->beeping) {
        SDL_memset(stream, 0, (size_t)len);
        return;
    }

    for (i = 0; i < count; i++) {
        samples[i] = (beeper->phase < 0.5) ? (Sint16)CHIP8_BEEP_AMPLITUDE
                                           : (Sint16)-CHIP8_BEEP_AMPLITUDE;
        beeper->phase += step;
        if (beeper->phase >= 1.0) {
            beeper->phase -= 1.0;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Teclado                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * @brief Converte um código físico do SDL na tecla hexadecimal do CHIP-8.
 *
 * Usa *scancodes* (posição física) em vez de *keycodes* (símbolo impresso),
 * para que o layout continue coerente em teclados ABNT2, AZERTY etc.
 *
 * ```
 *   CHIP-8            Teclado
 *  1 2 3 C           1 2 3 4
 *  4 5 6 D    <==    Q W E R
 *  7 8 9 E           A S D F
 *  A 0 B F           Z X C V
 * ```
 *
 * @param sc Scancode recebido no evento.
 * @return Tecla de 0x0 a 0xF, ou -1 se não faz parte do mapa.
 */
static int map_scancode(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_1: return 0x1;
    case SDL_SCANCODE_2: return 0x2;
    case SDL_SCANCODE_3: return 0x3;
    case SDL_SCANCODE_4: return 0xC;
    case SDL_SCANCODE_Q: return 0x4;
    case SDL_SCANCODE_W: return 0x5;
    case SDL_SCANCODE_E: return 0x6;
    case SDL_SCANCODE_R: return 0xD;
    case SDL_SCANCODE_A: return 0x7;
    case SDL_SCANCODE_S: return 0x8;
    case SDL_SCANCODE_D: return 0x9;
    case SDL_SCANCODE_F: return 0xE;
    case SDL_SCANCODE_Z: return 0xA;
    case SDL_SCANCODE_X: return 0x0;
    case SDL_SCANCODE_C: return 0xB;
    case SDL_SCANCODE_V: return 0xF;
    default: return -1;
    }
}

/* ------------------------------------------------------------------------ */
/* Linha de comando                                                          */
/* ------------------------------------------------------------------------ */

/** @brief Imprime o texto de ajuda. */
static void print_usage(const char *prog)
{
    printf(
        "Emulador CHIP-8 (frontend SDL2)\n"
        "\n"
        "Uso: %s [opcoes] rom.ch8\n"
        "\n"
        "Opcoes:\n"
        "  --ipf N        instrucoes por quadro (padrao %d, ~660 Hz)\n"
        "  --quirks MODO  'vip' (padrao) ou 'schip'\n"
        "  --scale N      escala inicial da janela (padrao %d)\n"
        "  --seed N       semente do gerador aleatorio (0 = relogio)\n"
        "  --mute         desliga o som\n"
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
        "  ESPACO         pausa / continua\n"
        "  - / +          diminui / aumenta as instrucoes por quadro\n",
        prog, CHIP8_DEFAULT_IPF, CHIP8_DEFAULT_SCALE);
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
    unsigned scale = CHIP8_DEFAULT_SCALE;
    unsigned long seed = 0;
    int mute = 0;
    int debug = 0;
    int paused = 0;
    int running = 1;
    int i;
    chip8_err_t err;

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_AudioDeviceID audio_dev = 0;
    beeper_t beeper = {0, 0.0};

    /* Relógio de quadros: o acumulador é `double` para que os 1/60 de segundo
       não acumulem erro de arredondamento ao longo de horas de execução. */
    Uint64 perf_freq;
    double frame_ticks;
    double next_frame;

    /* Buffer ARGB de 64x32 enviado à textura a cada quadro. */
    Uint32 pixels[CHIP8_SCREEN_PIXELS];

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
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = (unsigned)strtoul(argv[++i], NULL, 10);
            if (scale == 0) {
                scale = 1;
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
        } else if (strcmp(argv[i], "--mute") == 0) {
            mute = 1;
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

    /* --- SDL: janela, renderizador e textura ----------------------------- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "falha ao iniciar o SDL: %s\n", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("Emulador CHIP-8", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              (int)(CHIP8_SCREEN_W * scale),
                              (int)(CHIP8_SCREEN_H * scale),
                              SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        fprintf(stderr, "falha ao criar a janela: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* Preferência: aceleração por GPU com vsync. Onde não há GPU (máquinas
       virtuais, sessões remotas, integração contínua), o SDL falha em vez de
       cair para software sozinho — daí a segunda tentativa sem exigências. */
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED |
                                                  SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (renderer == NULL) {
        fprintf(stderr, "falha ao criar o renderizador: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Mantém a proporção 2:1 e as bordas pretas ao redimensionar a janela. */
    SDL_RenderSetLogicalSize(renderer, CHIP8_SCREEN_W, CHIP8_SCREEN_H);
    /* Vizinho mais próximo: pixels nítidos, sem borrão de interpolação. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, CHIP8_SCREEN_W,
                                CHIP8_SCREEN_H);
    if (texture == NULL) {
        fprintf(stderr, "falha ao criar a textura: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* --- SDL: áudio ------------------------------------------------------ */
    if (!mute) {
        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = CHIP8_AUDIO_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 512; /* buffer curto => bipe responde rápido */
        want.callback = audio_callback;
        want.userdata = &beeper;

        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (audio_dev == 0) {
            fprintf(stderr, "aviso: som indisponivel (%s); seguindo mudo\n",
                    SDL_GetError());
        } else {
            SDL_PauseAudioDevice(audio_dev, 0); /* começa a tocar (silêncio) */
        }
    }

    /* --- Laço principal --------------------------------------------------
       O ritmo é imposto por um relógio próprio de 60 Hz, e não pelo vsync.
       Depender do vsync deixaria a velocidade da emulação à mercê do monitor:
       num painel de 144 Hz o jogo rodaria 2,4 vezes mais rápido, e sem vsync
       nenhum (renderizador por software) rodaria a milhares de quadros por
       segundo. O vsync continua ligado quando disponível, para evitar
       rasgos na imagem, mas quem dita o compasso é este acumulador. */
    perf_freq = SDL_GetPerformanceFrequency();
    frame_ticks = (double)perf_freq / (double)CHIP8_TIMER_HZ;
    next_frame = (double)SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        char title[128];

        /* 1. Eventos. */
        while (SDL_PollEvent(&event)) {
            int key;

            switch (event.type) {
            case SDL_QUIT:
                running = 0;
                break;

            case SDL_KEYDOWN:
                if (event.key.repeat) {
                    break; /* a repetição automática não interessa aqui */
                }
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = 0;
                    continue;
                case SDLK_TAB:
                    chip8_reset(&vm);
                    continue;
                case SDLK_SPACE:
                    paused = !paused;
                    continue;
                case SDLK_MINUS:
                    if (ipf > 1) {
                        ipf--;
                    }
                    continue;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                    ipf++;
                    continue;
                default:
                    break;
                }
                key = map_scancode(event.key.keysym.scancode);
                if (key >= 0) {
                    chip8_key_down(&vm, (uint8_t)key);
                }
                break;

            case SDL_KEYUP:
                key = map_scancode(event.key.keysym.scancode);
                if (key >= 0) {
                    chip8_key_up(&vm, (uint8_t)key);
                }
                break;

            default:
                break;
            }
        }

        /* 2. Emulação de um quadro. */
        if (!paused) {
            if (debug && vm.pc + 1 < CHIP8_MEM_SIZE) {
                /* A checagem de limite importa: um programa descontrolado pode
                   deixar `pc` no último byte da memória, e aí `pc + 1` já está
                   fora dos 4 KiB. */
                char mnemonic[32];
                uint16_t op =
                    (uint16_t)((vm.mem[vm.pc] << 8) | vm.mem[vm.pc + 1]);
                chip8_disasm(op, mnemonic, sizeof(mnemonic));
                fprintf(stderr, "%04X: %04X  %-18s I=%03X SP=%u\n", vm.pc, op,
                        mnemonic, vm.I, (unsigned)vm.sp);
            }

            err = chip8_run_frame(&vm, ipf);
            if (err != CHIP8_OK) {
                fprintf(stderr, "erro de execucao em pc=%04X (opcode %04X): %s\n",
                        vm.pc, vm.last_opcode, chip8_strerror(err));
                running = 0;
            }
        }

        /* 3. Som: o núcleo diz apenas "sim" ou "não". */
        beeper.beeping = chip8_beeping(&vm);

        /* 4. Vídeo: converte o framebuffer de 1 byte/pixel para ARGB. */
        for (i = 0; i < CHIP8_SCREEN_PIXELS; i++) {
            pixels[i] = vm.display[i] ? 0xFFEEEEEEu  /* aceso: quase branco */
                                      : 0xFF101018u; /* apagado: quase preto */
        }
        SDL_UpdateTexture(texture, NULL, pixels,
                          CHIP8_SCREEN_W * (int)sizeof(Uint32));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        vm.draw_flag = 0;

        /* 5. Estado na barra de título (o CHIP-8 não tem onde escrever). */
        snprintf(title, sizeof(title), "CHIP-8 — %s | %u ipf | %s%s", rom_path,
                 ipf, paused ? "pausado" : "rodando",
                 vm.waiting_key ? " | aguardando tecla" : "");
        SDL_SetWindowTitle(window, title);

        /* 6. Sincronismo: dorme o que sobrou dos 16,67 ms do quadro. */
        {
            double now = (double)SDL_GetPerformanceCounter();

            next_frame += frame_ticks;
            if (next_frame > now) {
                Uint32 wait_ms =
                    (Uint32)((next_frame - now) * 1000.0 / (double)perf_freq);
                if (wait_ms > 0) {
                    SDL_Delay(wait_ms);
                }
            } else {
                /* Ficamos para trás (máquina lenta, janela sendo arrastada):
                   reancora no presente em vez de tentar recuperar quadros
                   que já passaram. */
                next_frame = now;
            }
        }
    }

    /* --- Encerramento ---------------------------------------------------- */
    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("emulacao encerrada apos %llu ciclos\n",
           (unsigned long long)vm.cycles);
    return 0;
}
