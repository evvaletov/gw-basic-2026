10 REM DOS smoke test for gwbasic16.exe -- exercises arithmetic, strings,
20 REM control flow, GOSUB, FOR/NEXT, DATA/READ, file I/O, MID$ assignment.
30 REM Output is captured via OPEN/PRINT# so the host can compare against
40 REM tests/expected/dos_smoke.expected.
50 OPEN "O",#1,"OUT.TXT"
60 REM --- 1. Arithmetic
70 PRINT #1, "ARITH"
80 PRINT #1, 2+2*3
90 PRINT #1, (2+2)*3
100 PRINT #1, 100\3, 100 MOD 3
110 PRINT #1, 2^10
120 REM --- 2. Strings
130 PRINT #1, "STRINGS"
140 A$ = "HELLO" + " " + "WORLD"
150 PRINT #1, A$
160 PRINT #1, LEN(A$); LEFT$(A$,5); RIGHT$(A$,5); MID$(A$,7,5)
170 MID$(A$,7,5) = "BASIC"
180 PRINT #1, A$
190 REM --- 3. Control flow
200 PRINT #1, "CONTROL"
210 FOR I = 1 TO 5
220   PRINT #1, "FOR"; I
230 NEXT I
240 J = 0
250 WHILE J < 3
260   J = J + 1
270   PRINT #1, "WHILE"; J
280 WEND
290 GOSUB 1000
300 REM --- 4. DATA/READ
310 PRINT #1, "DATA"
320 RESTORE 900
330 FOR K = 1 TO 4
340   READ X
350   PRINT #1, "X="; X
360 NEXT K
370 REM --- 5. DEF FN
380 PRINT #1, "DEFFN"
390 DEF FN SQUARE(N) = N*N
400 PRINT #1, FN SQUARE(7)
410 PRINT #1, FN SQUARE(13)
420 REM --- 6. Conditionals
430 PRINT #1, "IF"
440 IF 5 > 3 THEN PRINT #1, "T1"
450 IF 5 < 3 THEN PRINT #1, "F1" ELSE PRINT #1, "T2"
460 PRINT #1, "DONE"
470 CLOSE #1
480 END
900 DATA 10, 20, 30, 40
1000 PRINT #1, "GOSUB OK"
1010 RETURN
