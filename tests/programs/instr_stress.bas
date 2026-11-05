10 REM INSTR in every form: 2-arg, 3-arg, literals, vars, funcs, exprs
20 H$ = "hello world hello"
30 N$ = "hello"
40 PRINT INSTR(H$, N$)
50 PRINT INSTR(7, H$, N$)
60 PRINT INSTR("abcabc", "c")
70 PRINT INSTR(4, "abcabc", "c")
80 PRINT INSTR(H$, "zzz")
90 PRINT INSTR(H$, LEFT$(N$, 3))
100 PRINT INSTR(1 + 1, H$, "l")
110 K = INSTR(H$, N$) + INSTR(7, H$, N$)
120 PRINT K
130 IF INSTR(H$, "world") > 0 THEN PRINT "FOUND" ELSE PRINT "NO"
140 PRINT INSTR(MID$(H$, 7), "hello")
150 FOR I = 1 TO 3 : PRINT INSTR(I, "aXaXaX", "X") : NEXT I
160 PRINT INSTR("", "x"); INSTR("x", "")
