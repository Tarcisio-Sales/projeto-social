# ==========================================================================
#  Emulador CHIP-8 — Makefile
# ==========================================================================
#
#  Alvos principais:
#
#    make            compila tudo o que for possível no sistema atual
#    make test       compila e roda a suíte de testes do núcleo
#    make roms       gera as ROMs de demonstração em roms/
#    make run        gera as ROMs e abre a demo da bola no terminal
#    make sdl        compila o frontend gráfico (exige SDL2)
#    make clean      apaga os artefatos de compilação
#    make help       lista os alvos
#
#  O frontend SDL2 só entra no alvo padrão quando o `pkg-config` encontra a
#  biblioteca; sem ela, o resto do projeto compila normalmente e o `make`
#  avisa o que instalar.
# ==========================================================================

CC      ?= cc
CSTD    ?= -std=c99

# -Wall -Wextra: o básico. As demais pegam erros que costumam passar batidos
# em código de emulação (conversões estreitando valores, sombreamento de
# variáveis, declarações implícitas).
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
           -Wmissing-prototypes -Wpointer-arith -Wcast-align
OPT     ?= -O2
CFLAGS  := $(CSTD) $(WARN) $(OPT) -Iinclude $(CFLAGS)
LDFLAGS ?=

BUILD   := build
ROMDIR  := roms

# --- Fontes ---------------------------------------------------------------
CORE_SRC := src/chip8.c src/disasm.c
CORE_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC))

TERM_BIN   := $(BUILD)/chip8-term
SDL_BIN    := $(BUILD)/chip8-sdl
TEST_BIN   := $(BUILD)/test_chip8
MKROMS_BIN := $(BUILD)/mkroms
DISASM_BIN := $(BUILD)/chip8-disasm

# --- Detecção do SDL2 -----------------------------------------------------
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
HAVE_SDL   := $(if $(SDL_LIBS),yes,no)

# --- Alvo padrão ----------------------------------------------------------
.PHONY: all
all: $(TERM_BIN) $(DISASM_BIN) $(TEST_BIN) roms
ifeq ($(HAVE_SDL),yes)
all: $(SDL_BIN)
else
all:
	@echo ""
	@echo "aviso: SDL2 nao encontrado — apenas o frontend de terminal foi compilado."
	@echo "       Debian/Ubuntu: sudo apt install libsdl2-dev"
	@echo "       Fedora:        sudo dnf install SDL2-devel"
	@echo "       macOS:         brew install sdl2"
	@echo ""
endif

# --- Regras de compilação -------------------------------------------------
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TERM_BIN): $(CORE_OBJ) $(BUILD)/frontend/main_term.o
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)

$(SDL_BIN): $(CORE_OBJ) frontend/main_sdl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $^ -o $@ $(LDFLAGS) $(SDL_LIBS)

$(TEST_BIN): $(CORE_OBJ) $(BUILD)/tests/test_chip8.o
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)

$(MKROMS_BIN): $(BUILD)/tools/mkroms.o
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)

$(DISASM_BIN): $(CORE_OBJ) $(BUILD)/tools/disasm_main.o
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)

# --- Alvos de conveniência ------------------------------------------------
.PHONY: sdl
sdl:
ifeq ($(HAVE_SDL),yes)
	@$(MAKE) --no-print-directory $(SDL_BIN)
	@echo "compilado: $(SDL_BIN)"
else
	@echo "erro: SDL2 nao encontrado pelo pkg-config."
	@echo "      Debian/Ubuntu: sudo apt install libsdl2-dev"
	@exit 1
endif

.PHONY: test
test: $(TEST_BIN)
	@./$(TEST_BIN)

.PHONY: roms
roms: $(MKROMS_BIN)
	@mkdir -p $(ROMDIR)
	@./$(MKROMS_BIN) $(ROMDIR)

.PHONY: run
run: $(TERM_BIN) roms
	./$(TERM_BIN) $(ROMDIR)/bola.ch8

# Compila com sanitizadores de endereço e de comportamento indefinido e roda
# os testes — é assim que se pega acesso fora de vetor no framebuffer.
#
# Sai em um diretório próprio (build-asan) porque os objetos instrumentados
# não podem ser ligados a um binário comum: misturá-los com build/ quebraria
# o `make` seguinte com erros de símbolo do ASan.
.PHONY: sanitize
sanitize:
	@$(MAKE) --no-print-directory test BUILD=$(BUILD)-asan \
		OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,undefined"

.PHONY: clean
clean:
	rm -rf $(BUILD) $(BUILD)-asan

.PHONY: distclean
distclean: clean
	rm -f $(ROMDIR)/*.ch8

.PHONY: help
help:
	@echo "Emulador CHIP-8 — alvos disponiveis:"
	@echo "  make            compila nucleo, frontend(s), testes e ROMs"
	@echo "  make test       roda a suite de testes"
	@echo "  make sanitize   roda os testes com ASan/UBSan"
	@echo "  make roms       gera as ROMs de demonstracao"
	@echo "  make run        roda a demo da bola no terminal"
	@echo "  make sdl        compila o frontend grafico (exige SDL2)"
	@echo "  make clean      apaga os artefatos de compilacao"
	@echo ""
	@echo "  SDL2 detectado: $(HAVE_SDL)"
