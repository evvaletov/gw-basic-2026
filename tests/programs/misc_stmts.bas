10 REM Test miscellaneous statements and functions
20 REM --- RESET closes all files (no error if none open)
30 RESET
40 PRINT "RESET OK"
50 REM --- ENVIRON sets a variable
60 ENVIRON "GWTEST=hello123"
70 PRINT "ENVIRON$="; ENVIRON$("GWTEST")
80 REM --- ENVIRON$ of unset var returns ""
90 PRINT "EMPTY="; ENVIRON$("XYZZY_NONEXIST"); "."
100 REM --- ERDEV returns 0
110 PRINT "ERDEV="; ERDEV
120 REM --- ERDEV$ returns ""
130 PRINT "ERDEV$="; ERDEV$; "."
140 REM --- IOCTL$ returns ""
150 OPEN "R", #1, "tests/programs/misc_stmts.bas"
160 PRINT "IOCTL$="; IOCTL$(#1); "."
170 CLOSE #1
180 REM --- LCOPY silently accepted
190 LCOPY
200 LCOPY 1
210 PRINT "LCOPY OK"
220 REM --- DATE$ assignment accepted
230 DATE$ = "01-01-2000"
240 PRINT "DATE$ SET OK"
250 REM --- TIME$ assignment accepted
260 TIME$ = "12:00:00"
270 PRINT "TIME$ SET OK"
280 REM --- FRE returns a positive number
290 IF FRE(0) > 0 THEN PRINT "FRE OK" ELSE PRINT "FRE FAIL"
300 REM --- COM accepted silently
310 COM ON
320 PRINT "COM OK"
330 REM --- MOTOR accepted silently
340 MOTOR
350 PRINT "MOTOR OK"
360 PRINT "ALL DONE"
370 SYSTEM
