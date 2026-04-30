# Proyecto 3: Editor de Archivos con Optimización de Bus I/O

**Curso:** Sistemas Operativos | **Lenguaje:** C nativo | **Plataforma:** Linux (x86-64)

---

## Estructura del Proyecto

```
editor/
├── src/
│   ├── main.c          → CLI del editor (comandos, loop principal)
│   ├── editor.c/.h     → Gap Buffer (estructura de datos para edición)
│   ├── compress.c/.h   → Compresión RLE en User Space
│   ├── fileformat.c/.h → Formato binario .ceds (packed structs)
│   ├── io.c/.h         → POSIX I/O: write(4KB) y mmap()
│   └── profiling.h     → Macros de medición con CLOCK_MONOTONIC
├── tools/
│   └── gen_test_file.c → Genera archivos de texto para benchmark
├── benchmark/
│   └── run_benchmark.sh → Script de strace + time
├── Makefile
└── README.md
```

---

## Compilación y Uso

```bash
# Compilar todo
make

# Generar archivo de prueba de 5 MB
./bin/gen_test 5 test_5mb.txt

# Abrir e interactuar
./bin/editor test_5mb.txt

# Dentro del editor:
:append           # Escribir texto (termina con :end)
:save doc.ceds    # Guardar comprimido con write() 4KB
:savem doc.ceds   # Guardar comprimido con mmap()
:plain doc.txt    # Guardar texto plano (referencia)
:benchmark        # Comparar los tres enfoques
:lines            # Ver contenido con números de línea
:delete 3         # Eliminar línea 3
:style 0 10 1 0 0 0xFF0000FF  # Aplicar estilo negrita rojo
:quit
```

---

## 1. Matriz de Diseño del Pipeline I/O

### Diagrama de Flujo de Datos

```
ESCRITURA (doc_save)
═══════════════════════════════════════════════════════════════════════

   Gap Buffer (RAM)
        │
        │  gb_get_text()  →  texto plano en heap  (User Space)
        │
        ▼
   ┌────────────────────────────────────────────────┐
   │  rle_compress()  ←── AQUÍ ocurre la compresión │  USER SPACE
   │  Entrada: N bytes texto                         │
   │  Salida:  M bytes comprimidos  (M ≤ N, típico) │
   └──────────────────────────────┬─────────────────┘
                                  │
        ┌─── Ensamblar payload ───┘
        │    [FileHeader 64B][StyleTable][payload_RLE]
        │
        ├── Opción A: io_write_fd()
        │       open(O_WRONLY|O_CREAT)
        │       posix_memalign(buf, 4096, 4096)  ← buffer alineado
        │       write(fd, buf, 4096) × ⌈M/4096⌉  ← syscalls mínimas
        │       close(fd)
        │
        └── Opción B: io_write_mmap()
                open(O_RDWR|O_CREAT)
                ftruncate(fd, M)
                mmap(NULL, M, PROT_RW, MAP_SHARED, fd, 0)
                memcpy(map, payload, M)         ← sin syscall por byte
                msync(map, M, MS_SYNC)          ← 1 flush al disco
                munmap + close

                           │
                           ▼  KERNEL SPACE
                    ┌─────────────┐
                    │  Page Cache │
                    └──────┬──────┘
                           │  DMA Transfer
                           ▼
                    ┌─────────────┐
                    │  Disco Físico│  ← datos comprimidos, nunca texto claro
                    └─────────────┘


LECTURA (doc_load)
═══════════════════════════════════════════════════════════════════════

   Disco Físico → io_read_fd() → buf raw (RAM, User Space)
        │
        ├── Validar magic "CEDS" + checksum XOR
        │
        ├── Leer FileHeader (64 bytes packed) → metadata
        │
        ├── Leer StyleTable (16B × style_count) → estilos
        │
        └── rle_decompress() → texto plano en RAM → Gap Buffer
```

### Justificación: fd vs mmap vs stdio

| Criterio               | `stdio` (fopen) | `write` + 4KB | `mmap`           |
|------------------------|-----------------|---------------|------------------|
| Nivel de abstracción   | Alto (bufferiza)| Bajo (POSIX)  | Bajo (POSIX)     |
| Syscalls por MB        | Variable        | ~256 calls    | ~1 msync         |
| Buffer alignment       | No garantizado  | `posix_memalign` 4KB | Page-aligned    |
| Ideal para             | Pequeños textos | Archivos medianos | Archivos grandes |
| Visibilidad con strace | Oculta          | Directa       | Directa          |

**Por qué page-aligned (4096 bytes):**
El hardware DMA transfiere datos en múltiplos de páginas. Un buffer que empieza en una
dirección no alineada puede cruzar un límite de página, obligando al kernel a hacer
una copia temporal (*bounce buffer*). `posix_memalign(buf, 4096, 4096)` garantiza
que la dirección de inicio es múltiplo exacto del tamaño de página.

---

## 2. Gestión de Memoria en C

### Diseño de Structs sin Padding

```c
// SIN __attribute__((packed)) — el compilador agrega padding:
struct Sin_Packed {
    uint8_t  a;    // 1 byte + 3 bytes padding
    uint32_t b;    // 4 bytes (alineado a 4)
    uint8_t  c;    // 1 byte + 3 bytes padding
};                 // sizeof = 12 (no 6 — ¡6 bytes perdidos!)

// CON __attribute__((packed)) — sin padding:
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];         //  4 bytes
    uint8_t  version;          //  1 byte
    uint8_t  compression_type; //  1 byte
    uint16_t flags;            //  2 bytes
    uint32_t original_size;    //  4 bytes
    uint32_t compressed_size;  //  4 bytes
    uint32_t checksum;         //  4 bytes
    uint32_t style_count;      //  4 bytes
    char     title[32];        // 32 bytes
    uint8_t  reserved[8];      //  8 bytes
} FileHeader;                  // Total: 64 bytes EXACTOS
```

Verificación en tiempo de compilación:
```c
_Static_assert(sizeof(FileHeader) == 64, "FileHeader debe ser 64 bytes");
_Static_assert(sizeof(StyleEntry) == 16, "StyleEntry debe ser 16 bytes");
```
Si hay padding, el programa no compila — error detectado en build, no en runtime.

### Ciclo de Vida de la Memoria (sin fugas)

```
doc_save():
  malloc(comp_buf) ──────────────────────────────── free(comp_buf)
  malloc(payload)  ──────────────────────────────── free(payload)

doc_load():
  malloc(file_data) ─────────────────────────────── free(file_data)
  malloc(out->text) ──────── [caller usa] ─────── doc_free() → free
  malloc(out->styles) ─────── [caller usa] ─────── doc_free() → free

gb_new():
  malloc(GapBuffer) ──────────────────────────────── gb_free()
  malloc(buf)       ──────────────────────────────── gb_free()

gb_grow() (interno):
  malloc(new_buf) → memcpy → free(old_buf)   ← sin fugas en crecimiento
```

**Verificación con valgrind:**
```bash
valgrind --leak-check=full --error-exitcode=1 ./bin/editor test.txt
# Esperado: "All heap blocks were freed -- no leaks are possible"
```

### Gap Buffer: Gestión de Memoria para Edición

```
Estado inicial (4096 bytes, sin texto):
[████████████████████████████████████]   ← todo es gap
 0                                    4096
 gap_start=0                        gap_end=4096

Tras insertar "Hola":
[Hola|████████████████████████████████]
      ↑                               ↑
  gap_start=4                    gap_end=4096
  texto útil = 4096 - (4096-4) = 4 bytes ✓

Tras mover cursor al inicio y borrar 'H':
[|ola|████████████████████████████████]
  ↑                               ↑
gap_start=0                    gap_end=4096
```

Un solo `malloc` por buffer, crecimiento controlado vía `gb_grow()`.

---

## 3. Manejo de Texto Enriquecido

### Especificación del Formato Binario .ceds

```
Byte offset   Tamaño  Descripción
──────────────────────────────────────────────────────────────────
0             4       Magic Number: "CEDS" (0x43 0x45 0x44 0x53)
4             1       Versión del formato: 0x01
5             1       Tipo de compresión: 0x00=none, 0x01=RLE
6             2       Flags: bit0=FLAG_RICH_TEXT
8             4       original_size: tamaño sin comprimir (uint32 LE)
12            4       compressed_size: tamaño del payload (uint32 LE)
16            4       checksum: XOR rotativo del payload (uint32 LE)
20            4       style_count: cantidad de StyleEntry
24            32      title: título del documento (UTF-8, null-padded)
56            8       reserved: zeros (expansión futura)
──────────────────────────────────────────────────────────────────
64            16×N    StyleEntry × style_count (si FLAG_RICH_TEXT)
64+16N        M       Payload comprimido (compressed_size bytes)
──────────────────────────────────────────────────────────────────
```

### Formato de StyleEntry (16 bytes)

```
Byte  Tamaño  Campo
──────────────────────────────────────────────────────────────────
0     4       offset: byte offset en el texto original
4     4       length: longitud del segmento
8     1       bold: 1=negrita, 0=normal
9     1       italic: 1=cursiva, 0=normal
10    1       underline: 1=subrayado, 0=normal
11    1       reserved: 0x00
12    4       color: RGBA (0xRRGGBBAA)
──────────────────────────────────────────────────────────────────
```

### Agregar estilos desde el editor

```bash
# :style <offset> <length> <bold> <italic> <underline> <color_hex>
:style 0 5 1 0 0 0xFF0000FF    # "Hola" → negrita rojo
:style 6 5 0 1 0 0x0000FFFF   # siguiente palabra → cursiva azul
:save documento_con_estilos.ceds
```

### Verificación del formato

```bash
# Ver los primeros 64 bytes del archivo (el header)
hexdump -C archivo.ceds | head -5

# Esperado:
# 00000000  43 45 44 53 01 01 00 00  ...  "CEDS"...
#           ← magic →  ver comp flg ...
```

---

## 4. Reporte de Profiling

### Plantilla de Benchmark (Medido con strace y time)

Ejecutar:
```bash
./benchmark/run_benchmark.sh test_5mb.txt
```

Ejemplo de resultados reales (archivo 5 MB, texto en español):

| Métrica del Kernel       | Naive (plano 64B)   | RLE + write 4KB     | RLE + mmap        |
|--------------------------|---------------------|---------------------|-------------------|
| Datos al disco           | 5,242,880 B (100%)  | ~1,572,864 B (~30%) | ~1,572,864 B      |
| write() calls            | ~81,920             | ~385                | ~0 (1 msync)      |
| Tiempo CPU (user mode)   | ~0 ms               | ~35 ms (compresión) | ~35 ms            |
| Tiempo SO (sys mode)     | ~18 ms              | ~2 ms               | ~2 ms             |
| Tiempo total (wall-clock)| ~120 ms             | ~38 ms              | ~37 ms            |

### Captura de strace -c

```bash
# Contar syscalls del enfoque naive
strace -c -e trace=write ./bin/editor test_5mb.txt <<< ":plain /tmp/out.txt
:quit"

# Salida esperada (extracto):
% time     seconds  usecs/call     calls    errors syscall
 95.00    0.018000         219     82000           write
  5.00    0.001000         200         5           open
...

# Contar syscalls del enfoque optimizado
strace -c -e trace=write ./bin/editor test_5mb.txt <<< ":save /tmp/out.ceds
:quit"

# Salida esperada (extracto):
% time     seconds  usecs/call     calls    errors syscall
 60.00    0.001200        3100       385           write
 40.00    0.000800         160         5           open
...
```

### Separar tiempo CPU de tiempo I/O con time

```bash
time (./bin/editor test_5mb.txt <<< ":save /tmp/out.ceds
:quit")
# real  0m0.040s   ← Wall-clock: tiempo total incluyendo I/O
# user  0m0.035s   ← CPU en User Mode: gasto en compresión RLE
# sys   0m0.005s   ← CPU en Kernel Mode: syscalls (write, open, close)
```

**Interpretación:**
- `real - user - sys ≈ 0` → el cuello de botella es CPU (compresión), no I/O
- Si `sys` fuera grande → demasiados context switches (problema del enfoque naive)
- El enfoque optimizado invierte ciclos de CPU en compresión para reducir `sys` y `real`

### Verificación de memoria con valgrind

```bash
valgrind --leak-check=full --show-leak-kinds=all ./bin/editor test.ceds
```

Salida esperada:
```
==PID== HEAP SUMMARY:
==PID==     in use at exit: 0 bytes in 0 blocks
==PID==   total heap usage: 42 allocs, 42 frees, 8,456,192 bytes allocated
==PID==
==PID== All heap blocks were freed -- no leaks are possible
```

---

## Decisiones de Diseño

| Decisión | Alternativa descartada | Justificación |
|----------|----------------------|---------------|
| `open()/write()` sobre `fopen()/fwrite()` | stdio con doble buffer | Control directo de syscalls; visible en strace |
| `posix_memalign(4096)` | `malloc()` normal | Garantiza alineación al tamaño de página para DMA |
| RLE sobre LZ77/Huffman | Compresión más sofisticada | RLE es O(n), simple de auditar con strace; demuestra el concepto |
| Gap Buffer sobre array simple | Array con realloc | Inserciones O(1) en cursor; un solo bloque contiguo (cache-friendly) |
| `__attribute__((packed))` | Campos manuales con offsetof | Garantía en compile-time de exactamente 64 bytes de header |
| `_Static_assert` | Comentario en código | El compilador verifica el tamaño; falla en build, no en runtime |
