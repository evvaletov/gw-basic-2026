10 REM Caesar cipher - encode and decode
20 MG$ = "HELLO WORLD"
30 SH% = 13
40 GOSUB 200
50 PRINT "Encoded: "; R$
60 EN$ = R$
70 MG$ = EN$ : SH% = -13
80 GOSUB 200
90 PRINT "Decoded: "; R$
100 IF R$ = "HELLO WORLD" THEN PRINT "Caesar cipher OK" ELSE PRINT "FAIL"
110 END
200 REM Encode MG$ by SH% into R$
210 R$ = ""
220 FOR I = 1 TO LEN(MG$)
230 C% = ASC(MID$(MG$, I, 1))
240 IF C% >= 65 AND C% <= 90 THEN C% = (C% - 65 + SH% + 26) - INT((C% - 65 + SH% + 26) / 26) * 26 + 65
250 R$ = R$ + CHR$(C%)
260 NEXT I
270 RETURN
