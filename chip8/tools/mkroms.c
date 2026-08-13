/**
 * @file mkroms.c
 * @brief Gera as ROMs de demonstração que acompanham o emulador.
 *
 * As ROMs clássicas do CHIP-8 (PONG, TETRIS, INVADERS...) circulam sem
 * licença clara, então este projeto não as redistribui: em vez disso, monta
 * aqui três programas próprios, escritos e comentados instrução por
 * instrução. Rodar `make roms` produz os arquivos `.ch8` em `roms/`.
 *
 * ## Como os programas são escritos
 *
 * Cada ROM é um vetor de `uint16_t`, uma instrução por posição. Os rótulos
 * (destinos de salto) são declarados como `enum` com o **índice** da
 * instrução dentro do vetor; a macro ADDR() converte esse índice no endereço
 * real de memória. Assim, inserir ou remover uma instrução no meio do
 * programa não exige recalcular nenhum endereço à mão — basta atualizar o
 * `enum`, que fica logo acima do código.
 *
 * ```
 * endereço = 0x200 + 2 * índice
 * ```
 */

#include "chip8.h"

#include <stdio.h>
#include <string.h>

/** Converte o índice de uma instrução no endereço de memória correspondente. */
#define ADDR(index) ((uint16_t)(CHIP8_PROG_START + 2 * (index)))

/* ------------------------------------------------------------------------ */
/* Gravação                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * @brief Grava um programa (e dados opcionais) como arquivo `.ch8`.
 *
 * As instruções do CHIP-8 são big-endian: o byte mais significativo vem
 * primeiro, independentemente da máquina que gerou o arquivo.
 *
 * @param path      Caminho do arquivo a criar.
 * @param code      Vetor de instruções.
 * @param code_len  Quantidade de instruções.
 * @param data      Bytes de dados a anexar depois do código (pode ser NULL).
 * @param data_len  Quantidade de bytes de dados.
 * @return 0 em caso de sucesso, -1 em caso de falha de E/S.
 */
static int write_rom(const char *path, const uint16_t *code, size_t code_len,
                     const uint8_t *data, size_t data_len)
{
    FILE *file = fopen(path, "wb");
    size_t i;

    if (file == NULL) {
        fprintf(stderr, "nao foi possivel criar '%s'\n", path);
        return -1;
    }

    for (i = 0; i < code_len; i++) {
        fputc((code[i] >> 8) & 0xFF, file); /* byte alto primeiro */
        fputc(code[i] & 0xFF, file);
    }
    for (i = 0; i < data_len; i++) {
        fputc(data[i], file);
    }

    if (ferror(file)) {
        fclose(file);
        fprintf(stderr, "erro ao escrever '%s'\n", path);
        return -1;
    }
    fclose(file);

    printf("gerado: %-24s %zu bytes\n", path, code_len * 2 + data_len);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* ROM 1: fonte.ch8                                                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief Desenha os 16 dígitos hexadecimais da fonte embutida em uma grade.
 *
 * Exercita `FX29` (endereço do sprite de um dígito), `DXYN` (desenho) e o
 * controle de fluxo com `SE`/`JP`. É o teste mais rápido de "o emulador está
 * vivo?": se a grade 4x4 aparece completa, fonte, desenho e saltos estão
 * corretos.
 *
 * Registradores: V0 = x, V1 = y, V2 = dígito atual, V3 = temporário,
 * V4 = máscara 3 (para detectar o fim de cada linha de 4 dígitos).
 */
static int build_fonte(const char *path)
{
    /* Índices das instruções usadas como destino de salto. */
    enum {
        L_LOOP = 4,  /* início do laço de desenho          */
        L_END = 16   /* laço infinito que encerra o programa */
    };

    static const uint16_t code[] = {
        /*  0 */ 0x6002, /* LD   V0, 2      ; x inicial                     */
        /*  1 */ 0x6102, /* LD   V1, 2      ; y inicial                     */
        /*  2 */ 0x6200, /* LD   V2, 0      ; primeiro dígito              */
        /*  3 */ 0x6403, /* LD   V4, 3      ; máscara para "múltiplo de 4"  */

        /* L_LOOP: desenha o dígito atual e avança */
        /*  4 */ 0xF229, /* LD   F, V2      ; I = sprite do dígito V2       */
        /*  5 */ 0xD015, /* DRW  V0, V1, 5  ; desenha 5 linhas              */
        /*  6 */ 0x7006, /* ADD  V0, 6      ; x += 6 (4 de largura + espaço)*/
        /*  7 */ 0x7201, /* ADD  V2, 1      ; próximo dígito                */
        /*  8 */ 0x8320, /* LD   V3, V2                                     */
        /*  9 */ 0x8342, /* AND  V3, V4     ; V3 = V2 % 4                   */
        /* 10 */ 0x3300, /* SE   V3, 0      ; fim da linha? pula o JP       */
        /* 11 */ (uint16_t)(0x1000 | ADDR(L_LOOP)), /* JP L_LOOP            */

        /* Fim de uma linha de 4 dígitos: volta o x e desce o y */
        /* 12 */ 0x6002, /* LD   V0, 2      ; x volta ao início             */
        /* 13 */ 0x7106, /* ADD  V1, 6      ; y += 6                        */
        /* 14 */ 0x3210, /* SE   V2, 16     ; desenhou os 16? pula o JP     */
        /* 15 */ (uint16_t)(0x1000 | ADDR(L_LOOP)), /* JP L_LOOP            */

        /* L_END: nada mais a fazer; fica parado mostrando a grade */
        /* 16 */ (uint16_t)(0x1000 | ADDR(L_END)) /* JP L_END               */
    };

    return write_rom(path, code, sizeof(code) / sizeof(code[0]), NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* ROM 2: bola.ch8                                                           */
/* ------------------------------------------------------------------------ */

/**
 * @brief Bola que quica nas quatro bordas da tela, com bipe a cada batida.
 *
 * Exercita a parte "de jogo" do emulador: desenho por XOR (desenhar duas
 * vezes na mesma posição apaga), temporizador de atraso para controlar a
 * velocidade, temporizador de som, sub-rotinas (`CALL`/`RET`) e aritmética
 * com estouro de 8 bits — a inversão de direção é feita com `0 - v`, que
 * transforma 1 em 255 e vice-versa.
 *
 * Registradores: V0 = x, V1 = y, V2 = dx, V3 = dy, V4/V5 = temporizador,
 * V6 = temporário das sub-rotinas.
 */
static int build_bola(const char *path)
{
    enum {
        L_LOOP = 4,   /* início do quadro                    */
        L_WAIT = 8,   /* espera o temporizador de atraso     */
        L_X_MAX = 17, /* checagem da borda direita           */
        L_Y_MIN = 20, /* checagem da borda superior          */
        L_Y_MAX = 23, /* checagem da borda inferior          */
        L_NEXT = 26,  /* volta ao início do laço             */
        L_FLIP_X = 27,/* sub-rotina: inverte a direção em x  */
        L_FLIP_Y = 33,/* sub-rotina: inverte a direção em y  */
        L_SPRITE = 39 /* índice onde os dados do sprite começam */
    };

    static const uint16_t code[] = {
        /*  0 */ 0x6014, /* LD   V0, 20     ; x inicial                     */
        /*  1 */ 0x610A, /* LD   V1, 10     ; y inicial                     */
        /*  2 */ 0x6201, /* LD   V2, 1      ; dx = +1                       */
        /*  3 */ 0x6301, /* LD   V3, 1      ; dy = +1                       */

        /* L_LOOP: desenha a bola na posição atual */
        /*  4 */ (uint16_t)(0xA000 | ADDR(L_SPRITE)), /* LD I, sprite       */
        /*  5 */ 0xD014, /* DRW  V0, V1, 4  ; acende a bola (XOR)           */

        /* Pausa de 2 tiques de 60 Hz para a bola não voar pela tela */
        /*  6 */ 0x6402, /* LD   V4, 2                                      */
        /*  7 */ 0xF415, /* LD   DT, V4     ; delay_timer = 2               */

        /* L_WAIT: gira até o temporizador zerar */
        /*  8 */ 0xF507, /* LD   V5, DT                                     */
        /*  9 */ 0x3500, /* SE   V5, 0      ; zerou? pula o JP              */
        /* 10 */ (uint16_t)(0x1000 | ADDR(L_WAIT)), /* JP L_WAIT            */

        /* 11 */ 0xD014, /* DRW  V0, V1, 4  ; XOR de novo => apaga a bola   */

        /* Move */
        /* 12 */ 0x8024, /* ADD  V0, V2     ; x += dx                       */
        /* 13 */ 0x8134, /* ADD  V1, V3     ; y += dy                       */

        /* Borda esquerda (x == 0) */
        /* 14 */ 0x3000, /* SE   V0, 0      ; se x == 0, pula o JP          */
        /* 15 */ (uint16_t)(0x1000 | ADDR(L_X_MAX)), /* JP L_X_MAX          */
        /* 16 */ (uint16_t)(0x2000 | ADDR(L_FLIP_X)),/* CALL flip_x         */

        /* L_X_MAX: borda direita (x == 60, pois o sprite tem 4 de largura) */
        /* 17 */ 0x303C, /* SE   V0, 60                                     */
        /* 18 */ (uint16_t)(0x1000 | ADDR(L_Y_MIN)), /* JP L_Y_MIN          */
        /* 19 */ (uint16_t)(0x2000 | ADDR(L_FLIP_X)),/* CALL flip_x         */

        /* L_Y_MIN: borda superior (y == 0) */
        /* 20 */ 0x3100, /* SE   V1, 0                                      */
        /* 21 */ (uint16_t)(0x1000 | ADDR(L_Y_MAX)), /* JP L_Y_MAX          */
        /* 22 */ (uint16_t)(0x2000 | ADDR(L_FLIP_Y)),/* CALL flip_y         */

        /* L_Y_MAX: borda inferior (y == 28, altura 32 menos 4 do sprite) */
        /* 23 */ 0x311C, /* SE   V1, 28                                     */
        /* 24 */ (uint16_t)(0x1000 | ADDR(L_NEXT)),  /* JP L_NEXT           */
        /* 25 */ (uint16_t)(0x2000 | ADDR(L_FLIP_Y)),/* CALL flip_y         */

        /* L_NEXT: próximo quadro */
        /* 26 */ (uint16_t)(0x1000 | ADDR(L_LOOP)), /* JP L_LOOP            */

        /* L_FLIP_X: dx = 0 - dx, e um bipe curto */
        /* 27 */ 0x6600, /* LD   V6, 0                                      */
        /* 28 */ 0x8625, /* SUB  V6, V2     ; V6 = 0 - dx  (1 <-> 255)      */
        /* 29 */ 0x8260, /* LD   V2, V6                                     */
        /* 30 */ 0x6602, /* LD   V6, 2                                      */
        /* 31 */ 0xF618, /* LD   ST, V6     ; bipe de 2 tiques              */
        /* 32 */ 0x00EE, /* RET                                             */

        /* L_FLIP_Y: dy = 0 - dy, e um bipe curto */
        /* 33 */ 0x6600, /* LD   V6, 0                                      */
        /* 34 */ 0x8635, /* SUB  V6, V3     ; V6 = 0 - dy                   */
        /* 35 */ 0x8360, /* LD   V3, V6                                     */
        /* 36 */ 0x6602, /* LD   V6, 2                                      */
        /* 37 */ 0xF618, /* LD   ST, V6                                     */
        /* 38 */ 0x00EE  /* RET                                             */
    };

    /*
     * Sprite da bola: 4 linhas, e cada linha usa só os 4 bits mais altos
     * (a largura de um sprite é sempre 8 pixels, mas os 4 de baixo ficam
     * apagados).
     *
     *   0x60 = 0110 0000   .##.
     *   0xF0 = 1111 0000   ####
     *   0xF0 = 1111 0000   ####
     *   0x60 = 0110 0000   .##.
     */
    static const uint8_t sprite[] = {0x60, 0xF0, 0xF0, 0x60};

    return write_rom(path, code, sizeof(code) / sizeof(code[0]), sprite,
                     sizeof(sprite));
}

/* ------------------------------------------------------------------------ */
/* ROM 3: teclado.ch8                                                        */
/* ------------------------------------------------------------------------ */

/**
 * @brief Mostra no centro da tela o dígito da última tecla pressionada.
 *
 * Serve para conferir o mapeamento do teclado do frontend: pressione `Z` e
 * deve aparecer `A`; pressione `X` e deve aparecer `0`, e assim por diante.
 * Exercita `FX0A` (a única instrução que bloqueia a CPU) e `00E0` (limpar
 * a tela).
 */
static int build_teclado(const char *path)
{
    enum {
        L_LOOP = 2 /* volta a esperar a próxima tecla */
    };

    static const uint16_t code[] = {
        /*  0 */ 0x611C, /* LD   V1, 28     ; x, aproximadamente centralizado */
        /*  1 */ 0x620C, /* LD   V2, 12     ; y                              */

        /* L_LOOP */
        /*  2 */ 0xF00A, /* LD   V0, K      ; bloqueia até uma tecla ser dada*/
        /*  3 */ 0x00E0, /* CLS                                              */
        /*  4 */ 0xF029, /* LD   F, V0      ; I = sprite do dígito da tecla  */
        /*  5 */ 0xD125, /* DRW  V1, V2, 5                                   */
        /*  6 */ (uint16_t)(0x1000 | ADDR(L_LOOP)) /* JP L_LOOP              */
    };

    return write_rom(path, code, sizeof(code) / sizeof(code[0]), NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Programa principal                                                        */
/* ------------------------------------------------------------------------ */

/**
 * @brief Gera as três ROMs no diretório indicado (padrão: `roms`).
 *
 * Uso: `mkroms [diretorio]`
 */
int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "roms";
    char path[512];

    snprintf(path, sizeof(path), "%s/fonte.ch8", dir);
    if (build_fonte(path) != 0) {
        return 1;
    }

    snprintf(path, sizeof(path), "%s/bola.ch8", dir);
    if (build_bola(path) != 0) {
        return 1;
    }

    snprintf(path, sizeof(path), "%s/teclado.ch8", dir);
    if (build_teclado(path) != 0) {
        return 1;
    }

    return 0;
}
