/* Exercise the shipped loader with real packages and the linked host addresses.
 * Never execute target machine code on the build host. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_elf.h"
#include "host_imports.h"

static unsigned allocations;
static bool refuse_imports;
static bool refuse_memory;
void *esp_elf_malloc(uint32_t bytes, bool executable) {
    (void)executable;
    void *memory = refuse_memory ? NULL : malloc(bytes);
    if (memory)
        ++allocations;
    return memory;
}
void esp_elf_free(void *memory) {
    if (memory)
        --allocations;
    free(memory);
}
uintptr_t elf_find_sym_default(const char *name) {
    if (!refuse_imports) {
        for (unsigned i = 0; imports[i].name; ++i)
            if (strcmp(name, imports[i].name) == 0)
                return imports[i].address;
    }
    return 0;
}
struct source {
    FILE *file;
    size_t length;
};
static size_t length(void *context) { return ((struct source *)context)->length; }
static int read_at(void *context, size_t pos, void *dest, size_t count) {
    FILE *file = ((struct source *)context)->file;
    return fseek(file, (long)pos, SEEK_SET) || fread(dest, 1, count, file) != count ? -EIO : 0;
}
static void relocate(const char *path, esp_elf_t *image, bool success) {
    struct source input = {.file = fopen(path, "rb")};
    assert(input.file && !fseek(input.file, 0, SEEK_END));
    input.length = (size_t)ftell(input.file);
    elf_source_t source = {.ctx = &input, .size = length, .read = read_at};
    assert(esp_elf_init(image) == 0);
    assert((esp_elf_relocate_source(image, &source) == 0) == success);
    if (success)
        assert(image->entry != NULL);
    fclose(input.file);
}
int main(int argc, char **argv) {
    esp_elf_t *images = calloc(argc, sizeof(*images));
    assert(images && argc > 1);
    for (int run = 0; run < 100; ++run) {
        for (int i = 1; i < argc; ++i)
            relocate(argv[i], &images[i], true);
        for (int i = argc - 1; i > 0; --i)
            esp_elf_deinit(&images[i]);
        assert(allocations == 0);
    }
    refuse_imports = true;
    relocate(argv[1], &images[1], false);
    esp_elf_deinit(&images[1]);
    refuse_memory = true;
    relocate(argv[1], &images[1], false);
    esp_elf_deinit(&images[1]);
    assert(allocations == 0);
    free(images);
}
