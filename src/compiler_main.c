/*
 * gwbasic-compile: Ahead-of-time compiler for GW-BASIC programs.
 *
 * Tokenizes a .bas file using the existing tokenizer, runs an analysis
 * pass to collect variables/targets/DATA, then emits C source that links
 * against libgwrt to produce a native executable.
 */

#include "gwbasic.h"
#include "analysis.h"
#include "codegen.h"
#include "strpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global interpreter state (needed by tokenizer and analysis) */
interp_state_t gw;
hal_ops_t *gw_hal = NULL;

/* Stubs for symbols referenced by shared modules but not needed by compiler */
jmp_buf gw_error_jmp;
jmp_buf gw_run_jmp;
int print_col = 0;

void gw_init(void)
{
    memset(&gw, 0, sizeof(gw));
    for (int i = 0; i < 26; i++)
        gw.def_type[i] = VT_SNG;
}

/* Minimal main.c functions the compiler needs */
uint8_t gw_chrget(void)
{
    gw.text_ptr++;
    while (*gw.text_ptr == ' ') gw.text_ptr++;
    return *gw.text_ptr;
}

uint8_t gw_chrgot(void) { return *gw.text_ptr; }

void gw_skip_spaces(void)
{
    while (*gw.text_ptr == ' ') gw.text_ptr++;
}

/* Store a tokenized line into the program */
static void store_line(uint16_t num, uint8_t *tokens, int len)
{
    program_line_t *line = malloc(sizeof(program_line_t));
    line->num = num;
    line->len = len;
    line->tokens = malloc(len + 1);
    memcpy(line->tokens, tokens, len);
    line->tokens[len] = 0;
    line->next = NULL;

    /* Insert in order */
    if (!gw.prog_head || num < gw.prog_head->num) {
        line->next = gw.prog_head;
        gw.prog_head = line;
        return;
    }
    program_line_t *prev = gw.prog_head;
    while (prev->next && prev->next->num < num)
        prev = prev->next;
    if (prev->next && prev->next->num == num) {
        /* Replace existing line */
        program_line_t *old = prev->next;
        line->next = old->next;
        prev->next = line;
        free(old->tokens);
        free(old);
    } else {
        line->next = prev->next;
        prev->next = line;
    }
}

/* Parse a line number from the start of a text line */
static int parse_line_num(const char *text, uint16_t *num)
{
    const char *p = text;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return 0;
    *num = 0;
    while (*p >= '0' && *p <= '9') {
        *num = *num * 10 + (*p - '0');
        p++;
    }
    return (int)(p - text);
}

/* Load a .bas file (ASCII format).  Lines that don't begin with a number
 * are treated as direct-mode statements and given auto-assigned numbers
 * (10, 20, ...) so the compiler can handle scratchpad-style programs the
 * interpreter accepts via stdin or `gwbasic file.bas`. */
static int load_file(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", filename);
        return 1;
    }

    char buf[256];
    uint8_t kbuf[256];
    uint16_t last_num = 0;
    while (fgets(buf, sizeof(buf), f)) {
        int len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        if (buf[0] == '\0') continue;

        uint16_t num;
        int skip = parse_line_num(buf, &num);
        const char *content;
        if (skip == 0) {
            /* Unnumbered direct-mode line: auto-assign next number. */
            if (last_num >= 65520) {
                fprintf(stderr, "Auto line numbering overflow in %s\n", filename);
                fclose(f);
                return 1;
            }
            num = last_num + 10;
            content = buf;
            while (*content == ' ') content++;
        } else {
            content = buf + skip;
            while (*content == ' ') content++;
        }
        last_num = num;

        int tok_len = gw_crunch(content, kbuf, sizeof(kbuf));
        store_line(num, kbuf, tok_len);
    }

    fclose(f);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: gwbasic-compile [options] input.bas\n"
        "Options:\n"
        "  -o FILE          Output C source file (default: stdout)\n"
        "  -c               Compile to executable (invoke gcc)\n"
        "  -O LEVEL         GCC optimization level (default: 2)\n"
        "  --keep-c         Keep generated C file (with -c)\n"
        "  --runtime DIR    Path to runtime headers/library\n"
        "  --warn           Static analysis warnings\n"
        "  --safe           Runtime safety checks (implies --warn)\n"
        "  --safe=sanitize  Above + address/UB sanitizers (with -c)\n"
        "  --no-gc-check    Skip per-line gwrt_check_line() (no GC, no Break)\n"
        "  --fast-math      Skip division-by-zero checks\n"
        "  --emit-obj       Compile to object file (.o) instead of executable\n"
        "  --main-name N    Rename emitted entry point from main to N (for\n"
        "                   linking BASIC into a larger C/Fortran project)\n"
    );
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *output = NULL;
    bool compile_exe = false;
    int opt_level = 2;
    bool keep_c = false;
    const char *runtime_dir = NULL;
    bool warn_mode = false;
    bool safe_mode = false;
    bool sanitize_mode = false;
    bool no_gc_check = false;
    bool fast_math = false;
    bool emit_obj = false;
    const char *main_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "-c") == 0)
            compile_exe = true;
        else if (strcmp(argv[i], "-O") == 0 && i + 1 < argc)
            opt_level = atoi(argv[++i]);
        else if (strcmp(argv[i], "--keep-c") == 0)
            keep_c = true;
        else if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)
            runtime_dir = argv[++i];
        else if (strcmp(argv[i], "--warn") == 0)
            warn_mode = true;
        else if (strcmp(argv[i], "--safe=sanitize") == 0)
            sanitize_mode = safe_mode = warn_mode = true;
        else if (strcmp(argv[i], "--safe") == 0)
            safe_mode = warn_mode = true;
        else if (strcmp(argv[i], "--no-gc-check") == 0)
            no_gc_check = true;
        else if (strcmp(argv[i], "--fast-math") == 0)
            fast_math = true;
        else if (strcmp(argv[i], "--emit-obj") == 0)
            emit_obj = true;
        else if (strcmp(argv[i], "--main-name") == 0 && i + 1 < argc)
            main_name = argv[++i];
        else if (strncmp(argv[i], "--main-name=", 12) == 0)
            main_name = argv[i] + 12;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        else if (argv[i][0] != '-')
            input = argv[i];
    }

    if (!input) {
        usage();
        return 1;
    }

    gw_init();

    if (load_file(input) != 0)
        return 1;

    if (!gw.prog_head) {
        fprintf(stderr, "No program lines found in %s\n", input);
        return 1;
    }

    /* Analysis pass */
    analysis_t analysis;
    analysis_run(&analysis);

    /* Static analysis warnings */
    if (warn_mode)
        analysis_warnings(&analysis);

    /* Code generation */
    const char *c_file = output;
    char c_file_buf[512];
    if ((compile_exe || emit_obj) && !output) {
        snprintf(c_file_buf, sizeof(c_file_buf), "%s.c", input);
        c_file = c_file_buf;
    }

    FILE *f = c_file ? fopen(c_file, "w") : stdout;
    if (!f) {
        fprintf(stderr, "Cannot write: %s\n", c_file);
        return 1;
    }

    codegen_opts_t opts = {
        .safe_mode = safe_mode,
        .warn_mode = warn_mode,
        .no_gc_check = no_gc_check,
        .fast_math = fast_math,
        .main_name = main_name,
    };
    codegen_emit(f, &analysis, &opts);

    if (f != stdout)
        fclose(f);

    /* Compile to executable or object file if requested */
    if (compile_exe || emit_obj) {
        char cmd[2048];
        const char *rt = runtime_dir ? runtime_dir : ".";
        /* Derive output name from input (drop extension, pick .o or no
         * suffix for the executable). */
        char out_name[512];
        strncpy(out_name, input, sizeof(out_name) - 1);
        out_name[sizeof(out_name) - 1] = '\0';
        char *dot = strrchr(out_name, '.');
        if (dot) *dot = '\0';
        if (emit_obj) {
            size_t len = strlen(out_name);
            if (len + 3 < sizeof(out_name))
                strcpy(out_name + len, ".o");
        }

        const char *san_flags = sanitize_mode
            ? " -fsanitize=address,undefined -fno-sanitize-recover=all" : "";
        if (emit_obj) {
            /* Compile-only; the host project handles the link step.
             * No -L / -l flags here -- those belong on the user's link
             * command line. */
            snprintf(cmd, sizeof(cmd),
                "gcc -O%d%s -c -o %s %s -I%s/include 2>&1",
                opt_level, san_flags, out_name, c_file, rt);
        } else {
#ifdef GWRT_HAS_PULSEAUDIO
            const char *pulse_lib = " -lpulse-simple";
#else
            const char *pulse_lib = "";
#endif
            snprintf(cmd, sizeof(cmd),
                "gcc -O%d%s -o %s %s -I%s/include -L%s/build -lgwrt -lm -lpthread%s 2>&1",
                opt_level, san_flags, out_name, c_file, rt, rt, pulse_lib);
        }

        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "gcc failed (exit %d)\n", rc);
            return 1;
        }

        if (!keep_c && c_file != output)
            remove(c_file);

        fprintf(stderr, "Compiled: %s\n", out_name);
    }

    return 0;
}
