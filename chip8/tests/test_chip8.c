/**
 * @file test_chip8.c
 * @brief Suíte de testes do núcleo do emulador.
 *
 * Testes de unidade escritos sem nenhum framework externo: uma macro de
 * verificação, um contador de falhas e uma lista de funções. Rode com
 * `make test`.
 *
 * A estratégia é sempre a mesma: montar uma máquina limpa, escrever a
 * instrução a testar direto na memória em 0x200, executar um passo e
 * conferir o estado resultante. O auxiliar setup_op() faz esse preparo.
 *
 * A cobertura inclui as 35 instruções, os seis quirks configuráveis, os
 * casos de borda do desenho de sprites (colisão, corte e reaparecimento nas
 * bordas), os erros de pilha e de endereço, a espera por tecla e o
 * desmontador.
 */

#include "chip8.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Mini framework de teste                                                   */
/* ------------------------------------------------------------------------ */

/** Quantidade de verificações que falharam na execução inteira. */
static int g_failures = 0;
/** Quantidade total de verificações executadas. */
static int g_checks = 0;
/** Nome do teste em execução, usado nas mensagens de falha. */
static const char *g_current_test = "";

/**
 * @brief Verifica uma condição e registra a falha sem interromper a suíte.
 *
 * Continuar após uma falha faz com que uma única execução mostre todos os
 * problemas de uma vez, em vez de um por rodada.
 */
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FALHA [%s] %s (linha %d)\n", g_current_test, (msg),      \
                   __LINE__);                                                  \
        }                                                                      \
    } while (0)

/** @brief Compara dois inteiros e mostra os valores quando divergem. */
#define CHECK_EQ(got, expected, msg)                                           \
    do {                                                                       \
        long long _got = (long long)(got);                                     \
        long long _exp = (long long)(expected);                                \
        g_checks++;                                                            \
        if (_got != _exp) {                                                    \
            g_failures++;                                                      \
            printf("  FALHA [%s] %s: esperado %lld (0x%llX), obtido %lld "     \
                   "(0x%llX) (linha %d)\n",                                    \
                   g_current_test, (msg), _exp, _exp, _got, _got, __LINE__);   \
        }                                                                      \
    } while (0)

/** @brief Marca o início de um teste (aparece nas mensagens de falha). */
#define TEST(name)                                                             \
    do {                                                                       \
        g_current_test = (name);                                               \
        printf("- %s\n", (name));                                              \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Auxiliares                                                                */
/* ------------------------------------------------------------------------ */

/**
 * @brief Prepara uma máquina com uma única instrução em 0x200.
 *
 * @param vm     Máquina a preparar.
 * @param opcode Instrução a escrever no ponto de entrada.
 * @param quirks Quirks desejados, ou NULL para o padrão COSMAC VIP.
 */
static void setup_op(chip8_t *vm, uint16_t opcode, const chip8_quirks_t *quirks)
{
    chip8_init(vm, quirks);
    vm->mem[CHIP8_PROG_START] = (uint8_t)(opcode >> 8);
    vm->mem[CHIP8_PROG_START + 1] = (uint8_t)(opcode & 0xFF);
}

/** @brief Escreve uma instrução em um endereço qualquer da memória. */
static void put_op(chip8_t *vm, uint16_t addr, uint16_t opcode)
{
    vm->mem[addr] = (uint8_t)(opcode >> 8);
    vm->mem[addr + 1] = (uint8_t)(opcode & 0xFF);
}

/* ------------------------------------------------------------------------ */
/* Testes: inicialização                                                     */
/* ------------------------------------------------------------------------ */

static void test_init(void)
{
    chip8_t vm;
    int i;
    int display_limpo = 1;

    TEST("inicializacao");

    chip8_init(&vm, NULL);

    CHECK_EQ(vm.pc, CHIP8_PROG_START, "pc comeca em 0x200");
    CHECK_EQ(vm.sp, 0, "pilha comeca vazia");
    CHECK_EQ(vm.I, 0, "I comeca zerado");
    CHECK_EQ(vm.delay_timer, 0, "delay_timer comeca zerado");
    CHECK_EQ(vm.sound_timer, 0, "sound_timer comeca zerado");
    CHECK_EQ(vm.cycles, 0, "contador de ciclos comeca zerado");

    for (i = 0; i < CHIP8_NUM_REGS; i++) {
        CHECK_EQ(vm.V[i], 0, "registradores comecam zerados");
    }
    for (i = 0; i < CHIP8_SCREEN_PIXELS; i++) {
        if (vm.display[i] != 0) {
            display_limpo = 0;
        }
    }
    CHECK(display_limpo, "tela comeca apagada");

    /* A fonte deve estar instalada: o dígito 0 é F0 90 90 90 F0. */
    CHECK_EQ(vm.mem[CHIP8_FONT_ADDR + 0], 0xF0, "fonte: primeira linha do 0");
    CHECK_EQ(vm.mem[CHIP8_FONT_ADDR + 1], 0x90, "fonte: segunda linha do 0");
    /* E o dígito F (o 16º) termina em 0x80. */
    CHECK_EQ(vm.mem[CHIP8_FONT_ADDR + 79], 0x80, "fonte: ultima linha do F");

    /* Quirks padrão = COSMAC VIP. */
    CHECK(vm.quirks.vf_reset, "padrao: vf_reset ligado");
    CHECK(vm.quirks.memory_increment_i, "padrao: memory_increment_i ligado");
    CHECK(!vm.quirks.shift_vx_in_place, "padrao: shift no lugar desligado");
}

static void test_load_rom(void)
{
    chip8_t vm;
    uint8_t rom[] = {0x12, 0x34, 0xAB, 0xCD};
    uint8_t grande[CHIP8_MAX_ROM_SIZE + 1];

    TEST("carga de ROM");

    chip8_init(&vm, NULL);
    CHECK_EQ(chip8_load_rom(&vm, rom, sizeof(rom)), CHIP8_OK, "ROM aceita");
    CHECK_EQ(vm.mem[0x200], 0x12, "primeiro byte em 0x200");
    CHECK_EQ(vm.mem[0x203], 0xCD, "ultimo byte na sequencia");

    memset(grande, 0, sizeof(grande));
    CHECK_EQ(chip8_load_rom(&vm, grande, sizeof(grande)), CHIP8_ERR_ROM_TOO_BIG,
             "ROM grande demais e rejeitada");

    CHECK_EQ(chip8_load_rom(NULL, rom, sizeof(rom)), CHIP8_ERR_INVALID_ARG,
             "maquina nula e rejeitada");
    CHECK_EQ(chip8_load_rom_file(&vm, "/caminho/que/nao/existe.ch8"),
             CHIP8_ERR_IO, "arquivo inexistente e rejeitado");
}

static void test_reset(void)
{
    chip8_t vm;

    TEST("reset preserva a memoria");

    chip8_init(&vm, NULL);
    vm.mem[0x300] = 0x42;
    vm.V[3] = 9;
    vm.I = 0x400;
    vm.pc = 0x350;
    vm.sp = 2;
    vm.display[10] = 1;
    vm.delay_timer = 5;

    chip8_reset(&vm);

    CHECK_EQ(vm.mem[0x300], 0x42, "memoria (ROM) preservada");
    CHECK_EQ(vm.V[3], 0, "registradores zerados");
    CHECK_EQ(vm.I, 0, "I zerado");
    CHECK_EQ(vm.pc, CHIP8_PROG_START, "pc de volta a 0x200");
    CHECK_EQ(vm.sp, 0, "pilha esvaziada");
    CHECK_EQ(vm.display[10], 0, "tela limpa");
    CHECK_EQ(vm.delay_timer, 0, "temporizador zerado");
}

/* ------------------------------------------------------------------------ */
/* Testes: fluxo de controle                                                 */
/* ------------------------------------------------------------------------ */

static void test_fluxo(void)
{
    chip8_t vm;

    TEST("saltos, chamadas e retornos");

    /* 1NNN — JP */
    setup_op(&vm, 0x1345, NULL);
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x345, "1NNN salta para NNN");

    /* 2NNN — CALL empilha o endereço de retorno */
    setup_op(&vm, 0x2400, NULL);
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x400, "2NNN salta para NNN");
    CHECK_EQ(vm.sp, 1, "2NNN empilha um nivel");
    CHECK_EQ(vm.stack[0], 0x202, "endereco de retorno e a instrucao seguinte");

    /* 00EE — RET desempilha */
    put_op(&vm, 0x400, 0x00EE);
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x202, "00EE volta ao endereco empilhado");
    CHECK_EQ(vm.sp, 0, "00EE desempilha");

    /* 3XNN — SE Vx, NN */
    setup_op(&vm, 0x3042, NULL);
    vm.V[0] = 0x42;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x204, "3XNN pula quando VX == NN");

    setup_op(&vm, 0x3042, NULL);
    vm.V[0] = 0x00;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x202, "3XNN nao pula quando VX != NN");

    /* 4XNN — SNE Vx, NN */
    setup_op(&vm, 0x4142, NULL);
    vm.V[1] = 0x00;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x204, "4XNN pula quando VX != NN");

    /* 5XY0 — SE Vx, Vy */
    setup_op(&vm, 0x5120, NULL);
    vm.V[1] = 7;
    vm.V[2] = 7;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x204, "5XY0 pula quando VX == VY");

    /* 9XY0 — SNE Vx, Vy */
    setup_op(&vm, 0x9120, NULL);
    vm.V[1] = 7;
    vm.V[2] = 8;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x204, "9XY0 pula quando VX != VY");

    /* BNNN — JP V0, addr (comportamento COSMAC VIP) */
    setup_op(&vm, 0xB300, NULL);
    vm.V[0] = 0x20;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x320, "BNNN soma V0 ao endereco");
}

static void test_erros_de_pilha(void)
{
    chip8_t vm;
    int i;

    TEST("erros de pilha e de endereco");

    /* RET sem CALL. */
    setup_op(&vm, 0x00EE, NULL);
    CHECK_EQ(chip8_step(&vm), CHIP8_ERR_STACK_UNDERFLOW, "RET com pilha vazia");
    CHECK_EQ(vm.pc, CHIP8_PROG_START, "pc preservado no erro");

    /* 17 chamadas seguidas estouram a pilha de 16 níveis. */
    chip8_init(&vm, NULL);
    for (i = 0; i < CHIP8_STACK_SIZE; i++) {
        uint16_t addr = (uint16_t)(0x200 + i * 2);
        put_op(&vm, addr, (uint16_t)(0x2000 | (addr + 2)));
    }
    for (i = 0; i < CHIP8_STACK_SIZE; i++) {
        CHECK_EQ(chip8_step(&vm), CHIP8_OK, "chamadas dentro do limite");
    }
    put_op(&vm, vm.pc, 0x2999);
    CHECK_EQ(chip8_step(&vm), CHIP8_ERR_STACK_OVERFLOW, "17a chamada estoura");

    /* Opcode inexistente. */
    setup_op(&vm, 0x8F0F, NULL);
    CHECK_EQ(chip8_step(&vm), CHIP8_ERR_UNKNOWN_OPCODE, "8XYF nao existe");
    CHECK_EQ(vm.pc, CHIP8_PROG_START, "pc aponta para a instrucao invalida");

    /* FX65 lendo além do fim da memória. */
    setup_op(&vm, 0xFF65, NULL);
    vm.I = CHIP8_MEM_SIZE - 4;
    CHECK_EQ(chip8_step(&vm), CHIP8_ERR_BAD_ADDRESS, "FX65 fora da memoria");
}

/* ------------------------------------------------------------------------ */
/* Testes: aritmética e lógica                                               */
/* ------------------------------------------------------------------------ */

static void test_aritmetica(void)
{
    chip8_t vm;

    TEST("aritmetica e logica");

    /* 6XNN / 7XNN */
    setup_op(&vm, 0x6A2F, NULL);
    chip8_step(&vm);
    CHECK_EQ(vm.V[0xA], 0x2F, "6XNN carrega a constante");

    setup_op(&vm, 0x7A01, NULL);
    vm.V[0xA] = 0xFF;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0xA], 0x00, "7XNN estoura em 8 bits sem tocar em VF");
    CHECK_EQ(vm.V[0xF], 0, "7XNN nao altera VF");

    /* 8XY4 — soma com transporte */
    setup_op(&vm, 0x8014, NULL);
    vm.V[0] = 200;
    vm.V[1] = 100;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 44, "8XY4 resultado truncado (300 & 0xFF)");
    CHECK_EQ(vm.V[0xF], 1, "8XY4 marca o transporte");

    setup_op(&vm, 0x8014, NULL);
    vm.V[0] = 10;
    vm.V[1] = 20;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 30, "8XY4 soma simples");
    CHECK_EQ(vm.V[0xF], 0, "8XY4 zera VF quando nao ha transporte");

    /* 8XY5 — subtração; VF = 1 quando NÃO há empréstimo */
    setup_op(&vm, 0x8015, NULL);
    vm.V[0] = 20;
    vm.V[1] = 5;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 15, "8XY5 subtrai");
    CHECK_EQ(vm.V[0xF], 1, "8XY5 VF=1 sem emprestimo");

    setup_op(&vm, 0x8015, NULL);
    vm.V[0] = 5;
    vm.V[1] = 20;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 241, "8XY5 estoura para baixo (5-20 = 241)");
    CHECK_EQ(vm.V[0xF], 0, "8XY5 VF=0 com emprestimo");

    /* 8XY7 — SUBN: VX = VY - VX */
    setup_op(&vm, 0x8017, NULL);
    vm.V[0] = 5;
    vm.V[1] = 20;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 15, "8XY7 calcula VY - VX");
    CHECK_EQ(vm.V[0xF], 1, "8XY7 VF=1 sem emprestimo");

    /* Caso de borda: o destino é o próprio VF — o flag deve prevalecer. */
    setup_op(&vm, 0x8F14, NULL);
    vm.V[0xF] = 200;
    vm.V[1] = 100;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0xF], 1, "8XY4 com destino VF deixa o flag, nao a soma");
}

static void test_quirks_logicos(void)
{
    chip8_t vm;
    chip8_quirks_t vip = chip8_quirks_vip();
    chip8_quirks_t schip = chip8_quirks_schip();

    TEST("quirk: VF zerado pelas operacoes logicas");

    setup_op(&vm, 0x8011, &vip); /* OR */
    vm.V[0] = 0x0F;
    vm.V[1] = 0xF0;
    vm.V[0xF] = 1;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 0xFF, "8XY1 calcula o OR");
    CHECK_EQ(vm.V[0xF], 0, "VIP: 8XY1 zera VF");

    setup_op(&vm, 0x8011, &schip);
    vm.V[0] = 0x0F;
    vm.V[1] = 0xF0;
    vm.V[0xF] = 1;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0xF], 1, "SCHIP: 8XY1 nao mexe em VF");

    TEST("quirk: deslocamento no lugar");

    /* VIP: 8XY6 copia VY, desloca e guarda em VX. */
    setup_op(&vm, 0x8016, &vip);
    vm.V[0] = 0x00;
    vm.V[1] = 0x05; /* 0000 0101 */
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 0x02, "VIP: 8XY6 desloca VY");
    CHECK_EQ(vm.V[0xF], 1, "VIP: 8XY6 guarda o bit perdido");

    /* SCHIP: 8XY6 ignora VY e desloca o próprio VX. */
    setup_op(&vm, 0x8016, &schip);
    vm.V[0] = 0x05;
    vm.V[1] = 0x00;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 0x02, "SCHIP: 8XY6 desloca VX no lugar");
    CHECK_EQ(vm.V[0xF], 1, "SCHIP: 8XY6 guarda o bit perdido");

    /* 8XYE — deslocamento à esquerda. */
    setup_op(&vm, 0x801E, &schip);
    vm.V[0] = 0x81; /* 1000 0001 */
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 0x02, "SCHIP: 8XYE desloca a esquerda");
    CHECK_EQ(vm.V[0xF], 1, "SCHIP: 8XYE guarda o bit mais alto");

    TEST("quirk: BNNN x BXNN");

    setup_op(&vm, 0xB201, &schip);
    vm.V[0] = 0x10;
    vm.V[2] = 0x05;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x206, "SCHIP: BXNN usa VX (0x201 + V2)");
}

/* ------------------------------------------------------------------------ */
/* Testes: memória, fonte e BCD                                              */
/* ------------------------------------------------------------------------ */

static void test_memoria(void)
{
    chip8_t vm;
    chip8_quirks_t vip = chip8_quirks_vip();
    chip8_quirks_t schip = chip8_quirks_schip();

    TEST("memoria, fonte e BCD");

    /* ANNN */
    setup_op(&vm, 0xA123, NULL);
    chip8_step(&vm);
    CHECK_EQ(vm.I, 0x123, "ANNN carrega I");

    /* FX1E */
    setup_op(&vm, 0xF01E, NULL);
    vm.I = 0x100;
    vm.V[0] = 0x20;
    chip8_step(&vm);
    CHECK_EQ(vm.I, 0x120, "FX1E soma VX a I");

    /* FX29 — endereço do sprite do dígito */
    setup_op(&vm, 0xF029, NULL);
    vm.V[0] = 0xA;
    chip8_step(&vm);
    CHECK_EQ(vm.I, CHIP8_FONT_ADDR + 0xA * 5, "FX29 aponta para o digito A");
    CHECK_EQ(vm.mem[vm.I], 0xF0, "sprite do A comeca com 0xF0");

    /* FX33 — BCD de 254 => 2, 5, 4 */
    setup_op(&vm, 0xF033, NULL);
    vm.V[0] = 254;
    vm.I = 0x300;
    chip8_step(&vm);
    CHECK_EQ(vm.mem[0x300], 2, "BCD: centena");
    CHECK_EQ(vm.mem[0x301], 5, "BCD: dezena");
    CHECK_EQ(vm.mem[0x302], 4, "BCD: unidade");

    /* FX33 de 0 => 0, 0, 0 */
    setup_op(&vm, 0xF033, NULL);
    vm.V[0] = 0;
    vm.I = 0x300;
    chip8_step(&vm);
    CHECK_EQ(vm.mem[0x300], 0, "BCD de zero: centena");
    CHECK_EQ(vm.mem[0x302], 0, "BCD de zero: unidade");

    /* FX55 — salva V0..V3 */
    setup_op(&vm, 0xF355, &schip);
    vm.I = 0x400;
    vm.V[0] = 0x11;
    vm.V[1] = 0x22;
    vm.V[2] = 0x33;
    vm.V[3] = 0x44;
    vm.V[4] = 0x55;
    chip8_step(&vm);
    CHECK_EQ(vm.mem[0x400], 0x11, "FX55 grava V0");
    CHECK_EQ(vm.mem[0x403], 0x44, "FX55 grava ate VX inclusive");
    CHECK_EQ(vm.mem[0x404], 0x00, "FX55 nao passa de VX");
    CHECK_EQ(vm.I, 0x400, "SCHIP: FX55 nao mexe em I");

    /* FX55 com o quirk de incremento (COSMAC VIP) */
    setup_op(&vm, 0xF355, &vip);
    vm.I = 0x400;
    chip8_step(&vm);
    CHECK_EQ(vm.I, 0x404, "VIP: FX55 incrementa I em X+1");

    /* FX65 — carrega V0..V2 */
    setup_op(&vm, 0xF265, &schip);
    vm.I = 0x500;
    vm.mem[0x500] = 0xAA;
    vm.mem[0x501] = 0xBB;
    vm.mem[0x502] = 0xCC;
    vm.mem[0x503] = 0xDD;
    chip8_step(&vm);
    CHECK_EQ(vm.V[0], 0xAA, "FX65 carrega V0");
    CHECK_EQ(vm.V[2], 0xCC, "FX65 carrega ate VX inclusive");
    CHECK_EQ(vm.V[3], 0x00, "FX65 nao passa de VX");
}

/* ------------------------------------------------------------------------ */
/* Testes: vídeo                                                             */
/* ------------------------------------------------------------------------ */

static void test_desenho(void)
{
    chip8_t vm;
    chip8_quirks_t q = chip8_quirks_vip();

    TEST("desenho de sprites");

    /* Desenha uma linha cheia (0xFF) em (0,0). */
    q.clipping = 1;
    setup_op(&vm, 0xD011, &q);
    vm.I = 0x300;
    vm.mem[0x300] = 0xFF;
    vm.V[0] = 0;
    vm.V[1] = 0;
    chip8_step(&vm);
    CHECK_EQ(chip8_pixel(&vm, 0, 0), 1, "primeiro pixel aceso");
    CHECK_EQ(chip8_pixel(&vm, 7, 0), 1, "oitavo pixel aceso");
    CHECK_EQ(chip8_pixel(&vm, 8, 0), 0, "nono pixel intacto");
    CHECK_EQ(vm.V[0xF], 0, "sem colisao na primeira vez");
    CHECK(vm.draw_flag, "draw_flag ligada apos desenhar");

    /* Desenhar de novo no mesmo lugar apaga (XOR) e marca colisão. */
    vm.pc = CHIP8_PROG_START;
    chip8_step(&vm);
    CHECK_EQ(chip8_pixel(&vm, 0, 0), 0, "XOR apaga o pixel");
    CHECK_EQ(vm.V[0xF], 1, "colisao detectada");

    TEST("desenho: posicao inicial sempre sofre modulo");

    setup_op(&vm, 0xD011, &q);
    vm.I = 0x300;
    vm.mem[0x300] = 0x80; /* apenas o bit mais alto */
    vm.V[0] = 64 + 5;     /* 69 % 64 = 5 */
    vm.V[1] = 32 + 3;     /* 35 % 32 = 3 */
    chip8_step(&vm);
    CHECK_EQ(chip8_pixel(&vm, 5, 3), 1, "coordenada inicial e reduzida");

    TEST("quirk: corte x reaparecimento nas bordas");

    /* Com clipping: o que passa da borda direita some. */
    q.clipping = 1;
    setup_op(&vm, 0xD011, &q);
    vm.I = 0x300;
    vm.mem[0x300] = 0xFF;
    vm.V[0] = 60; /* 60..67, mas a tela acaba em 63 */
    vm.V[1] = 0;
    chip8_step(&vm);
    CHECK_EQ(chip8_pixel(&vm, 63, 0), 1, "ultimo pixel visivel aceso");
    CHECK_EQ(chip8_pixel(&vm, 0, 0), 0, "com corte, nada reaparece a esquerda");

    /* Sem clipping: o excedente reaparece do outro lado. */
    q.clipping = 0;
    setup_op(&vm, 0xD011, &q);
    vm.I = 0x300;
    vm.mem[0x300] = 0xFF;
    vm.V[0] = 60;
    vm.V[1] = 0;
    chip8_step(&vm);
    CHECK_EQ(chip8_pixel(&vm, 63, 0), 1, "ultimo pixel da borda aceso");
    CHECK_EQ(chip8_pixel(&vm, 0, 0), 1, "sem corte, reaparece a esquerda");
    CHECK_EQ(chip8_pixel(&vm, 3, 0), 1, "reaparecimento cobre 4 pixels");
    CHECK_EQ(chip8_pixel(&vm, 4, 0), 0, "e nao mais que isso");

    TEST("00E0 limpa a tela");

    setup_op(&vm, 0x00E0, &q);
    vm.display[100] = 1;
    chip8_step(&vm);
    CHECK_EQ(vm.display[100], 0, "CLS apaga tudo");
    CHECK(vm.draw_flag, "CLS pede redesenho");
}

/* ------------------------------------------------------------------------ */
/* Testes: teclado, temporizadores e execução por quadro                     */
/* ------------------------------------------------------------------------ */

static void test_teclado(void)
{
    chip8_t vm;

    TEST("teclado: SKP e SKNP");

    setup_op(&vm, 0xE09E, NULL); /* SKP V0 */
    vm.V[0] = 0x7;
    chip8_key_down(&vm, 0x7);
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x204, "EX9E pula com a tecla pressionada");

    setup_op(&vm, 0xE09E, NULL);
    vm.V[0] = 0x7;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x202, "EX9E nao pula com a tecla solta");

    setup_op(&vm, 0xE0A1, NULL); /* SKNP V0 */
    vm.V[0] = 0x7;
    chip8_step(&vm);
    CHECK_EQ(vm.pc, 0x204, "EXA1 pula com a tecla solta");

    TEST("teclado: FX0A bloqueia ate soltar a tecla");

    setup_op(&vm, 0xF10A, NULL); /* LD V1, K */
    chip8_step(&vm);
    CHECK(vm.waiting_key, "FX0A entra em espera");
    CHECK_EQ(vm.pc, 0x202, "pc ja passou da instrucao");

    chip8_step(&vm); /* ciclos passam sem efeito */
    chip8_step(&vm);
    CHECK(vm.waiting_key, "continua esperando sem tecla");
    CHECK_EQ(vm.V[1], 0, "nada escrito ainda");

    chip8_key_down(&vm, 0xC);
    CHECK(vm.waiting_key, "pressionar ainda nao libera");
    CHECK_EQ(vm.V[1], 0, "o valor so vem ao soltar");

    chip8_key_up(&vm, 0xC);
    CHECK(!vm.waiting_key, "soltar a tecla libera a CPU");
    CHECK_EQ(vm.V[1], 0xC, "o registrador recebe a tecla");

    /* Soltar uma tecla diferente da pressionada não deve liberar. */
    setup_op(&vm, 0xF20A, NULL);
    chip8_step(&vm);
    chip8_key_down(&vm, 0x3);
    chip8_key_up(&vm, 0x5);
    CHECK(vm.waiting_key, "soltar outra tecla nao libera");
}

static void test_temporizadores(void)
{
    chip8_t vm;

    TEST("temporizadores e som");

    setup_op(&vm, 0xF015, NULL); /* LD DT, V0 */
    vm.V[0] = 3;
    chip8_step(&vm);
    CHECK_EQ(vm.delay_timer, 3, "FX15 carrega o delay_timer");

    chip8_tick_timers(&vm);
    CHECK_EQ(vm.delay_timer, 2, "tique decrementa");
    chip8_tick_timers(&vm);
    chip8_tick_timers(&vm);
    CHECK_EQ(vm.delay_timer, 0, "chega a zero");
    chip8_tick_timers(&vm);
    CHECK_EQ(vm.delay_timer, 0, "nao passa de zero");

    setup_op(&vm, 0xF018, NULL); /* LD ST, V0 */
    vm.V[0] = 2;
    chip8_step(&vm);
    CHECK(chip8_beeping(&vm), "som ligado com ST > 0");
    chip8_tick_timers(&vm);
    chip8_tick_timers(&vm);
    CHECK(!chip8_beeping(&vm), "som desliga com ST = 0");

    /* FX07 lê o delay de volta. */
    setup_op(&vm, 0xF507, NULL);
    vm.delay_timer = 42;
    chip8_step(&vm);
    CHECK_EQ(vm.V[5], 42, "FX07 le o delay_timer");
}

static void test_run_frame(void)
{
    chip8_t vm;
    chip8_quirks_t q = chip8_quirks_vip();

    TEST("execucao por quadro");

    /* Um laço `JP` sobre si mesmo: gasta exatamente `ipf` ciclos. */
    q.display_wait = 0;
    chip8_init(&vm, &q);
    put_op(&vm, 0x200, 0x1200);
    vm.delay_timer = 10;
    CHECK_EQ(chip8_run_frame(&vm, 10), CHIP8_OK, "quadro sem erros");
    CHECK_EQ(vm.cycles, 10, "executou ipf instrucoes");
    CHECK_EQ(vm.delay_timer, 9, "timers andam uma vez por quadro");

    TEST("quirk: DXYN espera o sincronismo vertical");

    /* Com display_wait, o quadro para logo depois do desenho. */
    q.display_wait = 1;
    chip8_init(&vm, &q);
    vm.I = 0x300;
    put_op(&vm, 0x200, 0xD001); /* DRW */
    put_op(&vm, 0x202, 0x1202); /* laço infinito */
    chip8_run_frame(&vm, 100);
    CHECK_EQ(vm.cycles, 1, "para no primeiro DXYN do quadro");

    /* Sem o quirk, o quadro inteiro é executado. */
    q.display_wait = 0;
    chip8_init(&vm, &q);
    vm.I = 0x300;
    put_op(&vm, 0x200, 0xD001);
    put_op(&vm, 0x202, 0x1202);
    chip8_run_frame(&vm, 100);
    CHECK_EQ(vm.cycles, 100, "sem espera, usa todo o orcamento");

    TEST("execucao por quadro para na espera por tecla");

    chip8_init(&vm, NULL);
    put_op(&vm, 0x200, 0xF00A);
    chip8_run_frame(&vm, 50);
    CHECK_EQ(vm.cycles, 1, "para assim que FX0A bloqueia");
}

/* ------------------------------------------------------------------------ */
/* Testes: aleatoriedade e desmontador                                       */
/* ------------------------------------------------------------------------ */

static void test_aleatorio(void)
{
    chip8_t vm;
    int i;
    int mascara_respeitada = 1;
    int houve_variacao = 0;
    uint8_t primeiro;

    TEST("CXNN: aleatorio com mascara");

    chip8_init(&vm, NULL);
    chip8_seed(&vm, 12345);
    for (i = 0; i < 64; i++) {
        put_op(&vm, 0x200, 0xC00F); /* RND V0, 0x0F */
        vm.pc = 0x200;
        chip8_step(&vm);
        if ((vm.V[0] & 0xF0) != 0) {
            mascara_respeitada = 0;
        }
        if (i == 0) {
            primeiro = vm.V[0];
        } else if (vm.V[0] != primeiro) {
            houve_variacao = 1;
        }
    }
    CHECK(mascara_respeitada, "CXNN aplica a mascara NN");
    CHECK(houve_variacao, "CXNN produz valores diferentes");

    TEST("CXNN: a mesma semente repete a sequencia");
    {
        chip8_t a, b;
        int iguais = 1;

        chip8_init(&a, NULL);
        chip8_init(&b, NULL);
        chip8_seed(&a, 777);
        chip8_seed(&b, 777);

        for (i = 0; i < 32; i++) {
            put_op(&a, 0x200, 0xC0FF);
            put_op(&b, 0x200, 0xC0FF);
            a.pc = 0x200;
            b.pc = 0x200;
            chip8_step(&a);
            chip8_step(&b);
            if (a.V[0] != b.V[0]) {
                iguais = 0;
            }
        }
        CHECK(iguais, "mesma semente, mesma sequencia");
    }
}

static void test_disasm(void)
{
    char buf[32];

    TEST("desmontador");

    chip8_disasm(0x00E0, buf, sizeof(buf));
    CHECK(strcmp(buf, "CLS") == 0, "00E0 => CLS");

    chip8_disasm(0x00EE, buf, sizeof(buf));
    CHECK(strcmp(buf, "RET") == 0, "00EE => RET");

    chip8_disasm(0x1234, buf, sizeof(buf));
    CHECK(strcmp(buf, "JP #234") == 0, "1NNN => JP");

    chip8_disasm(0x6A12, buf, sizeof(buf));
    CHECK(strcmp(buf, "LD VA, #12") == 0, "6XNN => LD");

    chip8_disasm(0xD015, buf, sizeof(buf));
    CHECK(strcmp(buf, "DRW V0, V1, 5") == 0, "DXYN => DRW");

    chip8_disasm(0xF033, buf, sizeof(buf));
    CHECK(strcmp(buf, "LD B, V0") == 0, "FX33 => LD B");

    chip8_disasm(0xF265, buf, sizeof(buf));
    CHECK(strcmp(buf, "LD V2, [I]") == 0, "FX65 => LD Vx, [I]");

    chip8_disasm(0x8FFF, buf, sizeof(buf));
    CHECK(strcmp(buf, "DW #8FFF") == 0, "opcode invalido vira dado");

    /* Buffer pequeno demais não pode estourar. */
    {
        char pequeno[4];
        size_t n = chip8_disasm(0xD015, pequeno, sizeof(pequeno));
        CHECK(n < sizeof(pequeno), "respeita o tamanho do buffer");
        CHECK(pequeno[sizeof(pequeno) - 1] == '\0', "termina a string");
    }
}

/* ------------------------------------------------------------------------ */
/* Teste de integração: roda uma das ROMs de demonstração                     */
/* ------------------------------------------------------------------------ */

/**
 * @brief Executa a ROM `fonte` (embutida aqui) e confere o resultado na tela.
 *
 * É o teste de mais alto nível da suíte: monta o mesmo programa gerado por
 * `mkroms.c`, roda até ele atingir o laço final e verifica que os 16 dígitos
 * foram desenhados na grade esperada.
 */
static void test_integracao_fonte(void)
{
    chip8_t vm;
    chip8_quirks_t q = chip8_quirks_vip();
    static const uint16_t code[] = {
        0x6002, 0x6102, 0x6200, 0x6403, 0xF229, 0xD015, 0x7006, 0x7201,
        0x8320, 0x8342, 0x3300, 0x1208, 0x6002, 0x7106, 0x3210, 0x1208,
        0x1220
    };
    /* Índice 4 => endereço 0x208; índice 16 => 0x220 (conferidos acima). */
    int i;
    int pixels_acesos = 0;

    TEST("integracao: ROM 'fonte' desenha os 16 digitos");

    q.display_wait = 0; /* acelera: não precisamos simular o vsync aqui */
    chip8_init(&vm, &q);

    for (i = 0; i < (int)(sizeof(code) / sizeof(code[0])); i++) {
        put_op(&vm, (uint16_t)(0x200 + i * 2), code[i]);
    }

    /* 2000 ciclos são muito mais do que o programa precisa. */
    for (i = 0; i < 2000; i++) {
        if (chip8_step(&vm) != CHIP8_OK) {
            CHECK(0, "a ROM nao deveria produzir erros");
            break;
        }
    }

    CHECK_EQ(vm.pc, 0x220, "termina parada no laco final");

    for (i = 0; i < CHIP8_SCREEN_PIXELS; i++) {
        pixels_acesos += vm.display[i];
    }
    /* Cada dígito acende entre 10 e 20 pixels; 16 dígitos dão bem mais que 100. */
    CHECK(pixels_acesos > 100, "a grade de digitos foi desenhada");

    /* O dígito 0 (canto superior esquerdo) começa em (2,2) com a linha cheia
       "####" — 4 pixels acesos. */
    CHECK_EQ(chip8_pixel(&vm, 2, 2), 1, "topo do digito 0");
    CHECK_EQ(chip8_pixel(&vm, 5, 2), 1, "topo do digito 0 (4 pixels)");
    CHECK_EQ(chip8_pixel(&vm, 3, 3), 0, "miolo do digito 0 e vazado");
}

/* ------------------------------------------------------------------------ */
/* Programa principal                                                        */
/* ------------------------------------------------------------------------ */

int main(void)
{
    printf("Testes do nucleo CHIP-8\n");
    printf("=======================\n");

    test_init();
    test_load_rom();
    test_reset();
    test_fluxo();
    test_erros_de_pilha();
    test_aritmetica();
    test_quirks_logicos();
    test_memoria();
    test_desenho();
    test_teclado();
    test_temporizadores();
    test_run_frame();
    test_aleatorio();
    test_disasm();
    test_integracao_fonte();

    printf("=======================\n");
    printf("%d verificacoes, %d falha(s)\n", g_checks, g_failures);

    if (g_failures == 0) {
        printf("TUDO CERTO\n");
        return 0;
    }
    printf("HOUVE FALHAS\n");
    return 1;
}
