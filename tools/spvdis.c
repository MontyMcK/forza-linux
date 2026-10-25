// Minimal SPIR-V disassembler on top of the system SPIRV-Tools library
// (spirv-tools-libs; the spirv-dis binary is not installed). Used to read the
// shaders DXVK dumps with DXVK_SHADER_DUMP_PATH (keep those dumps in
// research/private/: they are derived from game data).
//
//   clang -O1 -o build/spvdis tools/spvdis.c -l:libSPIRV-Tools-shared.so
//   build/spvdis research/private/shaders/fs.<hash>.spv
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct spv_context_t* spv_context;
typedef struct { const char* str; size_t length; } spv_text_t;
typedef struct { struct { size_t line, column, index; } position; char* error; int is_text_source; } spv_diagnostic_t;

spv_context spvContextCreate(int env);
int spvBinaryToText(spv_context ctx, const uint32_t* binary, size_t words, uint32_t options, spv_text_t** text,
                    spv_diagnostic_t** diagnostic);

enum { kEnvUniversal16 = 24, kOptIndent = 1 << 3, kOptFriendlyNames = 1 << 6, kOptComment = 1 << 7 };

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s file.spv\n", argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t* words = malloc(n);
    if (fread(words, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    spv_text_t* text = NULL;
    spv_diagnostic_t* diag = NULL;
    int r = spvBinaryToText(spvContextCreate(kEnvUniversal16), words, n / 4,
                            kOptIndent | kOptFriendlyNames | kOptComment, &text, &diag);
    if (r != 0) {
        fprintf(stderr, "spvBinaryToText failed (%d): %s\n", r, diag && diag->error ? diag->error : "?");
        return 1;
    }
    fwrite(text->str, 1, text->length, stdout);
    return 0;
}
