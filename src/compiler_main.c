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
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

/* GCC must receive paths literally, including spaces and shell metacharacters. */
static int run_gcc(char *const args[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("Cannot start gcc");
        return 1;
    }
    if (pid == 0) {
        execvp(args[0], args);
        perror("Cannot execute gcc");
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("Cannot wait for gcc");
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

static char *path_suffix(const char *name, const char *suffix)
{
    size_t length = strlen(name) + strlen(suffix) + 1;
    char *result = malloc(length);
    if (!result) {
        fprintf(stderr, "Out of memory building compiler path\n");
        exit(1);
    }
    snprintf(result, length, "%s%s", name, suffix);
    return result;
}

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
        if (tok_len < 0) {
            fprintf(stderr, "Line buffer overflow in %s at %u\n", filename, (unsigned)num);
            fclose(f);
            return 1;
        }
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
        "  -O LEVEL         Optimization tier 0..3 (default 2). Drives both the\n"
        "                   codegen tier (O0 literal C; O1 +const-fold +dead-code;\n"
        "                   O2 +fast-path emitter) and the gcc -O level.\n"
        "  --keep-c         Keep generated C file (with -c)\n"
        "  --runtime DIR    Path to runtime headers/library\n"
        "  --warn           Static analysis warnings\n"
        "  --safe           Runtime safety checks (implies --warn)\n"
        "  --safe=sanitize  Above + address/UB sanitizers (with -c)\n"
        "  --no-gc-check    Skip per-line gwrt_check_line() (no GC, no Break)\n"
        "  --fast-math      Skip division-by-zero checks\n"
        "  --inline-arrays  Cache runtime storage of eligible numeric arrays for\n"
        "                   direct indexing (--safe uses runtime lookups)\n"
        "  --emit-obj       Compile to object file (.o) instead of executable\n"
        "  --main-name N    Rename emitted entry point from main to N (for\n"
        "                   linking BASIC into a larger C/Fortran project)\n"
        "  --max-output-size MB  Abort if the generated C exceeds MB megabytes\n"
        "                   (default 256; 0 = unlimited; guards codegen loops)\n"
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
    bool inline_arrays = false;
    bool emit_obj = false;
    const char *main_name = NULL;
    unsigned long max_output_mb = 256;  /* abort runaway codegen; 0 = unlimited */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "-c") == 0)
            compile_exe = true;
        else if (strcmp(argv[i], "-O") == 0 && i + 1 < argc)
            opt_level = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] == 'O'
                 && argv[i][2] >= '0' && argv[i][2] <= '9' && argv[i][3] == '\0')
            opt_level = argv[i][2] - '0';   /* glued form: -O0..-O3 */
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
        else if (strcmp(argv[i], "--inline-arrays") == 0)
            inline_arrays = true;
        else if (strcmp(argv[i], "--emit-obj") == 0)
            emit_obj = true;
        else if (strcmp(argv[i], "--main-name") == 0 && i + 1 < argc)
            main_name = argv[++i];
        else if (strncmp(argv[i], "--main-name=", 12) == 0)
            main_name = argv[i] + 12;
        else if (strcmp(argv[i], "--max-output-size") == 0 && i + 1 < argc)
            max_output_mb = strtoul(argv[++i], NULL, 10);
        else if (strncmp(argv[i], "--max-output-size=", 18) == 0)
            max_output_mb = strtoul(argv[i] + 18, NULL, 10);
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
    char *c_file_buf = NULL;
    if ((compile_exe || emit_obj) && !output) {
        c_file_buf = path_suffix(input, ".c");
        c_file = c_file_buf;
    }

    FILE *f = c_file ? fopen(c_file, "w") : stdout;
    if (!f) {
        fprintf(stderr, "Cannot write: %s\n", c_file);
        for (int i = 0; i < analysis.data_count; i++)
            free(analysis.data_pool[i]);
        return 1;
    }

    if (opt_level < 0) opt_level = 0;
    if (opt_level > 3) opt_level = 3;

    codegen_opts_t opts = {
        .safe_mode = safe_mode,
        .warn_mode = warn_mode,
        .opt_level = opt_level,
        .inline_arrays = inline_arrays,
        .no_gc_check = no_gc_check,
        .fast_math = fast_math,
        .main_name = main_name,
        .max_output_bytes = (size_t)max_output_mb * 1024 * 1024,
        .out_path = (f == stdout) ? NULL : c_file,
    };
    codegen_emit(f, &analysis, &opts);
    for (int i = 0; i < analysis.data_count; i++)
        free(analysis.data_pool[i]);

    if (f != stdout)
        fclose(f);

    /* Compile to executable or object file if requested */
    if (compile_exe || emit_obj) {
        const char *rt = runtime_dir ? runtime_dir : ".";
        /* Derive output name from input (drop extension, pick .o or no
         * suffix for the executable). */
        char *out_name = path_suffix(input, ".out");
        out_name[strlen(input)] = '\0';
        char *base = strrchr(out_name, '/');
        base = base ? base + 1 : out_name;
        char *dot = strrchr(base, '.');
        if (dot && dot != base) *dot = '\0';
        else if (!emit_obj) strcat(out_name, ".out");
        if (emit_obj) strcat(out_name, ".o");

        char optimization[4];
        snprintf(optimization, sizeof(optimization), "-O%d", opt_level);
        char *include_dir = path_suffix(rt, "/include");
        char *library_dir = path_suffix(rt, "/build");
        char *args[24];
        int count = 0;
        args[count++] = "gcc";
        args[count++] = optimization;
        if (sanitize_mode) {
            args[count++] = "-fsanitize=address,undefined";
            args[count++] = "-fno-sanitize-recover=all";
        }
        if (emit_obj) args[count++] = "-c";
        args[count++] = "-o";
        args[count++] = out_name;
        args[count++] = (char *)c_file;
        args[count++] = "-I";
        args[count++] = include_dir;
        if (!emit_obj) {
            args[count++] = "-L";
            args[count++] = library_dir;
            args[count++] = "-lgwrt";
            args[count++] = "-lm";
            args[count++] = "-lpthread";
#ifdef GWRT_HAS_PULSEAUDIO
            args[count++] = "-lpulse-simple";
#endif
        }
        args[count] = NULL;

        int rc = run_gcc(args);
        free(include_dir);
        free(library_dir);
        if (rc != 0) {
            fprintf(stderr, "gcc failed (exit %d)\n", rc);
            free(out_name);
            free(c_file_buf);
            return 1;
        }

        if (!keep_c && c_file != output)
            remove(c_file);

        fprintf(stderr, "Compiled: %s\n", out_name);
        free(out_name);
    }

    free(c_file_buf);
    return 0;
}
