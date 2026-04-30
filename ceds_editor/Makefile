# ─────────────────────────────────────────────────────────────────────────────
# Makefile — CEDS Editor (Proyecto 3: Optimización de Bus I/O)
#
# Targets principales:
#   make          → Compila editor y herramienta de generación de tests
#   make clean    → Elimina objetos y binarios
#   make valgrind → Verifica fugas de memoria con valgrind
#   make check    → Prueba de compresión/descompresión rápida
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
# -Wall -Wextra: advertencias exhaustivas para evitar bugs comunes
# -O2: optimización que el compilador aplica (inline, dead code elim, etc.)
# -std=c11: permite _Static_assert, tipos stdint, etc.
# -D_GNU_SOURCE: habilita extensiones POSIX/GNU en Linux (mmap, posix_memalign)
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE

SRCDIR  = src
TOOLDIR = tools
OBJDIR  = obj
BINDIR  = bin

# Fuentes del editor principal
SRCS = $(SRCDIR)/main.c \
       $(SRCDIR)/editor.c \
       $(SRCDIR)/compress.c \
       $(SRCDIR)/fileformat.c \
       $(SRCDIR)/io.c

OBJS = $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

TARGET     = $(BINDIR)/editor
GEN_TARGET = $(BINDIR)/gen_test

.PHONY: all clean valgrind check dirs

all: dirs $(TARGET) $(GEN_TARGET)
	@echo ""
	@echo "✓ Compilación exitosa"
	@echo "  Editor:     $(TARGET)"
	@echo "  Gen test:   $(GEN_TARGET)"
	@echo ""
	@echo "Uso rápido:"
	@echo "  ./$(GEN_TARGET) 5 test_5mb.txt         # genera 5 MB de texto"
	@echo "  ./$(TARGET) test_5mb.txt                # importar y editar"
	@echo "  ./benchmark/run_benchmark.sh test_5mb.txt"

dirs:
	@mkdir -p $(OBJDIR) $(BINDIR)

# Compilar cada .c en su .o correspondiente
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Enlazar el editor
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compilar la herramienta de generación de archivos de prueba
$(GEN_TARGET): $(TOOLDIR)/gen_test_file.c
	$(CC) $(CFLAGS) -o $@ $<

# ─── Herramientas de verificación ──────────────────────────────────────────

# Verificar fugas de memoria con valgrind
# Salida esperada: "All heap blocks were freed -- no leaks are possible"
valgrind: $(TARGET)
	@echo "Ejecutando valgrind (puede tardar ~30s)..."
	echo ":append\nHola mundo de prueba\n:end\n:save /tmp/valgrind_test.ceds\n:open /tmp/valgrind_test.ceds\n:quit" \
	    | valgrind --leak-check=full --error-exitcode=1 $(TARGET)

# Prueba rápida de compresión/descompresión (smoke test)
check: $(TARGET)
	@echo "=== Smoke Test: comprimir → guardar → cargar → verificar ==="
	@echo "Hola, este es un test del editor CEDS.\nLínea 2.\nLínea 3.\nFin." \
	    > /tmp/ceds_smoke.txt
	@echo ":open /tmp/ceds_smoke.txt\n:save /tmp/ceds_smoke.ceds\n:quit" \
	    | $(TARGET) > /dev/null
	@echo ":open /tmp/ceds_smoke.ceds\n:show\n:quit" | $(TARGET)
	@echo "=== Test completado ==="

clean:
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "Limpieza completada."
