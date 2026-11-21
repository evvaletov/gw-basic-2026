#include "gwbasic.h"
#include "debug.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __MSDOS__
#include <signal.h>
#endif

/* Fixed tables keep the monitor small on 16-bit DOS. Inspection does not call
   the evaluator, PTRGET, array_element, string allocation or file readers. */
#define SLOTS 16
static FILE *commands, *out;
static int stepping = 1, nw, nb, io_failed;
static char watches[SLOTS][80];
static struct { unsigned line; int offset; } breaks[SLOTS];
static unsigned long stops;

#ifndef __MSDOS__
/* Let debugger-owned writes report EFBIG, so the documented I/O failure path
   can close files and restore the terminal before exit(2). Guest execution
   keeps its original signal mask and dispositions. */
static void block_log_limit(sigset_t *previous) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGXFSZ);
    sigprocmask(SIG_BLOCK, &blocked, previous);
}
#endif

static void close_debug(void) {
#ifndef __MSDOS__
    sigset_t previous;
    block_log_limit(&previous);
#endif
    int bad = 0;
    if (commands && fclose(commands)) bad = 1;
    if (out) { if (ferror(out)) bad = 1; if (fclose(out)) bad = 1; }
    commands = out = NULL;
    if (bad && !io_failed) {
        if (gw_hal) gw_hal->shutdown();
        fputs("gwbasic: debugger I/O failure\n", stderr);
        fflush(NULL);
        /* Already in an atexit handler; do not invoke the handlers again. */
        _exit(2);
    }
#ifndef __MSDOS__
    sigprocmask(SIG_SETMASK, &previous, NULL);
#endif
}

static void io_error(void) {
    io_failed = 1;
    if (gw_hal) gw_hal->shutdown();
    fputs("gwbasic: debugger I/O failure\n", stderr);
    exit(2);
}

static void flush_debug(void) {
    if (out && (ferror(out) || fflush(out))) io_error();
}

void gw_debug_open(const char *script, const char *output) {
    if (!script && !output) return;
    if (!script || !output) {
        fputs("debug needs --debug-script and --debug-output\n", stderr); exit(1);
    }
    commands = fopen(script, "r");
    if (!commands) { fputs("cannot open debug script\n", stderr); exit(1); }
    int fd = open(output, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || !(out = fdopen(fd, "w"))) {
        if (fd >= 0) close(fd);
        fclose(commands); commands = NULL;
        fputs("cannot create debug output (must be a new file)\n", stderr); exit(1);
    }
    if (atexit(close_debug)) { fputs("cannot register debugger cleanup\n", stderr); exit(1); }
#ifndef __MSDOS__
    sigset_t previous;
    block_log_limit(&previous);
#endif
    fputs("debug-v1 engine=basic-interpreter frontend=line\n", out);
    flush_debug();
#ifndef __MSDOS__
    sigprocmask(SIG_SETMASK, &previous, NULL);
#endif
}

static int reference(const char *s, char name[GW_NAME_SIZE], gw_valtype_t *type,
                     int subs[8], int *rank) {
    int n = 0;
    if (!isalpha((unsigned char)*s)) return 0;
    while (isalnum((unsigned char)*s) || *s == '.') {
        if (n == GW_NAME_MAX) return 0;
        name[n++] = (char)toupper((unsigned char)*s++);
    }
    name[n] = 0;
    *type = gw.def_type[name[0]-'A'];
    if (*s == '%') { *type = VT_INT; s++; }
    else if (*s == '!') { *type = VT_SNG; s++; }
    else if (*s == '#') { *type = VT_DBL; s++; }
    else if (*s == '$') { *type = VT_STR; s++; }
    *rank = 0;
    if (*s == '(') {
        do {
            char *end; s++;
            if (*rank == 8 || !isdigit((unsigned char)*s)) return 0;
            errno = 0; long x = strtol(s, &end, 10);
            if (errno || x < 0 || x > 32767) return 0;
            subs[(*rank)++] = (int)x; s = end;
        } while (*s == ',');
        if (*s++ != ')') return 0;
    }
    return !*s;
}

static void native_value(const gw_value_t *v) {
    switch (v->type) {
    case VT_INT: fprintf(out, "integer=%d\n", v->ival); break;
    case VT_SNG: fprintf(out, "single=%.9g\n", (double)v->fval); break;
    case VT_DBL: fprintf(out, "double=%.17g\n", v->dval); break;
    case VT_STR:
        fprintf(out, "string length=%u hex=", (unsigned)v->sval.len);
        for (unsigned i = 0; i < v->sval.len; i++) fprintf(out, "%02x", (unsigned char)v->sval.data[i]);
        fputc('\n', out); break;
    default: fputs("unavailable\n", out); break;
    }
}

static void value(const char *ref) {
    char name[GW_NAME_SIZE]; gw_valtype_t type;
    int subs[8], rank;
    if (!reference(ref, name, &type, subs, &rank)) { fprintf(out, "error invalid-reference %s\n", ref); return; }
    fprintf(out, "value %s ", ref);
    if (!rank) {
        for (int i = 0; i < gw.var_count; i++)
            if (gw.vars[i].type == type && !strcmp(gw.vars[i].name, name)) {
                native_value(&gw.vars[i].val); return;
            }
        gw_value_t zero; memset(&zero, 0, sizeof zero); zero.type = type;
        fputs("default ", out); native_value(&zero); return;
    }
    array_entry_t *a = NULL;
    for (int i = 0; i < gw.array_count; i++)
        if (gw.arrays[i].type == type && !strcmp(gw.arrays[i].name, name)) { a = &gw.arrays[i]; break; }
    if (!a) { fputs("array-not-allocated\n", out); return; }
    if (a->ndims != rank) { fputs("error rank-mismatch\n", out); return; }
    int index = 0, stride = 1;
    fputs("bounds=", out);
    for (int i = 0; i < rank; i++)
        fprintf(out, "%s%d:%d", i ? "," : "", gw.option_base, a->dims[i]);
    for (int i = 0; i < rank; i++) {
        if (subs[i] < gw.option_base || subs[i] > a->dims[i]) { fputs(" error bounds\n", out); return; }
        index += (subs[i] - gw.option_base) * stride;
        stride *= a->dims[i] - gw.option_base + 1;
    }
    fputc(' ', out); native_value(&a->data[index]);
}

static void file_info(int number, int start, int count) {
    file_entry_t *f = &gw.files[number];
    if (!f->fp) { fprintf(out, "file %d closed\n", number); return; }
    fprintf(out, "file %d mode=%c position=%ld record-length=%d input-cursor=%d eof=%d\n",
            number, f->mode, ftell(f->fp), f->record_len, f->input_pos, f->eof_flag);
    for (int i = 0; i < f->field_count; i++)
        fprintf(out, "field %s file=%d offset=%d length=%d\n", f->fields[i].name,
                number, f->fields[i].offset, f->fields[i].width);
    if (f->field_buf && start < f->record_len) {
        fprintf(out, "buffer offset=%d hex=", start);
        for (int i = start; i < f->record_len && i - start < count; i++) fprintf(out, "%02x", (unsigned)f->field_buf[i]);
        fputc('\n', out);
    }
}

static int file_request(const char *s, int values[3]) {
    for (int i = 0; i < 3; i++) {
        while (*s == ' ') s++;
        size_t n = strspn(s, "0123456789");
        if (!n || n > 4) return 0;
        values[i] = 0;
        while (n--) values[i] = values[i] * 10 + (*s++ - '0');
        if (*s && *s != ' ') return 0;
    }
    while (*s == ' ') s++;
    return !*s;
}

static void state(void) {
    fprintf(out, "state variables=%d arrays=%d for-depth=%d gosub-depth=%d while-depth=%d\n",
            gw.var_count, gw.array_count, gw.for_sp, gw.gosub_sp, gw.while_sp);
    for (int i = 1; i <= 15; i++) if (gw.files[i].fp) file_info(i, 0, 16);
}

static void values(void) {
    for (int i = 0; i < nw; i++) value(watches[i]);
}

/* Keep a partially received command across a handled signal. Reads from the
   POSIX controller are atomic, but a user-supplied FIFO may arrive in chunks. */
static int read_command(char line[256]) {
    size_t n = 0;
    int too_long = 0;
    for (;;) {
        errno = 0;
        int c = fgetc(commands);
        if (c == EOF) {
            if (ferror(commands)) {
                if (errno == EINTR) { clearerr(commands); continue; }
                io_error();
            }
            if (!n && !too_long) return 0;
            break;
        }
        if (c == '\n') break;
        if (n < 255) line[n++] = (char)c;
        else too_long = 1;
    }
    if (n && line[n-1] == '\r') n--;
    line[n] = 0;
    return too_long || n > 254 ? -1 : 1;
}

static void monitor(void) {
    if (!commands || !gw.running || !gw.cur_line) return;
    int offset = (int)(gw.text_ptr - gw.cur_line->tokens);
    int hit = stepping;
    for (int i = 0; i < nb; i++)
        if (breaks[i].line == gw.cur_line_num && (breaks[i].offset < 0 || breaks[i].offset == offset)) hit = 1;
    if (!hit) return;
    fprintf(out, "stop %lu line=%u offset=%d depth=%d\n", ++stops, (unsigned)gw.cur_line_num, offset, gw.gosub_sp);
    values();
    char line[256];
    for (;;) {
        flush_debug();
        int command = read_command(line);
        if (!command) {
            fputs("resume eof\n", out); flush_debug();
            FILE *f = commands; commands = NULL;
            if (fclose(f)) io_error();
            return;
        }
        if (command < 0) {
            fputs("error command-too-long\n", out); continue;
        }
        if (strchr(line, '\r')) { fputs("error invalid-command\n", out); continue; }
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char *arg = p; while (*arg && !isspace((unsigned char)*arg)) arg++;
        if (*arg) *arg++ = 0;
        while (isspace((unsigned char)*arg)) arg++;
        size_t len = strlen(arg); while (len && isspace((unsigned char)arg[len-1])) arg[--len] = 0;
        if (!strcmp(p, "watch") || !strcmp(p, "unwatch"))
            for (char *c = arg; *c; c++) *c = (char)toupper((unsigned char)*c);
        if ((!strcmp(p, "step") || !strcmp(p, "continue")) && !*arg) { stepping = !strcmp(p, "step"); return; }
        if (!strcmp(p, "capabilities") && !*arg) {
            fputs("capabilities step continue break clear watch unwatch inspect checkpoint snapshot; aot=no next=no finish=no restore=no\n", out);
        } else if (!strcmp(p, "break")) {
            char *end; errno = 0; long n = strtol(arg, &end, 10);
            int has_line = end != arg;
            long off = -1;
            if (*end == ':') { char *begin = end+1; off = strtol(begin, &end, 10); if (end == begin || off < 0 || off > 32767) off = -2; }
            if (errno || !has_line || *end || n < 0 || n > 65529L || off < -1 || nb == SLOTS)
                fputs("error breakpoint\n", out);
            else { breaks[nb].line = (unsigned)n; breaks[nb++].offset = (int)off; fprintf(out, "break line=%ld offset=%ld\n", n, off); }
        } else if (!strcmp(p, "clear") && !*arg) nb = 0;
        else if (!strcmp(p, "watch")) {
            char name[GW_NAME_SIZE]; gw_valtype_t type; int subs[8], rank;
            int exists = 0;
            for (int i = 0; i < nw; i++) if (!strcmp(arg, watches[i])) exists = 1;
            if (exists) { value(arg); continue; }
            if (nw == SLOTS || strlen(arg) >= sizeof watches[0] || !reference(arg, name, &type, subs, &rank)) fputs("error watch\n", out);
            else { strcpy(watches[nw++], arg); value(arg); }
        } else if (!strcmp(p, "unwatch")) {
            char name[GW_NAME_SIZE]; gw_valtype_t type; int subs[8], rank;
            if (!reference(arg, name, &type, subs, &rank)) { fputs("error watch\n", out); continue; }
            for (int i = 0; i < nw; i++) if (!strcmp(arg, watches[i])) {
                nw--;
                if (i < nw) memmove(watches[i], watches[i+1], (size_t)(nw-i)*sizeof watches[0]);
                break;
            }
        } else if (!strcmp(p, "inspect")) {
            if (!strcmp(arg, "state")) state();
            else if (!strncmp(arg, "file ", 5)) {
                int request[3];
                if (file_request(arg+5, request) && request[0] >= 1 && request[0] <= 15 &&
                    request[2] >= 1 && request[2] <= 64) file_info(request[0], request[1], request[2]);
                else fputs("error file-request (file NUMBER OFFSET COUNT)\n", out);
            } else value(arg);
        } else if (!strcmp(p, "checkpoint") || !strcmp(p, "snapshot")) {
            if (!*arg || strlen(arg) > 64 || strspn(arg, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != strlen(arg)) {
                fputs("error checkpoint-name\n", out); continue;
            }
            fprintf(out, "checkpoint %s stop=%lu begin scope=watches-state-files\n", arg, stops);
            values(); state(); fprintf(out, "checkpoint %s end\n", arg);
        } else fputs("error unsupported-command\n", out);
    }
}

void gw_debug_stop(void) {
    if (!commands) return;
    int saved_errno = errno;
#ifndef __MSDOS__
    sigset_t previous;
    block_log_limit(&previous);
#endif
    monitor();
#ifndef __MSDOS__
    sigprocmask(SIG_SETMASK, &previous, NULL);
#endif
    errno = saved_errno;
}
