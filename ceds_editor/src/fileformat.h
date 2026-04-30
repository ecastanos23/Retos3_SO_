#ifndef FILEFORMAT_H
#define FILEFORMAT_H

/*
 * fileformat.h - Formato binario del archivo .ceds
 *
 * Estructura del archivo en disco:
 *
 *   ┌──────────────────────────────┐
 *   │  FileHeader  (64 bytes)      │  ← Magic, versión, tamaños, checksum
 *   ├──────────────────────────────┤
 *   │  StyleEntry × style_count    │  ← Tabla de estilos (texto enriquecido)
 *   │  (16 bytes × N)              │    Solo si flags & FLAG_RICH_TEXT
 *   ├──────────────────────────────┤
 *   │  Payload comprimido (RLE)    │  ← El texto comprimido en User Space
 *   │  (compressed_size bytes)     │    NUNCA viaja texto plano al disco
 *   └──────────────────────────────┘
 *
 * Uso de __attribute__((packed)):
 *   Por defecto, el compilador agrega bytes de padding entre campos de un
 *   struct para alinear cada campo a su tamaño natural (ej: uint32_t a 4 bytes).
 *   Esto desperdicia bytes en el archivo y dificulta la lectura portable.
 *   Con __attribute__((packed)), el compilador empaqueta los campos sin padding,
 *   garantizando que sizeof(FileHeader) == 64 exactamente.
 *
 *   Costo: accesos no alineados en CPU. Aceptable para headers de archivo
 *   que se leen/escriben una sola vez (no en loops críticos de rendimiento).
 */

#include <stdint.h>
#include <stddef.h>

/* Identificador del formato (primeros 4 bytes del archivo) */
#define CEDS_MAGIC    "CEDS"
#define CEDS_VERSION  1

/* Tipos de compresión */
#define COMPRESSION_NONE  0
#define COMPRESSION_RLE   1

/* Flags del documento */
#define FLAG_RICH_TEXT  (1 << 0)   /* El documento tiene estilos */

/* ─── Header del archivo: exactamente 64 bytes ───────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];          /* "CEDS"                        (4 bytes) */
    uint8_t  version;           /* Versión del formato = 1       (1 byte)  */
    uint8_t  compression_type;  /* COMPRESSION_*                 (1 byte)  */
    uint16_t flags;             /* FLAG_*                        (2 bytes) */
    uint32_t original_size;     /* Tamaño del texto sin comprimir(4 bytes) */
    uint32_t compressed_size;   /* Tamaño del payload comprimido (4 bytes) */
    uint32_t checksum;          /* XOR checksum del payload      (4 bytes) */
    uint32_t style_count;       /* Cantidad de StyleEntry        (4 bytes) */
    char     title[32];         /* Título del documento          (32 bytes)*/
    uint8_t  reserved[8];       /* Reservado (expansión futura)  (8 bytes) */
} FileHeader;
/* Total: 4+1+1+2+4+4+4+4+32+8 = 64 bytes */

/* Verificación en tiempo de compilación: si falla, el struct tiene padding */
_Static_assert(sizeof(FileHeader) == 64,
               "FileHeader debe ser exactamente 64 bytes (revisar padding)");

/* ─── Entrada de estilo para texto enriquecido: exactamente 16 bytes ──────── */
typedef struct __attribute__((packed)) {
    uint32_t offset;    /* Byte offset en el texto original  (4 bytes) */
    uint32_t length;    /* Longitud del segmento en bytes    (4 bytes) */
    uint8_t  bold;      /* 1 = negrita                       (1 byte)  */
    uint8_t  italic;    /* 1 = cursiva                       (1 byte)  */
    uint8_t  underline; /* 1 = subrayado                     (1 byte)  */
    uint8_t  reserved;  /* Alineación                        (1 byte)  */
    uint32_t color;     /* Color RGBA (0xRRGGBBAA)           (4 bytes) */
} StyleEntry;
/* Total: 4+4+1+1+1+1+4 = 16 bytes */

_Static_assert(sizeof(StyleEntry) == 16,
               "StyleEntry debe ser exactamente 16 bytes (revisar padding)");

/* ─── Resultado de carga de documento ───────────────────────────────────── */
typedef struct {
    char       *text;          /* Texto descomprimido (heap, liberar con free) */
    size_t      text_size;     /* Tamaño del texto en bytes */
    FileHeader  header;        /* Copia del header leído del archivo */
    StyleEntry *styles;        /* Tabla de estilos (heap, NULL si sin estilos) */
} LoadedDocument;

/*
 * doc_save - Guarda el documento en formato .ceds comprimido.
 *
 * La compresión ocurre COMPLETAMENTE en User Space antes de cualquier
 * syscall de escritura, garantizando que ningún byte de texto claro
 * viaja al bus I/O.
 *
 * use_mmap: 0 = io_write_fd (write bloques 4KB), 1 = io_write_mmap
 * Retorna: 0 en éxito, -1 en error.
 */
int doc_save(const char *path, const char *text, size_t text_size,
             const char *title, const StyleEntry *styles, uint32_t style_count,
             int use_mmap);

/*
 * doc_load - Carga un documento .ceds desde disco.
 * Llama a doc_free() cuando termines de usarlo.
 * Retorna: 0 en éxito, -1 en error.
 */
int doc_load(const char *path, LoadedDocument *out);

/* doc_free - Libera todos los recursos de un LoadedDocument. */
void doc_free(LoadedDocument *doc);

/* calc_checksum - XOR checksum rotativo del buffer dado. */
uint32_t calc_checksum(const uint8_t *data, size_t size);

#endif /* FILEFORMAT_H */
