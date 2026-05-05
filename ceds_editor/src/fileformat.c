#include "fileformat.h"
#include "compress.h"
#include "io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * fileformat.c - Serialización/deserialización del formato binario .ceds.
 *
 * Pipeline de guardado (doc_save):
 *   texto (RAM) → rle_compress (User Space) → ensamblar payload → io_write_*
 *
 * Pipeline de carga (doc_load):
 *   io_read_fd → validar header → rle_decompress (User Space) → texto (RAM)
 *
 * NINGÚN byte de texto plano toca el bus I/O.
 */

uint32_t calc_checksum(const uint8_t *data, size_t size) {
    uint32_t csum = 0;
    /*
     * XOR rotativo: el bit-shift varía según la posición del byte,
     * lo que hace que la misma secuencia de bytes en diferente posición
     * produzca checksums distintos (más robusto que XOR plano).
     */
    for (size_t i = 0; i < size; i++) {
        csum ^= ((uint32_t)data[i]) << ((i % 4) * 8);
    }
    return csum;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int doc_save(const char *path, const char *text, size_t text_size,
             const char *title, const StyleEntry *styles, uint32_t style_count,
             int use_mmap) {

    /* ── Paso 1: COMPRIMIR en User Space ─────────────────────────────────
     * Todo el trabajo de CPU ocurre AQUÍ, antes de invocar ninguna syscall.
     * Reducir el tamaño ahora = menos datos que el bus I/O debe transferir.
     */
    size_t  worst     = RLE_WORST_CASE(text_size);
    uint8_t *comp_buf = malloc(worst);
    if (!comp_buf) { perror("doc_save: malloc comp_buf"); return -1; }

    ssize_t comp_size = rle_compress((const uint8_t *)text, text_size,
                                     comp_buf, worst);
    if (comp_size < 0) {
        fprintf(stderr, "doc_save: error en compresión RLE\n");
        free(comp_buf);
        return -1;
    }

    /* ── Paso 2: Construir el FileHeader (64 bytes, sin padding) ──────── */
    FileHeader hdr;
    memset(&hdr, 0, sizeof(FileHeader));

    memcpy(hdr.magic, CEDS_MAGIC, 4);
    hdr.version          = CEDS_VERSION;
    hdr.compression_type = COMPRESSION_RLE;
    hdr.flags            = (style_count > 0) ? FLAG_RICH_TEXT : 0;
    hdr.original_size    = (uint32_t)text_size;
    hdr.compressed_size  = (uint32_t)comp_size;
    hdr.checksum         = calc_checksum(comp_buf, (size_t)comp_size);
    hdr.style_count      = style_count;

    if (title) {
        strncpy(hdr.title, title, sizeof(hdr.title) - 1);
    }

    /* ── Paso 3: Ensamblar el payload completo en RAM ─────────────────── */
    size_t  style_sz    = style_count * sizeof(StyleEntry);
    size_t  total_size  = sizeof(FileHeader) + style_sz + (size_t)comp_size;
    uint8_t *payload    = malloc(total_size);
    if (!payload) {
        perror("doc_save: malloc payload");
        free(comp_buf);
        return -1;
    }

    /* Layout: [FileHeader][StyleEntry × N][payload comprimido] */
    uint8_t *ptr = payload;
    memcpy(ptr, &hdr, sizeof(FileHeader));           ptr += sizeof(FileHeader);
    if (style_count > 0 && styles) {
        memcpy(ptr, styles, style_sz);               ptr += style_sz;
    }
    memcpy(ptr, comp_buf, (size_t)comp_size);
    free(comp_buf);

    /* ── Paso 4: Una sola escritura al disco (todo en User Space ya listo) */
    int result = use_mmap
                 ? io_write_mmap(path, payload, total_size)
                 : io_write_fd(path,   payload, total_size);

    free(payload);
    return result;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int doc_load(const char *path, LoadedDocument *out) {
    /* Leer el archivo completo en RAM de una vez */
    size_t   file_size;
    uint8_t *file_data = io_read_fd(path, &file_size);
    if (!file_data) return -1;

    /* Validar tamaño mínimo */
    if (file_size < sizeof(FileHeader)) {
        fprintf(stderr, "doc_load: archivo demasiado pequeño\n");
        free(file_data);
        return -1;
    }

    /* Validar magic number */
    FileHeader *hdr = (FileHeader *)file_data;
    if (memcmp(hdr->magic, CEDS_MAGIC, 4) != 0) {
        fprintf(stderr, "doc_load: magic number inválido (no es .ceds)\n");
        free(file_data);
        return -1;
    }

    if (hdr->version != CEDS_VERSION) {
        fprintf(stderr, "doc_load: versión de formato no soportada\n");
        free(file_data);
        return -1;
    }

    if (hdr->compression_type != COMPRESSION_RLE) {
        fprintf(stderr, "doc_load: tipo de compresión no soportado\n");
        free(file_data);
        return -1;
    }

    /* Validar checksum del payload comprimido */
    if (hdr->style_count > 64) {
        fprintf(stderr, "doc_load: demasiados estilos para este editor\n");
        free(file_data);
        return -1;
    }

    size_t  style_sz  = hdr->style_count * sizeof(StyleEntry);
    size_t  total_sz  = sizeof(FileHeader) + style_sz + hdr->compressed_size;

    if (hdr->style_count > 0 && !(hdr->flags & FLAG_RICH_TEXT)) {
        fprintf(stderr, "doc_load: estilos presentes pero FLAG_RICH_TEXT no está activo\n");
        free(file_data);
        return -1;
    }

    if (total_sz != file_size || total_sz < sizeof(FileHeader)) {
        fprintf(stderr, "doc_load: tamaños inconsistentes en el archivo\n");
        free(file_data);
        return -1;
    }

    uint8_t *comp_ptr = file_data + sizeof(FileHeader) + style_sz;
    uint32_t actual_ck = calc_checksum(comp_ptr, hdr->compressed_size);
    if (actual_ck != hdr->checksum) {
        fprintf(stderr, "doc_load: checksum inválido — archivo corrupto\n");
        free(file_data);
        return -1;
    }

    /* Cargar tabla de estilos (si existe) */
    out->styles = NULL;
    if (hdr->style_count > 0) {
        out->styles = malloc(style_sz);
        if (out->styles) {
            memcpy(out->styles, file_data + sizeof(FileHeader), style_sz);
        }
    }

    /* Descomprimir el texto en User Space */
    out->text = malloc(hdr->original_size + 1);   /* +1 para null terminator */
    if (!out->text) {
        perror("doc_load: malloc text");
        free(out->styles);
        free(file_data);
        return -1;
    }

    ssize_t dec_size = rle_decompress(comp_ptr, hdr->compressed_size,
                                      (uint8_t *)out->text, hdr->original_size);

    if (dec_size < 0 || (uint32_t)dec_size != hdr->original_size) {
        fprintf(stderr, "doc_load: fallo en descompresión "
                        "(esperado %u bytes, obtenido %zd)\n",
                hdr->original_size, dec_size);
        free(out->text);
        free(out->styles);
        free(file_data);
        return -1;
    }

    out->text[dec_size] = '\0';
    out->text_size = (size_t)dec_size;
    out->header    = *hdr;          /* copia el header al resultado */

    free(file_data);
    return 0;
}

void doc_free(LoadedDocument *doc) {
    if (!doc) return;
    free(doc->text);
    free(doc->styles);
    doc->text      = NULL;
    doc->styles    = NULL;
    doc->text_size = 0;
}
