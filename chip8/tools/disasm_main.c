/**
 * @file disasm_main.c
 * @brief Ferramenta de linha de comando que lista uma ROM em assembly.
 *
 * ```
 * chip8-disasm rom.ch8
 * ```
 *
 * Cada linha da saída traz o endereço de memória, o opcode em hexadecimal e
 * o mnemônico correspondente:
 *
 * ```
 * 0200: 6002  LD V0, #02
 * 0202: 6102  LD V1, #02
 * 0204: F229  LD F, V2
 * ```
 *
 * Como no CHIP-8 código e dados dividem a mesma memória e o arquivo não tem
 * cabeçalho algum, é impossível saber com certeza onde termina o programa e
 * começam os sprites: bytes de dados aparecem como instruções esquisitas ou
 * como `DW #XXXX`. Isso é característica do formato, não defeito da
 * ferramenta.
 */

#include "chip8.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *file;
    uint8_t rom[CHIP8_MAX_ROM_SIZE];
    size_t len;
    size_t i;

    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Uso: %s rom.ch8\n", argv[0]);
        printf("Lista o conteudo de uma ROM CHIP-8 em assembly.\n");
        return (argc < 2) ? 1 : 0;
    }

    file = fopen(argv[1], "rb");
    if (file == NULL) {
        fprintf(stderr, "nao foi possivel abrir '%s'\n", argv[1]);
        return 1;
    }
    len = fread(rom, 1, sizeof(rom), file);
    fclose(file);

    if (len == 0) {
        fprintf(stderr, "arquivo vazio\n");
        return 1;
    }

    for (i = 0; i + 1 < len; i += 2) {
        uint16_t opcode = (uint16_t)((rom[i] << 8) | rom[i + 1]);
        char text[32];

        chip8_disasm(opcode, text, sizeof(text));
        printf("%04X: %04X  %s\n", (unsigned)(CHIP8_PROG_START + i), opcode,
               text);
    }

    /* Um arquivo de tamanho ímpar deixa um byte solto no fim — geralmente
       preenchimento ou o último byte de um sprite. */
    if (len % 2 != 0) {
        printf("%04X: %02X    DB #%02X\n",
               (unsigned)(CHIP8_PROG_START + len - 1), rom[len - 1],
               rom[len - 1]);
    }

    return 0;
}
