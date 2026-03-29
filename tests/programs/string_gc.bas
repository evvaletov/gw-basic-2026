10 REM Test string garbage collection
20 REM FRE("") triggers GC and returns free string space
30 F0 = FRE("")
40 IF F0 > 0 THEN PRINT "INITIAL OK" ELSE PRINT "INITIAL FAIL"
50 REM --- Allocate some string variables
60 A$ = STRING$(200, "A")
70 B$ = STRING$(200, "B")
80 C$ = STRING$(200, "C")
90 F1 = FRE("")
100 IF F1 < F0 THEN PRINT "ALLOC OK" ELSE PRINT "ALLOC FAIL"
110 REM --- Overwrite with shorter strings (old data becomes garbage)
120 A$ = "short"
130 B$ = "tiny"
140 C$ = "x"
150 F2 = FRE("")
160 IF F2 > F1 THEN PRINT "GC RECLAIMED" ELSE PRINT "GC FAIL"
170 REM --- Stress test: loop with string concatenation
180 CLEAR 4096
190 D$ = ""
200 FOR I = 1 TO 50
210   D$ = D$ + "X"
220 NEXT I
230 PRINT "CONCAT LEN="; LEN(D$)
240 F3 = FRE("")
250 IF F3 > 0 THEN PRINT "POOL OK" ELSE PRINT "POOL FAIL"
260 REM --- CLEAR with string space size
270 CLEAR 8192
280 F4 = FRE("")
290 IF F4 > 7000 AND F4 < 9000 THEN PRINT "CLEAR SIZE OK" ELSE PRINT "CLEAR SIZE FAIL"
300 PRINT "ALL DONE"
310 SYSTEM
