/**
 * @file chip8.c
 * @brief Implementação do núcleo do emulador CHIP-8.
 *
 * O arquivo segue a ordem natural de leitura de um emulador:
 *
 *  1. Fonte hexadecimal embutida;
 *  2. Gerador pseudoaleatório;
 *  3. Ciclo de vida (init / reset / seed);
 *  4. Carga de ROM;
 *  5. Auxiliares de memória e de desenho;
 *  6. O interpretador propriamente dito (busca → decodificação → execução);
 *  7. Temporizadores, teclado e utilidades.
 *
 * ## Como o interpretador decodifica uma instrução
 *
 * Toda instrução do CHIP-8 tem exatamente 16 bits (2 bytes, big-endian).
 * Os campos são sempre extraídos das mesmas posições:
 *
 * ```
 *      15..12   11..8    7..4     3..0
 *     +-------+-------+-------+-------+
 *     |   H   |   X   |   Y   |   N   |
 *     +-------+-------+-------+-------+
 *             \_______________________/   NNN = endereço de 12 bits
 *                     \_______________/   NN  = constante de 8 bits
 * ```
 *
 * - `H`   escolhe a família da instrução (primeiro nibble);
 * - `X`   índice de um registrador V;
 * - `Y`   índice de um segundo registrador V;
 * - `N`   constante de 4 bits;
 * - `NN`  constante de 8 bits (byte baixo);
 * - `NNN` endereço de 12 bits.
 *
 * A execução é um `switch` sobre `H`, com `switch` aninhado onde a família
 * precisa de mais bits para se desambiguar (famílias 0, 5, 8, 9, E e F).
 */

#include "chip8.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* 1. Fonte hexadecimal embutida                                             */
/* ------------------------------------------------------------------------ */

/**
 * Sprites dos 16 dígitos hexadecimais, 5 bytes (linhas) por caractere,
 * 4 pixels de largura (os 4 bits altos de cada byte). A instrução `FX29`
 * aponta `I` para o caractere desejado dentro desta tabela.
 *
 * O dígito 0, por exemplo, é `F0 90 90 90 F0`:
 *
 * ```
 *  1111 ....   ####
 *  1001 ....   #..#
 *  1001 ....   #..#
 *  1001 ....   #..#
 *  1111 ....   ####
 * ```
 */
static const uint8_t chip8_font[CHIP8_FONT_SIZE] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, /* 0 */
    0x20, 0x60, 0x20, 0x20, 0x70, /* 1 */
    0xF0, 0x10, 0xF0, 0x80, 0xF0, /* 2 */
    0xF0, 0x10, 0xF0, 0x10, 0xF0, /* 3 */
    0x90, 0x90, 0xF0, 0x10, 0x10, /* 4 */
    0xF0, 0x80, 0xF0, 0x10, 0xF0, /* 5 */
    0xF0, 0x80, 0xF0, 0x90, 0xF0, /* 6 */
    0xF0, 0x10, 0x20, 0x40, 0x40, /* 7 */
    0xF0, 0x90, 0xF0, 0x90, 0xF0, /* 8 */
    0xF0, 0x90, 0xF0, 0x10, 0xF0, /* 9 */
    0xF0, 0x90, 0xF0, 0x90, 0x90, /* A */
    0xE0, 0x90, 0xE0, 0x90, 0xE0, /* B */
    0xF0, 0x80, 0x80, 0x80, 0xF0, /* C */
    0xE0, 0x90, 0x90, 0x90, 0xE0, /* D */
    0xF0, 0x80, 0xF0, 0x80, 0xF0, /* E */
    0xF0, 0x80, 0xF0, 0x80, 0x80  /* F */
};

/* ------------------------------------------------------------------------ */
/* 2. Gerador pseudoaleatório                                                */
/* ------------------------------------------------------------------------ */

/** Semente padrão, usada quando o chamador pede semente 0. */
#define CHIP8_DEFAULT_SEED 0x2A6F1D3Bu

/**
 * @brief Gera o próximo número pseudoaleatório (xorshift32).
 *
 * Embutir o gerador em vez de usar `rand()` mantém o núcleo determinístico e
 * independente do estado global da libc: a mesma semente sempre produz a
 * mesma partida, o que permite testar `CXNN` de forma reprodutível.
 *
 * @param vm Máquina cujo estado de RNG será avançado.
 * @return Valor pseudoaleatório de 32 bits.
 */
static uint32_t chip8_rand(chip8_t *vm)
{
    uint32_t x = vm->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rng_state = x;
    return x;
}

/* ------------------------------------------------------------------------ */
/* 3. Ciclo de vida                                                          */
/* ------------------------------------------------------------------------ */

chip8_quirks_t chip8_quirks_vip(void)
{
    chip8_quirks_t q;
    q.vf_reset = 1;
    q.memory_increment_i = 1;
    q.display_wait = 1;
    q.clipping = 1;
    q.shift_vx_in_place = 0;
    q.jump_with_offset_vx = 0;
    return q;
}

chip8_quirks_t chip8_quirks_schip(void)
{
    chip8_quirks_t q;
    q.vf_reset = 0;
    q.memory_increment_i = 0;
    q.display_wait = 0;
    q.clipping = 1;
    q.shift_vx_in_place = 1;
    q.jump_with_offset_vx = 1;
    return q;
}

void chip8_init(chip8_t *vm, const chip8_quirks_t *quirks)
{
    if (vm == NULL) {
        return;
    }

    memset(vm, 0, sizeof(*vm));
    memcpy(&vm->mem[CHIP8_FONT_ADDR], chip8_font, CHIP8_FONT_SIZE);

    vm->pc = CHIP8_PROG_START;
    vm->rng_state = CHIP8_DEFAULT_SEED;
    vm->quirks = (quirks != NULL) ? *quirks : chip8_quirks_vip();
}

void chip8_reset(chip8_t *vm)
{
    if (vm == NULL) {
        return;
    }

    /* Tudo volta ao estado inicial, exceto a memória (que guarda a ROM),
       os quirks escolhidos e a semente do gerador aleatório. */
    memset(vm->V, 0, sizeof(vm->V));
    memset(vm->stack, 0, sizeof(vm->stack));
    memset(vm->display, 0, sizeof(vm->display));
    memset(vm->keys, 0, sizeof(vm->keys));

    vm->I = 0;
    vm->pc = CHIP8_PROG_START;
    vm->sp = 0;
    vm->delay_timer = 0;
    vm->sound_timer = 0;
    vm->draw_flag = 1; /* força o frontend a limpar a tela */
    vm->waiting_key = 0;
    vm->waiting_reg = 0;
    vm->waiting_pressed_key = 0;
    vm->waiting_has_key = 0;
    vm->vblank_wait = 0;
    vm->cycles = 0;
    vm->last_opcode = 0;
}

void chip8_seed(chip8_t *vm, uint32_t seed)
{
    if (vm == NULL) {
        return;
    }
    /* xorshift trava permanentemente em zero, então 0 vira a semente padrão. */
    vm->rng_state = (seed != 0) ? seed : CHIP8_DEFAULT_SEED;
}

/* ------------------------------------------------------------------------ */
/* 4. Carga de ROM                                                           */
/* ------------------------------------------------------------------------ */

chip8_err_t chip8_load_rom(chip8_t *vm, const uint8_t *data, size_t len)
{
    if (vm == NULL || (data == NULL && len > 0)) {
        return CHIP8_ERR_INVALID_ARG;
    }
    if (len > (size_t)CHIP8_MAX_ROM_SIZE) {
        return CHIP8_ERR_ROM_TOO_BIG;
    }

    memcpy(&vm->mem[CHIP8_PROG_START], data, len);
    return CHIP8_OK;
}

chip8_err_t chip8_load_rom_file(chip8_t *vm, const char *path)
{
    uint8_t buffer[CHIP8_MAX_ROM_SIZE];
    FILE *file;
    size_t read_bytes;

    if (vm == NULL || path == NULL) {
        return CHIP8_ERR_INVALID_ARG;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return CHIP8_ERR_IO;
    }

    read_bytes = fread(buffer, 1, sizeof(buffer), file);

    /* Se ainda há byte sobrando no arquivo, ele não cabe na memória. */
    if (read_bytes == sizeof(buffer) && fgetc(file) != EOF) {
        fclose(file);
        return CHIP8_ERR_ROM_TOO_BIG;
    }
    if (ferror(file)) {
        fclose(file);
        return CHIP8_ERR_IO;
    }
    fclose(file);

    return chip8_load_rom(vm, buffer, read_bytes);
}

/* ------------------------------------------------------------------------ */
/* 5. Auxiliares de memória e de desenho                                     */
/* ------------------------------------------------------------------------ */

/**
 * @brief Verifica se a faixa [addr, addr + len) cabe na memória.
 *
 * @return Diferente de zero se o acesso é válido.
 */
static int chip8_range_ok(uint16_t addr, uint16_t len)
{
    return ((uint32_t)addr + (uint32_t)len) <= (uint32_t)CHIP8_MEM_SIZE;
}

/**
 * @brief Executa `DXYN`: desenha um sprite por XOR e apura a colisão.
 *
 * O sprite tem 8 pixels de largura (um bit por pixel, do mais significativo
 * para o menos) e `n` linhas, lidas a partir do endereço em `I`.
 *
 * Regras implementadas:
 * - a posição inicial sempre sofre módulo (`VX % 64`, `VY % 32`), em todos
 *   os intérpretes conhecidos;
 * - o que ultrapassa a borda é cortado ou reaparece do outro lado, conforme
 *   o quirk `clipping`;
 * - `VF` recebe 1 se algum pixel aceso foi apagado (colisão), senão 0. `VF`
 *   é zerado *antes* do desenho, então um `DXYN` sem colisão sempre deixa
 *   `VF` em 0.
 *
 * @param vm Máquina em execução.
 * @param x_reg Índice do registrador com a coordenada X.
 * @param y_reg Índice do registrador com a coordenada Y.
 * @param n Altura do sprite, de 0 a 15 linhas.
 *
 * @retval CHIP8_OK              Sprite desenhado.
 * @retval CHIP8_ERR_BAD_ADDRESS O sprite não cabe dentro da memória.
 */
static chip8_err_t chip8_draw_sprite(chip8_t *vm, uint8_t x_reg, uint8_t y_reg,
                                     uint8_t n)
{
    int origin_x = vm->V[x_reg] % CHIP8_SCREEN_W;
    int origin_y = vm->V[y_reg] % CHIP8_SCREEN_H;
    int row;

    if (!chip8_range_ok(vm->I, n)) {
        return CHIP8_ERR_BAD_ADDRESS;
    }

    vm->V[0xF] = 0;

    for (row = 0; row < (int)n; row++) {
        uint8_t bits = vm->mem[vm->I + row];
        int py = origin_y + row;
        int col;

        if (py >= CHIP8_SCREEN_H) {
            if (vm->quirks.clipping) {
                break; /* as linhas seguintes também estariam fora */
            }
            py %= CHIP8_SCREEN_H;
        }

        for (col = 0; col < 8; col++) {
            int px;
            int index;

            if ((bits & (0x80u >> col)) == 0) {
                continue; /* bit apagado no sprite não altera a tela */
            }

            px = origin_x + col;
            if (px >= CHIP8_SCREEN_W) {
                if (vm->quirks.clipping) {
                    break; /* as colunas seguintes também estariam fora */
                }
                px %= CHIP8_SCREEN_W;
            }

            index = py * CHIP8_SCREEN_W + px;
            if (vm->display[index]) {
                vm->V[0xF] = 1; /* pixel aceso apagado => colisão */
            }
            vm->display[index] ^= 1;
        }
    }

    vm->draw_flag = 1;

    /* No hardware original o desenho acontecia durante o retraço vertical:
       a CPU ficava parada até o próximo quadro. */
    if (vm->quirks.display_wait) {
        vm->vblank_wait = 1;
    }

    return CHIP8_OK;
}

/* ------------------------------------------------------------------------ */
/* 6. O interpretador                                                        */
/* ------------------------------------------------------------------------ */

chip8_err_t chip8_step(chip8_t *vm)
{
    uint16_t opcode;
    uint16_t pc_before;
    uint8_t x, y, n;
    uint8_t nn;
    uint16_t nnn;

    if (vm == NULL) {
        return CHIP8_ERR_INVALID_ARG;
    }

    /* `FX0A` deixou a máquina bloqueada: o ciclo passa, nada acontece. */
    if (vm->waiting_key) {
        vm->cycles++;
        return CHIP8_OK;
    }

    /* --- Busca ------------------------------------------------------------
       As instruções são big-endian: o byte em `pc` é o mais significativo. */
    if (!chip8_range_ok(vm->pc, 2)) {
        return CHIP8_ERR_BAD_ADDRESS;
    }

    pc_before = vm->pc;
    opcode = (uint16_t)((vm->mem[vm->pc] << 8) | vm->mem[vm->pc + 1]);
    vm->last_opcode = opcode;
    vm->pc += 2; /* aponta para a próxima instrução antes de executar */
    vm->cycles++;

    /* --- Decodificação: extrai os campos comuns a todas as famílias ------ */
    x = (uint8_t)((opcode & 0x0F00u) >> 8);
    y = (uint8_t)((opcode & 0x00F0u) >> 4);
    n = (uint8_t)(opcode & 0x000Fu);
    nn = (uint8_t)(opcode & 0x00FFu);
    nnn = (uint16_t)(opcode & 0x0FFFu);

    /* --- Execução -------------------------------------------------------- */
    switch (opcode & 0xF000u) {

    case 0x0000:
        switch (opcode) {
        case 0x00E0: /* CLS — limpa a tela. */
            memset(vm->display, 0, sizeof(vm->display));
            vm->draw_flag = 1;
            break;

        case 0x00EE: /* RET — retorna da sub-rotina. */
            if (vm->sp == 0) {
                vm->pc = pc_before;
                return CHIP8_ERR_STACK_UNDERFLOW;
            }
            vm->pc = vm->stack[--vm->sp];
            break;

        default:
            /* `0NNN` (SYS addr) chamava código de máquina do RCA 1802 no
               COSMAC VIP. Nenhuma ROM moderna usa, e emular seria emular
               outro processador: a instrução é ignorada de propósito. */
            break;
        }
        break;

    case 0x1000: /* 1NNN — JP addr: salta para NNN. */
        vm->pc = nnn;
        break;

    case 0x2000: /* 2NNN — CALL addr: empilha o retorno e salta. */
        if (vm->sp >= CHIP8_STACK_SIZE) {
            vm->pc = pc_before;
            return CHIP8_ERR_STACK_OVERFLOW;
        }
        vm->stack[vm->sp++] = vm->pc;
        vm->pc = nnn;
        break;

    case 0x3000: /* 3XNN — SE Vx, NN: pula a próxima se VX == NN. */
        if (vm->V[x] == nn) {
            vm->pc += 2;
        }
        break;

    case 0x4000: /* 4XNN — SNE Vx, NN: pula a próxima se VX != NN. */
        if (vm->V[x] != nn) {
            vm->pc += 2;
        }
        break;

    case 0x5000: /* 5XY0 — SE Vx, Vy: pula a próxima se VX == VY. */
        if (n != 0x0) {
            vm->pc = pc_before;
            return CHIP8_ERR_UNKNOWN_OPCODE;
        }
        if (vm->V[x] == vm->V[y]) {
            vm->pc += 2;
        }
        break;

    case 0x6000: /* 6XNN — LD Vx, NN: carrega a constante. */
        vm->V[x] = nn;
        break;

    case 0x7000: /* 7XNN — ADD Vx, NN: soma sem afetar VF (estoura em 8 bits). */
        vm->V[x] = (uint8_t)(vm->V[x] + nn);
        break;

    case 0x8000: /* Família aritmética/lógica: o último nibble diz qual é. */
        switch (n) {
        case 0x0: /* 8XY0 — LD Vx, Vy. */
            vm->V[x] = vm->V[y];
            break;

        case 0x1: /* 8XY1 — OR Vx, Vy. */
            vm->V[x] |= vm->V[y];
            if (vm->quirks.vf_reset) {
                vm->V[0xF] = 0;
            }
            break;

        case 0x2: /* 8XY2 — AND Vx, Vy. */
            vm->V[x] &= vm->V[y];
            if (vm->quirks.vf_reset) {
                vm->V[0xF] = 0;
            }
            break;

        case 0x3: /* 8XY3 — XOR Vx, Vy. */
            vm->V[x] ^= vm->V[y];
            if (vm->quirks.vf_reset) {
                vm->V[0xF] = 0;
            }
            break;

        case 0x4: { /* 8XY4 — ADD Vx, Vy: VF = 1 se houve transporte. */
            uint16_t sum = (uint16_t)(vm->V[x] + vm->V[y]);
            vm->V[x] = (uint8_t)sum;
            /* VF é escrito por último: se X for F, o flag prevalece. */
            vm->V[0xF] = (sum > 0xFF) ? 1 : 0;
            break;
        }

        case 0x5: { /* 8XY5 — SUB Vx, Vy: VF = 1 se NÃO houve empréstimo. */
            uint8_t borrow = (vm->V[x] >= vm->V[y]) ? 1 : 0;
            vm->V[x] = (uint8_t)(vm->V[x] - vm->V[y]);
            vm->V[0xF] = borrow;
            break;
        }

        case 0x6: { /* 8XY6 — SHR: desloca 1 bit à direita; VF = bit perdido. */
            uint8_t src = vm->quirks.shift_vx_in_place ? vm->V[x] : vm->V[y];
            uint8_t lost = (uint8_t)(src & 0x1u);
            vm->V[x] = (uint8_t)(src >> 1);
            vm->V[0xF] = lost;
            break;
        }

        case 0x7: { /* 8XY7 — SUBN Vx, Vy: VX = VY - VX. */
            uint8_t borrow = (vm->V[y] >= vm->V[x]) ? 1 : 0;
            vm->V[x] = (uint8_t)(vm->V[y] - vm->V[x]);
            vm->V[0xF] = borrow;
            break;
        }

        case 0xE: { /* 8XYE — SHL: desloca 1 bit à esquerda; VF = bit perdido. */
            uint8_t src = vm->quirks.shift_vx_in_place ? vm->V[x] : vm->V[y];
            uint8_t lost = (uint8_t)((src & 0x80u) ? 1 : 0);
            vm->V[x] = (uint8_t)(src << 1);
            vm->V[0xF] = lost;
            break;
        }

        default:
            vm->pc = pc_before;
            return CHIP8_ERR_UNKNOWN_OPCODE;
        }
        break;

    case 0x9000: /* 9XY0 — SNE Vx, Vy: pula a próxima se VX != VY. */
        if (n != 0x0) {
            vm->pc = pc_before;
            return CHIP8_ERR_UNKNOWN_OPCODE;
        }
        if (vm->V[x] != vm->V[y]) {
            vm->pc += 2;
        }
        break;

    case 0xA000: /* ANNN — LD I, addr. */
        vm->I = nnn;
        break;

    case 0xB000: /* BNNN — JP V0, addr (ou BXNN, conforme o quirk). */
        if (vm->quirks.jump_with_offset_vx) {
            vm->pc = (uint16_t)(nnn + vm->V[x]);
        } else {
            vm->pc = (uint16_t)(nnn + vm->V[0]);
        }
        break;

    case 0xC000: /* CXNN — RND Vx, NN: aleatório mascarado por NN. */
        vm->V[x] = (uint8_t)(chip8_rand(vm) & nn);
        break;

    case 0xD000: { /* DXYN — DRW: desenha sprite e apura colisão. */
        chip8_err_t err = chip8_draw_sprite(vm, x, y, n);
        if (err != CHIP8_OK) {
            vm->pc = pc_before;
            return err;
        }
        break;
    }

    case 0xE000:
        switch (nn) {
        case 0x9E: /* EX9E — SKP Vx: pula se a tecla em VX está pressionada. */
            if (vm->keys[vm->V[x] & 0x0Fu]) {
                vm->pc += 2;
            }
            break;

        case 0xA1: /* EXA1 — SKNP Vx: pula se a tecla em VX está solta. */
            if (!vm->keys[vm->V[x] & 0x0Fu]) {
                vm->pc += 2;
            }
            break;

        default:
            vm->pc = pc_before;
            return CHIP8_ERR_UNKNOWN_OPCODE;
        }
        break;

    case 0xF000:
        switch (nn) {
        case 0x07: /* FX07 — LD Vx, DT. */
            vm->V[x] = vm->delay_timer;
            break;

        case 0x0A: /* FX0A — LD Vx, K: bloqueia até uma tecla ser digitada. */
            vm->waiting_key = 1;
            vm->waiting_reg = x;
            vm->waiting_has_key = 0;
            vm->waiting_pressed_key = 0;
            break;

        case 0x15: /* FX15 — LD DT, Vx. */
            vm->delay_timer = vm->V[x];
            break;

        case 0x18: /* FX18 — LD ST, Vx: enquanto ST > 0, o alto-falante soa. */
            vm->sound_timer = vm->V[x];
            break;

        case 0x1E: /* FX1E — ADD I, Vx (não mexe em VF nesta implementação). */
            vm->I = (uint16_t)(vm->I + vm->V[x]);
            break;

        case 0x29: /* FX29 — LD F, Vx: I aponta para o sprite do dígito. */
            vm->I = (uint16_t)(CHIP8_FONT_ADDR +
                               (vm->V[x] & 0x0Fu) * CHIP8_FONT_CHAR_H);
            break;

        case 0x33: /* FX33 — LD B, Vx: grava VX em BCD (centena/dezena/unidade). */
            if (!chip8_range_ok(vm->I, 3)) {
                vm->pc = pc_before;
                return CHIP8_ERR_BAD_ADDRESS;
            }
            vm->mem[vm->I] = (uint8_t)(vm->V[x] / 100);
            vm->mem[vm->I + 1] = (uint8_t)((vm->V[x] / 10) % 10);
            vm->mem[vm->I + 2] = (uint8_t)(vm->V[x] % 10);
            break;

        case 0x55: { /* FX55 — LD [I], Vx: salva V0..VX na memória. */
            int i;
            if (!chip8_range_ok(vm->I, (uint16_t)(x + 1))) {
                vm->pc = pc_before;
                return CHIP8_ERR_BAD_ADDRESS;
            }
            for (i = 0; i <= (int)x; i++) {
                vm->mem[vm->I + i] = vm->V[i];
            }
            if (vm->quirks.memory_increment_i) {
                vm->I = (uint16_t)(vm->I + x + 1);
            }
            break;
        }

        case 0x65: { /* FX65 — LD Vx, [I]: carrega V0..VX da memória. */
            int i;
            if (!chip8_range_ok(vm->I, (uint16_t)(x + 1))) {
                vm->pc = pc_before;
                return CHIP8_ERR_BAD_ADDRESS;
            }
            for (i = 0; i <= (int)x; i++) {
                vm->V[i] = vm->mem[vm->I + i];
            }
            if (vm->quirks.memory_increment_i) {
                vm->I = (uint16_t)(vm->I + x + 1);
            }
            break;
        }

        default:
            vm->pc = pc_before;
            return CHIP8_ERR_UNKNOWN_OPCODE;
        }
        break;

    default:
        vm->pc = pc_before;
        return CHIP8_ERR_UNKNOWN_OPCODE;
    }

    return CHIP8_OK;
}

chip8_err_t chip8_run_frame(chip8_t *vm, unsigned ipf)
{
    chip8_err_t err = CHIP8_OK;
    unsigned i;

    if (vm == NULL) {
        return CHIP8_ERR_INVALID_ARG;
    }

    vm->vblank_wait = 0;

    for (i = 0; i < ipf; i++) {
        err = chip8_step(vm);
        if (err != CHIP8_OK) {
            break;
        }
        /* Sem sentido continuar gastando ciclos: ou a CPU está travada
           esperando uma tecla, ou o desenho pediu sincronismo vertical. */
        if (vm->waiting_key || vm->vblank_wait) {
            break;
        }
    }

    chip8_tick_timers(vm);
    return err;
}

/* ------------------------------------------------------------------------ */
/* 7. Temporizadores, teclado e utilidades                                   */
/* ------------------------------------------------------------------------ */

void chip8_tick_timers(chip8_t *vm)
{
    if (vm == NULL) {
        return;
    }
    if (vm->delay_timer > 0) {
        vm->delay_timer--;
    }
    if (vm->sound_timer > 0) {
        vm->sound_timer--;
    }
    vm->vblank_wait = 0; /* chegou o novo quadro: `DXYN` pode desenhar de novo */
}

void chip8_key_down(chip8_t *vm, uint8_t key)
{
    if (vm == NULL || key >= CHIP8_NUM_KEYS) {
        return;
    }

    vm->keys[key] = 1;

    /* Durante um `FX0A`, guardamos qual tecla desceu; o valor só é entregue
       quando ela subir (veja chip8_key_up). */
    if (vm->waiting_key && !vm->waiting_has_key) {
        vm->waiting_pressed_key = key;
        vm->waiting_has_key = 1;
    }
}

void chip8_key_up(chip8_t *vm, uint8_t key)
{
    if (vm == NULL || key >= CHIP8_NUM_KEYS) {
        return;
    }

    vm->keys[key] = 0;

    /* O interpretador original só devolvia a tecla depois de solta — é o que
       impede uma única batida de tecla de ser lida por vários `FX0A`
       seguidos. */
    if (vm->waiting_key && vm->waiting_has_key &&
        vm->waiting_pressed_key == key) {
        vm->V[vm->waiting_reg] = key;
        vm->waiting_key = 0;
        vm->waiting_has_key = 0;
        vm->waiting_pressed_key = 0;
    }
}

int chip8_beeping(const chip8_t *vm)
{
    return (vm != NULL && vm->sound_timer > 0) ? 1 : 0;
}

int chip8_pixel(const chip8_t *vm, int x, int y)
{
    if (vm == NULL || x < 0 || y < 0 || x >= CHIP8_SCREEN_W ||
        y >= CHIP8_SCREEN_H) {
        return 0;
    }
    return vm->display[y * CHIP8_SCREEN_W + x] ? 1 : 0;
}

const char *chip8_strerror(chip8_err_t err)
{
    switch (err) {
    case CHIP8_OK:
        return "sem erro";
    case CHIP8_ERR_UNKNOWN_OPCODE:
        return "instrucao desconhecida";
    case CHIP8_ERR_STACK_OVERFLOW:
        return "estouro de pilha (CALL alem de 16 niveis)";
    case CHIP8_ERR_STACK_UNDERFLOW:
        return "pilha vazia (RET sem CALL correspondente)";
    case CHIP8_ERR_BAD_ADDRESS:
        return "acesso a memoria fora dos 4 KiB";
    case CHIP8_ERR_ROM_TOO_BIG:
        return "ROM grande demais para a memoria";
    case CHIP8_ERR_IO:
        return "falha de leitura do arquivo";
    case CHIP8_ERR_INVALID_ARG:
        return "argumento invalido";
    default:
        return "erro desconhecido";
    }
}
