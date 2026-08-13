/**
 * @file disasm.c
 * @brief Desmontador de instruções CHIP-8.
 *
 * Traduz um opcode de 16 bits para a sintaxe de montagem clássica do CHIP-8
 * (a mesma do manual do COSMAC VIP e do Cowgod's Chip-8 Technical Reference):
 * mnemônico em maiúsculas, registradores como `V0`..`VF`, endereços em
 * hexadecimal com três dígitos.
 *
 * É usado pelo modo de traço dos frontends (`--debug`) e pela ferramenta de
 * linha de comando `chip8-disasm`. O desmontador não guarda estado nenhum,
 * então serve tanto para acompanhar a execução ao vivo quanto para listar um
 * arquivo inteiro.
 *
 * Uma ressalva inerente ao formato: como código e dados dividem a mesma
 * memória e não há cabeçalho que os separe, listar uma ROM inteira sempre
 * mostra sprites e tabelas como se fossem instruções. Opcodes que não
 * correspondem a nenhuma instrução saem como `DW #XXXX` ("define word").
 */

#include "chip8.h"

#include <stdarg.h>
#include <stdio.h>

/**
 * @brief Formata o texto no buffer, protegendo contra buffer nulo ou vazio.
 *
 * @return Quantidade de caracteres realmente escritos (sem o terminador).
 */
static size_t chip8_fmt(char *buf, size_t n, const char *fmt, ...)
{
    va_list args;
    int written;

    if (buf == NULL || n == 0) {
        return 0;
    }

    va_start(args, fmt);
    written = vsnprintf(buf, n, fmt, args);
    va_end(args);

    if (written < 0) {
        buf[0] = '\0';
        return 0;
    }
    /* Em caso de truncamento, snprintf devolve o tamanho que *seria* preciso. */
    if ((size_t)written >= n) {
        return n - 1;
    }
    return (size_t)written;
}

size_t chip8_disasm(uint16_t opcode, char *buf, size_t n)
{
    unsigned x = (opcode & 0x0F00u) >> 8;
    unsigned y = (opcode & 0x00F0u) >> 4;
    unsigned nib = opcode & 0x000Fu;
    unsigned nn = opcode & 0x00FFu;
    unsigned nnn = opcode & 0x0FFFu;

    if (buf == NULL || n == 0) {
        return 0;
    }

    switch (opcode & 0xF000u) {

    case 0x0000:
        if (opcode == 0x00E0) {
            return chip8_fmt(buf, n, "CLS");
        }
        if (opcode == 0x00EE) {
            return chip8_fmt(buf, n, "RET");
        }
        /* 0NNN: chamada de código nativo do RCA 1802, ignorada pelo núcleo. */
        return chip8_fmt(buf, n, "SYS #%03X", nnn);

    case 0x1000:
        return chip8_fmt(buf, n, "JP #%03X", nnn);

    case 0x2000:
        return chip8_fmt(buf, n, "CALL #%03X", nnn);

    case 0x3000:
        return chip8_fmt(buf, n, "SE V%X, #%02X", x, nn);

    case 0x4000:
        return chip8_fmt(buf, n, "SNE V%X, #%02X", x, nn);

    case 0x5000:
        if (nib == 0x0) {
            return chip8_fmt(buf, n, "SE V%X, V%X", x, y);
        }
        break;

    case 0x6000:
        return chip8_fmt(buf, n, "LD V%X, #%02X", x, nn);

    case 0x7000:
        return chip8_fmt(buf, n, "ADD V%X, #%02X", x, nn);

    case 0x8000:
        switch (nib) {
        case 0x0:
            return chip8_fmt(buf, n, "LD V%X, V%X", x, y);
        case 0x1:
            return chip8_fmt(buf, n, "OR V%X, V%X", x, y);
        case 0x2:
            return chip8_fmt(buf, n, "AND V%X, V%X", x, y);
        case 0x3:
            return chip8_fmt(buf, n, "XOR V%X, V%X", x, y);
        case 0x4:
            return chip8_fmt(buf, n, "ADD V%X, V%X", x, y);
        case 0x5:
            return chip8_fmt(buf, n, "SUB V%X, V%X", x, y);
        case 0x6:
            return chip8_fmt(buf, n, "SHR V%X, V%X", x, y);
        case 0x7:
            return chip8_fmt(buf, n, "SUBN V%X, V%X", x, y);
        case 0xE:
            return chip8_fmt(buf, n, "SHL V%X, V%X", x, y);
        default:
            break;
        }
        break;

    case 0x9000:
        if (nib == 0x0) {
            return chip8_fmt(buf, n, "SNE V%X, V%X", x, y);
        }
        break;

    case 0xA000:
        return chip8_fmt(buf, n, "LD I, #%03X", nnn);

    case 0xB000:
        return chip8_fmt(buf, n, "JP V0, #%03X", nnn);

    case 0xC000:
        return chip8_fmt(buf, n, "RND V%X, #%02X", x, nn);

    case 0xD000:
        return chip8_fmt(buf, n, "DRW V%X, V%X, %u", x, y, nib);

    case 0xE000:
        if (nn == 0x9E) {
            return chip8_fmt(buf, n, "SKP V%X", x);
        }
        if (nn == 0xA1) {
            return chip8_fmt(buf, n, "SKNP V%X", x);
        }
        break;

    case 0xF000:
        switch (nn) {
        case 0x07:
            return chip8_fmt(buf, n, "LD V%X, DT", x);
        case 0x0A:
            return chip8_fmt(buf, n, "LD V%X, K", x);
        case 0x15:
            return chip8_fmt(buf, n, "LD DT, V%X", x);
        case 0x18:
            return chip8_fmt(buf, n, "LD ST, V%X", x);
        case 0x1E:
            return chip8_fmt(buf, n, "ADD I, V%X", x);
        case 0x29:
            return chip8_fmt(buf, n, "LD F, V%X", x);
        case 0x33:
            return chip8_fmt(buf, n, "LD B, V%X", x);
        case 0x55:
            return chip8_fmt(buf, n, "LD [I], V%X", x);
        case 0x65:
            return chip8_fmt(buf, n, "LD V%X, [I]", x);
        default:
            break;
        }
        break;

    default:
        break;
    }

    /* Não é instrução válida: provavelmente dado (sprite, tabela, texto). */
    return chip8_fmt(buf, n, "DW #%04X", opcode);
}
