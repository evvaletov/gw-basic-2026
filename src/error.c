#include "error.h"
#include "hal.h"
#include "interp.h"
#include "gwbasic.h"
#include <stdio.h>
#include <stdlib.h>

jmp_buf gw_error_jmp;
int gw_errno = 0;

static const struct { int num; const char *msg; } error_table[] = {
    {  1, "NEXT without FOR" },
    {  2, "Syntax error" },
    {  3, "RETURN without GOSUB" },
    {  4, "Out of DATA" },
    {  5, "Illegal function call" },
    {  6, "Overflow" },
    {  7, "Out of memory" },
    {  8, "Undefined line number" },
    {  9, "Subscript out of range" },
    { 10, "Duplicate Definition" },
    { 11, "Division by zero" },
    { 12, "Illegal direct" },
    { 13, "Type mismatch" },
    { 14, "Out of string space" },
    { 15, "String too long" },
    { 16, "String formula too complex" },
    { 17, "Can't continue" },
    { 18, "Undefined user function" },
    { 19, "No RESUME" },
    { 20, "RESUME without error" },
    { 21, "Unprintable error" },
    { 22, "Missing operand" },
    { 23, "Line buffer overflow" },
    { 24, "Device Timeout" },
    { 25, "Device Fault" },
    { 26, "FOR without NEXT" },
    { 27, "Out of Paper" },
    { 29, "WHILE without WEND" },
    { 30, "WEND without WHILE" },
    { 50, "FIELD overflow" },
    { 51, "Internal error" },
    { 52, "Bad file number" },
    { 53, "File not found" },
    { 54, "Bad file mode" },
    { 56, "File already open" },
    { 58, "Device I/O Error" },
    { 60, "File already exists" },
    { 62, "Disk full" },
    { 63, "Input past end" },
    { 64, "Bad record number" },
    { 65, "Bad file name" },
    { 67, "Direct statement in file" },
    { 68, "Too many files" },
    {  0, NULL }
};

const char *gw_error_msg(int errnum)
{
    for (int i = 0; error_table[i].msg; i++) {
        if (error_table[i].num == errnum)
            return error_table[i].msg;
    }
    return "Unprintable error";
}

void gw_error(int errnum)
{
    gw_errno = errnum;
    gw.err_line_num = gw.cur_line_num;

    /* ON ERROR GOTO handler */
    if (gw.on_error_line && gw.running && !gw.in_error_handler) {
        program_line_t *target = gw_find_line(gw.on_error_line);
        if (target) {
            gw.in_error_handler = true;
            gw.err_resume_text = gw.text_ptr;
            gw.err_resume_line = gw.cur_line;
            gw.cur_line = target;
            gw.text_ptr = target->tokens;
            gw.cur_line_num = target->num;
            longjmp(gw_run_jmp, 1);
        }
    }

    const char *msg = gw_error_msg(errnum);
    char buf[80];

    if (gw.cur_line_num != LINE_DIRECT)
        snprintf(buf, sizeof(buf), "\n%s in %u\n", msg, gw.cur_line_num);
    else
        snprintf(buf, sizeof(buf), "\n%s\n", msg);

    if (gw_hal)
        gw_hal->puts(buf);
    else
        fputs(buf, stderr);

    gw.running = false;
    longjmp(gw_error_jmp, errnum);
}
