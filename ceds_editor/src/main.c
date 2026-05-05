/*
 * main.c - Editor de texto CEDS con optimización de Bus I/O
 *
 * Interfaz: editor interactivo por línea de comandos.
 * Estructura interna: Gap Buffer para edición + RLE + POSIX I/O.
 *
 * Uso:
 *   ./bin/editor                   → Editor en blanco
 *   ./bin/editor archivo.txt       → Importar texto plano
 *   ./bin/editor archivo.ceds      → Abrir documento comprimido
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "editor.h"
#include "fileformat.h"
#include "io.h"
#include "profiling.h"

#define VERSION   "1.0"
#define MAX_LINE  4096

/* ─── Estado global del editor ──────────────────────────────────────────── */
static GapBuffer *g_buf       = NULL;          /* Gap Buffer principal */
static char       g_title[32] = "Sin título";  /* Título del documento */
static char       g_path[256] = "";            /* Ruta del archivo actual */
static int        g_modified  = 0;             /* 1 si hay cambios sin guardar */

/* Estilos de texto enriquecido (opcional, para nivel Excelente) */
static StyleEntry g_styles[64];
static uint32_t   g_style_count = 0;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

/* Obtiene el tamaño de un archivo en bytes */
static size_t file_size_of(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) ? (size_t)st.st_size : 0;
}

/* Extrae el nombre de archivo de una ruta */
static const char *basename_of(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* ─── Comandos del editor ────────────────────────────────────────────────── */

static void cmd_new(const char *title) {
    if (g_buf) gb_free(g_buf);
    g_buf = gb_new();
    if (!g_buf) { fprintf(stderr, "ERROR: no se pudo inicializar el buffer\n"); return; }

    strncpy(g_title, title && title[0] ? title : "Sin título", sizeof(g_title) - 1);
    g_title[sizeof(g_title) - 1] = '\0';
    g_path[0]     = '\0';
    g_modified    = 0;
    g_style_count = 0;
    memset(g_styles, 0, sizeof(g_styles));
    printf("Nuevo documento creado: '%s'\n", g_title);
}

/* ─── */
static void cmd_open(const char *path) {
    if (!path || !path[0]) { printf("Uso: :open <archivo>\n"); return; }

    size_t plen = strlen(path);

    if (plen > 5 && strcmp(path + plen - 5, ".ceds") == 0) {
        /* ── Cargar formato .ceds ─────────────────── */
        LoadedDocument doc;
        PROF_START(load);
        if (doc_load(path, &doc) < 0) { printf("Error al abrir '%s'\n", path); return; }
        PROF_END(load);

        if (g_buf) gb_free(g_buf);
        g_buf = gb_new();
        gb_insert_str(g_buf, doc.text, doc.text_size);

        strncpy(g_title, doc.header.title, sizeof(g_title) - 1);
        g_title[sizeof(g_title) - 1] = '\0';
        strncpy(g_path,  path,             sizeof(g_path)  - 1);
        g_path[sizeof(g_path) - 1] = '\0';
        g_modified    = 0;
        g_style_count = 0;
        memset(g_styles, 0, sizeof(g_styles));
        if (doc.styles && doc.header.style_count > 0) {
            g_style_count = doc.header.style_count;
            if (g_style_count > 64) {
                g_style_count = 64;
            }
            memcpy(g_styles, doc.styles, g_style_count * sizeof(StyleEntry));
        }

        printf("Abierto: '%s'\n", path);
        printf("  Título         : %s\n", g_title);
        printf("  Tamaño texto   : %u bytes\n", doc.header.original_size);
        printf("  Tamaño en disco: %u bytes comprimidos (%.1f%% del original)\n",
               doc.header.compressed_size,
               doc.header.compressed_size * 100.0 / doc.header.original_size);
        PROF_PRINT(load, "  Tiempo de carga");
        doc_free(&doc);

    } else {
        /* ── Importar texto plano (.txt u otro) ───── */
        size_t   fsize;
        uint8_t *data = io_read_fd(path, &fsize);
        if (!data) { printf("Error al leer '%s'\n", path); return; }

        if (g_buf) gb_free(g_buf);
        g_buf = gb_new();
        gb_insert_str(g_buf, (char *)data, fsize);
        free(data);

        strncpy(g_title, basename_of(path), sizeof(g_title) - 1);
        g_title[sizeof(g_title) - 1] = '\0';
        strncpy(g_path,  path,              sizeof(g_path)  - 1);
        g_path[sizeof(g_path) - 1] = '\0';
        g_modified = 0;
        printf("Importado: '%s' (%zu bytes de texto plano)\n", path, fsize);
    }
}

/* ─── */
static void cmd_show(void) {
    if (!g_buf || gb_text_size(g_buf) == 0) {
        printf("[Documento vacío — usa :append para escribir]\n");
        return;
    }
    char *text = gb_get_text(g_buf);
    if (!text) return;
    printf("─── %s ───\n%s\n────\n", g_title, text);
    free(text);
}

/* ─── */
static void cmd_lines(void) {
    if (!g_buf) return;
    char *text = gb_get_text(g_buf);
    if (!text) return;

    printf("─── %s (con números de línea) ───\n", g_title);
    int line = 1;
    printf("%4d │ ", line);
    for (size_t i = 0; text[i]; i++) {
        putchar(text[i]);
        if (text[i] == '\n' && text[i + 1]) printf("%4d │ ", ++line);
    }
    printf("────\nTotal: %d línea(s), %zu bytes\n", line, gb_text_size(g_buf));
    free(text);
}

/* ─── */
static void cmd_append(void) {
    if (!g_buf) { printf("Primero crea un documento con :new\n"); return; }

    printf("Modo APPEND — Escribe tu texto. Línea ':end' para terminar.\n");
    gb_move_to(g_buf, gb_text_size(g_buf));   /* cursor al final */

    char line[MAX_LINE];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        if (strncmp(line, ":end", 4) == 0)    break;
        gb_insert_str(g_buf, line, strlen(line));
        g_modified = 1;
    }
    printf("Texto agregado. Tamaño total: %zu bytes\n", gb_text_size(g_buf));
}

/* ─── */
static void cmd_delete_line(int line_num) {
    if (!g_buf || line_num < 1) return;
    char *text = gb_get_text(g_buf);
    if (!text) return;

    int    cur_line = 1;
    size_t start    = 0;

    for (size_t i = 0; text[i]; i++) {
        if (cur_line == line_num) {
            /* Encontrar fin de la línea */
            size_t end = i;
            while (text[end] && text[end] != '\n') end++;
            if (text[end] == '\n') end++;

            /* Reconstruir el texto sin esa línea */
            size_t old_size = gb_text_size(g_buf);
            size_t new_size = old_size - (end - start);
            char  *new_text = malloc(new_size + 1);
            if (!new_text) { free(text); return; }

            memcpy(new_text,         text,        start);
            memcpy(new_text + start, text + end,  old_size - end);
            new_text[new_size] = '\0';

            gb_free(g_buf);
            g_buf = gb_new();
            gb_insert_str(g_buf, new_text, new_size);
            free(new_text);
            g_modified = 1;
            printf("Línea %d eliminada.\n", line_num);
            break;
        }
        if (text[i] == '\n') {
            cur_line++;
            start = i + 1;
        }
    }
    free(text);
}

/* ─── */
static void cmd_save(const char *path, int use_mmap) {
    if (!g_buf) { printf("No hay documento abierto\n"); return; }

    const char *dest = (path && path[0]) ? path : g_path;
    if (!dest || !dest[0]) { printf("Especifica un nombre: :save <archivo.ceds>\n"); return; }

    char *text = gb_get_text(g_buf);
    if (!text) return;
    size_t text_size = strlen(text);

    PROF_START(save);
    int ret = doc_save(dest, text, text_size, g_title,
                       g_style_count ? g_styles : NULL, g_style_count, use_mmap);
    PROF_END(save);
    free(text);

    if (ret == 0) {
        strncpy(g_path, dest, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = '\0';
        g_modified = 0;
        size_t on_disk = file_size_of(dest);
        printf("Guardado: '%s' [%s]\n", dest, use_mmap ? "mmap" : "write(4KB)");
        printf("  Texto original : %zu bytes\n", text_size);
        printf("  Archivo en disco: %zu bytes (%.1f%% del original)\n",
               on_disk, on_disk * 100.0 / text_size);
        PROF_PRINT(save, "  Tiempo de guardado");
    } else {
        printf("Error al guardar '%s'\n", dest);
    }
}

/* ─── */
static void cmd_add_style(uint32_t offset, uint32_t length,
                          int bold, int italic, int underline, uint32_t color) {
    if (g_style_count >= 64) { printf("Límite de estilos alcanzado (64)\n"); return; }
    StyleEntry *s = &g_styles[g_style_count++];
    s->offset    = offset;
    s->length    = length;
    s->bold      = (uint8_t)bold;
    s->italic    = (uint8_t)italic;
    s->underline = (uint8_t)underline;
    s->reserved  = 0;
    s->color     = color;
    printf("Estilo agregado #%u: offset=%u len=%u B=%d I=%d U=%d\n",
           g_style_count, offset, length, bold, italic, underline);
}

/* ─── */
static void cmd_benchmark(void) {
    if (!g_buf || gb_text_size(g_buf) == 0) {
        printf("El documento está vacío. Usa :append para agregar contenido.\n");
        return;
    }

    char  *text = gb_get_text(g_buf);
    size_t size = strlen(text);

    printf("\n══════════════════════════════════════════════════════\n");
    printf(" BENCHMARK I/O  —  Texto: %zu bytes (%.2f KB)\n", size, size / 1024.0);
    printf("══════════════════════════════════════════════════════\n\n");

    /* ── Enfoque 1: Texto plano, escritura naïve (bloques de 64 B) ─── */
    PROF_START(naive);
    io_write_plain_naive("/tmp/ceds_bench_naive.txt", text, size);
    PROF_END(naive);

    /* ── Enfoque 2: RLE + write() en bloques de 4KB ─────────────────── */
    PROF_START(fd);
    doc_save("/tmp/ceds_bench_fd.ceds", text, size, "bench", NULL, 0, 0);
    PROF_END(fd);

    /* ── Enfoque 3: RLE + mmap ───────────────────────────────────────── */
    PROF_START(mm);
    doc_save("/tmp/ceds_bench_mmap.ceds", text, size, "bench", NULL, 0, 1);
    PROF_END(mm);

    size_t sz_naive = file_size_of("/tmp/ceds_bench_naive.txt");
    size_t sz_fd    = file_size_of("/tmp/ceds_bench_fd.ceds");
    size_t sz_mmap  = file_size_of("/tmp/ceds_bench_mmap.ceds");

    printf("┌─────────────────────────────┬──────────────┬────────────┬───────────┐\n");
    printf("│ Métrica                     │ Naive (plano)│ RLE+write  │ RLE+mmap  │\n");
    printf("├─────────────────────────────┼──────────────┼────────────┼───────────┤\n");
    printf("│ Datos escritos al disco     │ %8zu B   │ %8zu B │ %7zu B │\n",
           sz_naive, sz_fd, sz_mmap);
    printf("│ Ratio vs naive              │    100.0%%    │  %6.1f%%  │  %6.1f%%  │\n",
           sz_fd   * 100.0 / sz_naive,
           sz_mmap * 100.0 / sz_naive);
    printf("│ write() calls estimadas     │ %8zu     │ %8zu │ %7s │\n",
           sz_naive / 64,            /* naive: bloques de 64B */
           sz_fd    / 4096 + 1,      /* fd: bloques de 4KB   */
           "~1");                    /* mmap: 1 msync        */
    printf("│ Tiempo wall-clock           │ %9.3f ms │ %8.3f ms│ %7.3f ms│\n",
           PROF_MS(naive), PROF_MS(fd), PROF_MS(mm));
    printf("└─────────────────────────────┴──────────────┴────────────┴───────────┘\n\n");

    printf("Para contar syscalls exactas con strace:\n");
    printf("  strace -c -e trace=write,read,mmap,msync ./bin/editor <archivo>\n\n");
    free(text);
}

/* ─── */
static void cmd_info(void) {
    printf("Documento : %s%s\n", g_title, g_modified ? " [modificado]" : "");
    printf("Archivo   : %s\n",   g_path[0] ? g_path : "(sin guardar)");
    printf("Texto     : %zu bytes, ", g_buf ? gb_text_size(g_buf) : 0);
    if (g_buf) {
        char *t = gb_get_text(g_buf);
        int lines = 0;
        for (size_t i = 0; t && t[i]; i++) if (t[i] == '\n') lines++;
        printf("%d línea(s)\n", lines + 1);
        free(t);
    } else printf("0 líneas\n");
    printf("Estilos   : %u entradas\n", g_style_count);
    printf("FileHeader: %zu bytes | StyleEntry: %zu bytes\n",
           sizeof(FileHeader), sizeof(StyleEntry));
}

/* ─── */
static void print_help(void) {
    printf("\nComandos disponibles:\n");
    printf("  :new [título]            Nuevo documento\n");
    printf("  :open <archivo>          Abrir .ceds o importar .txt\n");
    printf("  :save [archivo.ceds]     Guardar comprimido (write 4KB)\n");
    printf("  :savem [archivo.ceds]    Guardar comprimido (mmap)\n");
    printf("  :plain <archivo.txt>     Guardar texto plano (sin comprimir)\n");
    printf("  :show                    Mostrar contenido\n");
    printf("  :lines                   Mostrar con números de línea\n");
    printf("  :append                  Modo escritura (termina con :end)\n");
    printf("  :delete <N>              Eliminar línea N\n");
    printf("  :style <off> <len> <b> <i> <u> <color>  Agregar estilo\n");
    printf("  :benchmark               Comparar enfoques de I/O\n");
    printf("  :info                    Información del documento\n");
    printf("  :help                    Esta ayuda\n");
    printf("  :quit / :q               Salir\n\n");
    printf("También puedes escribir texto directamente (se agrega al doc).\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    printf("CEDS Editor v%s — Editor con Optimización de Bus I/O (Linux/C)\n", VERSION);
    printf("Escribe ':help' para ver los comandos.\n\n");

    cmd_new("Sin título");

    if (argc >= 2) {
        cmd_open(argv[1]);
    }

    char  line[MAX_LINE];
    char *arg;

    while (1) {
        printf("%s%s> ", g_title, g_modified ? "*" : "");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';     /* quitar newline */

        if (line[0] == '\0') continue;

        if (line[0] != ':') {
            /* Texto sin comando → insertar al final del documento */
            if (g_buf) {
                gb_move_to(g_buf, gb_text_size(g_buf));
                gb_insert_str(g_buf, line, strlen(line));
                gb_insert(g_buf, '\n');
                g_modified = 1;
            }
            continue;
        }

        /* Separar el comando del argumento */
        arg = strchr(line, ' ');
        if (arg) { *arg = '\0'; arg++; }

        /* Despachar comandos */
        if      (!strcmp(line, ":new"))       cmd_new(arg);
        else if (!strcmp(line, ":open"))      cmd_open(arg);
        else if (!strcmp(line, ":save"))      cmd_save(arg, 0);
        else if (!strcmp(line, ":savem"))     cmd_save(arg, 1);
        else if (!strcmp(line, ":plain")) {
            if (g_buf && arg) {
                char *t = gb_get_text(g_buf);
                if (t) {
                    io_write_plain_naive(arg, t, strlen(t));
                    printf("Guardado plano: '%s' (%zu bytes)\n", arg, strlen(t));
                    free(t);
                }
            }
        }
        else if (!strcmp(line, ":show"))      cmd_show();
        else if (!strcmp(line, ":lines"))     cmd_lines();
        else if (!strcmp(line, ":append"))    cmd_append();
        else if (!strcmp(line, ":delete"))    cmd_delete_line(arg ? atoi(arg) : 0);
        else if (!strcmp(line, ":style")) {
            /* :style <offset> <length> <bold> <italic> <underline> <color_hex> */
            uint32_t off = 0, len = 0, col = 0xFFFFFFFF;
            int b = 0, i = 0, u = 0;
            if (arg) sscanf(arg, "%u %u %d %d %d %x", &off, &len, &b, &i, &u, &col);
            cmd_add_style(off, len, b, i, u, col);
        }
        else if (!strcmp(line, ":benchmark")) cmd_benchmark();
        else if (!strcmp(line, ":info"))      cmd_info();
        else if (!strcmp(line, ":help"))      print_help();
        else if (!strcmp(line, ":quit") || !strcmp(line, ":q")) {
            if (g_modified) printf("Advertencia: hay cambios sin guardar.\n");
            break;
        }
        else printf("Comando desconocido: '%s'. Usa :help\n", line);
    }

    if (g_buf) gb_free(g_buf);
    printf("Saliendo. ¡Hasta luego!\n");
    return 0;
}
