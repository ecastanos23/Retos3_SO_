#!/bin/bash
# benchmark/run_benchmark.sh
# ─────────────────────────────────────────────────────────────────────────────
# Mide empíricamente la diferencia de rendimiento entre enfoques de I/O.
# Usa strace para contar syscalls y time para separar CPU de I/O.
#
# Uso: ./benchmark/run_benchmark.sh [archivo_de_prueba.txt]
#
# Prerequisitos: strace, valgrind (opcionales)
# ─────────────────────────────────────────────────────────────────────────────

set -e

EDITOR="./bin/editor"
GENTEST="./bin/gen_test"
TESTFILE="${1:-}"
RESULTS_DIR="benchmark/results"

mkdir -p "$RESULTS_DIR"

# ── Verificar binarios ──────────────────────────────────────────────────────
if [ ! -f "$EDITOR" ]; then
    echo "ERROR: '$EDITOR' no encontrado. Ejecuta 'make' primero."
    exit 1
fi

# ── Generar archivo de prueba si no se especificó uno ──────────────────────
if [ -z "$TESTFILE" ] || [ ! -f "$TESTFILE" ]; then
    TESTFILE="$RESULTS_DIR/test_5mb.txt"
    echo "Generando archivo de prueba de 5 MB..."
    if [ -f "$GENTEST" ]; then
        "$GENTEST" 5 "$TESTFILE"
    else
        # Fallback: generar con dd + tr si gen_test no está disponible
        dd if=/dev/urandom bs=1M count=5 2>/dev/null | tr -dc 'a-zA-Z \n' | \
            head -c 5242880 > "$TESTFILE"
    fi
fi

FILE_SIZE=$(stat -c%s "$TESTFILE" 2>/dev/null || stat -f%z "$TESTFILE")

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo " CEDS Editor — Benchmark de I/O con strace y time"
echo " Archivo: $TESTFILE"
echo " Tamaño:  $FILE_SIZE bytes ($(echo "scale=2; $FILE_SIZE/1048576" | bc) MB)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# ── EXPERIMENTO 1: Texto plano naïve (bloques de 64 bytes) ─────────────────
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ EXPERIMENTO 1: Texto plano, write() de 64B (naïve)         │"
echo "└─────────────────────────────────────────────────────────────┘"
echo "Corriendo strace..."

strace -c -e trace=write,open,close \
    bash -c "echo ':open $TESTFILE\n:plain /tmp/bench_naive_out.txt\n:quit' | $EDITOR" \
    2>"$RESULTS_DIR/strace_naive.txt" || true

echo "──── Resultado strace (naive) ────"
cat "$RESULTS_DIR/strace_naive.txt"
echo ""

# ── EXPERIMENTO 2: Compresión RLE + write() 4KB ────────────────────────────
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ EXPERIMENTO 2: Compresión RLE + write() alineado 4KB       │"
echo "└─────────────────────────────────────────────────────────────┘"
echo "Corriendo strace..."

strace -c -e trace=write,open,close \
    bash -c "echo ':open $TESTFILE\n:save /tmp/bench_fd_out.ceds\n:quit' | $EDITOR" \
    2>"$RESULTS_DIR/strace_fd.txt" || true

echo "──── Resultado strace (RLE + write) ────"
cat "$RESULTS_DIR/strace_fd.txt"
echo ""

# ── EXPERIMENTO 3: Compresión RLE + mmap ───────────────────────────────────
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ EXPERIMENTO 3: Compresión RLE + mmap()                     │"
echo "└─────────────────────────────────────────────────────────────┘"
echo "Corriendo strace..."

strace -c -e trace=mmap,munmap,msync,open,close \
    bash -c "echo ':open $TESTFILE\n:savem /tmp/bench_mmap_out.ceds\n:quit' | $EDITOR" \
    2>"$RESULTS_DIR/strace_mmap.txt" || true

echo "──── Resultado strace (RLE + mmap) ────"
cat "$RESULTS_DIR/strace_mmap.txt"
echo ""

# ── EXPERIMENTO 4: Wall-clock con time (User/Sys/Real) ─────────────────────
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ EXPERIMENTO 4: time — separar CPU (user/sys) de I/O (real) │"
echo "└─────────────────────────────────────────────────────────────┘"

echo -n "[Naive]        "; \
{ time ( printf ':open %s\n:plain /tmp/t_naive.txt\n:quit\n' "$TESTFILE" \
    | "$EDITOR" > /dev/null 2>&1 ); } 2>&1

echo -n "[RLE + write]  "; \
{ time ( printf ':open %s\n:save /tmp/t_fd.ceds\n:quit\n' "$TESTFILE" \
    | "$EDITOR" > /dev/null 2>&1 ); } 2>&1

echo -n "[RLE + mmap]   "; \
{ time ( printf ':open %s\n:savem /tmp/t_mmap.ceds\n:quit\n' "$TESTFILE" \
    | "$EDITOR" > /dev/null 2>&1 ); } 2>&1

echo ""

# ── Resumen de tamaños en disco ────────────────────────────────────────────
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ RESUMEN: Volumen de datos escritos al disco                 │"
echo "└─────────────────────────────────────────────────────────────┘"
SZ_NAIVE=$(stat -c%s /tmp/t_naive.txt  2>/dev/null || echo 0)
SZ_FD=$(stat -c%s    /tmp/t_fd.ceds    2>/dev/null || echo 0)
SZ_MMAP=$(stat -c%s  /tmp/t_mmap.ceds  2>/dev/null || echo 0)

echo "  Naive (plano 64B):    $SZ_NAIVE bytes (100%)"
echo "  RLE + write (4KB):    $SZ_FD bytes ($(echo "scale=1; $SZ_FD*100/$SZ_NAIVE" | bc)%)"
echo "  RLE + mmap:           $SZ_MMAP bytes ($(echo "scale=1; $SZ_MMAP*100/$SZ_NAIVE" | bc)%)"
echo ""
echo "Resultados de strace guardados en: $RESULTS_DIR/"
echo ""
echo "═══ Comandos adicionales recomendados para el reporte ═══"
echo "  valgrind --leak-check=full ./bin/editor $TESTFILE"
echo "  perf stat ./bin/editor $TESTFILE"
echo "  strace -c ./bin/editor $TESTFILE   # conteo completo de syscalls"
