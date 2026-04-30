/*
 * gen_test_file.c - Generador de archivos de texto para benchmarks.
 *
 * Genera texto con vocabulario de Sistemas Operativos para simular
 * un documento real. El texto tiene patrones repetidos que el RLE
 * puede comprimir eficientemente.
 *
 * Uso: ./bin/gen_test <MB> <archivo_salida>
 * Ej:  ./bin/gen_test 5 test_5mb.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Vocabulario de Sistemas Operativos */
static const char *WORDS[] = {
    "sistema", "operativo", "kernel", "proceso", "memoria", "buffer",
    "archivo", "disco", "CPU", "bus", "interrupción", "scheduler",
    "pipeline", "compresión", "descriptor", "página", "virtual", "físico",
    "Linux", "POSIX", "syscall", "mmap", "write", "read", "open", "close",
    "malloc", "free", "puntero", "estructura", "eficiencia", "rendimiento",
    "optimización", "latencia", "throughput", "benchmark", "profiling",
    "context", "switch", "DMA", "cache", "heap", "stack", "segfault",
    "valgrind", "strace", "alineación", "bloque", "sector", "inodo",
    "compresión", "descompresión", "algoritmo", "datos", "archivo", "binario"
};
#define WORD_COUNT (int)(sizeof(WORDS) / sizeof(WORDS[0]))

/* Frases de relleno (generan repetición, ideal para RLE) */
static const char *PHRASES[] = {
    "el sistema operativo gestiona los recursos del hardware",
    "la memoria virtual permite aislar los procesos entre sí",
    "el bus de datos conecta la CPU con los dispositivos",
    "las llamadas al sistema son la interfaz entre User y Kernel Space",
    "el scheduler decide qué proceso se ejecuta en la CPU",
    "la compresión reduce el volumen de datos en el bus de I/O"
};
#define PHRASE_COUNT (int)(sizeof(PHRASES) / sizeof(PHRASES[0]))

int main(int argc, char *argv[]) {
    size_t target_mb = 5;
    const char *outfile = "test_document.txt";

    if (argc >= 2) target_mb = (size_t)atol(argv[1]);
    if (argc >= 3) outfile   = argv[2];

    size_t target_bytes = target_mb * 1024 * 1024;

    FILE *f = fopen(outfile, "w");
    if (!f) { perror("fopen"); return 1; }

    srand((unsigned)time(NULL));
    size_t written   = 0;

    printf("Generando %zu MB en '%s'...\n", target_mb, outfile);

    while (written < target_bytes) {
        /* Cada 10 líneas, insertar una frase completa (genera patrones RLE) */
        if (rand() % 10 == 0) {
            const char *phrase = PHRASES[rand() % PHRASE_COUNT];
            size_t plen = strlen(phrase);
            fwrite(phrase, 1, plen, f);
            fputc('\n', f);
            written   += plen + 1;
            continue;
        }

        /* Línea normal: 6-14 palabras aleatorias */
        int line_words = 6 + rand() % 9;
        for (int i = 0; i < line_words && written < target_bytes; i++) {
            const char *w = WORDS[rand() % WORD_COUNT];
            size_t wlen   = strlen(w);
            fwrite(w, 1, wlen, f);
            written += wlen;

            if (i < line_words - 1) { fputc(' ', f); written++; }
        }
        fputc('\n', f);
        written++;
    }

    fclose(f);

    /* Reportar estadísticas */
    printf("Generado: %zu bytes (%.2f MB)\n", written, written / 1024.0 / 1024.0);
    printf("Usa con: ./bin/editor %s\n", outfile);
    return 0;
}
