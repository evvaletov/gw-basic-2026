#ifndef GW_ERROR_H
#define GW_ERROR_H

#include <setjmp.h>

/* Error codes matching GW-BASIC's ERRTAB */
#define ERR_NF   1   /* NEXT without FOR */
#define ERR_SN   2   /* Syntax error */
#define ERR_RG   3   /* RETURN without GOSUB */
#define ERR_OD   4   /* Out of DATA */
#define ERR_FC   5   /* Illegal function call */
#define ERR_OV   6   /* Overflow */
#define ERR_OM   7   /* Out of memory */
#define ERR_UL   8   /* Undefined line number */
#define ERR_BS   9   /* Subscript out of range */
#define ERR_DD  10   /* Duplicate Definition */
#define ERR_DZ  11   /* Division by zero */
#define ERR_ID  12   /* Illegal direct */
#define ERR_TM  13   /* Type mismatch */
#define ERR_OS  14   /* Out of string space */
#define ERR_LS  15   /* String too long */
#define ERR_ST  16   /* String formula too complex */
#define ERR_CN  17   /* Can't continue */
#define ERR_UF  18   /* Undefined user function */
#define ERR_NR  19   /* No RESUME */
#define ERR_RW  20   /* RESUME without error */
#define ERR_UE  21   /* Unprintable error */
#define ERR_MO  22   /* Missing operand */
#define ERR_BO  23   /* Line buffer overflow */
#define ERR_DT  24   /* Device Timeout */
#define ERR_DF  25   /* Device Fault */
#define ERR_FW  26   /* FOR without NEXT */
#define ERR_OP  27   /* Out of Paper */
#define ERR_WH  29   /* WHILE without WEND */
#define ERR_WE  30   /* WEND without WHILE */

/* Disk errors (starting at 50) */
#define ERR_FO  50   /* FIELD overflow */
#define ERR_IE  51   /* Internal error */
#define ERR_BN  52   /* Bad file number */
#define ERR_FF  53   /* File not found */
#define ERR_BM  54   /* Bad file mode */
#define ERR_AO  56   /* File already open */
#define ERR_IO  58   /* Device I/O Error */
#define ERR_FE  60   /* File already exists */
#define ERR_FL  61   /* Disk full */
#define ERR_EF  62   /* Input past end */
#define ERR_RN  64   /* Bad record number */
#define ERR_NM  65   /* Bad file name */
#define ERR_DS  67   /* Direct statement in file */
#define ERR_TF  68   /* Too many files */
#define ERR_DA  70   /* Disk already exists */
#define ERR_PE  76   /* Path not found */

const char *gw_error_msg(int errnum);
void gw_error(int errnum);

/* Error recovery jump target - set in main loop */
extern jmp_buf gw_error_jmp;
extern int gw_errno;

#endif
