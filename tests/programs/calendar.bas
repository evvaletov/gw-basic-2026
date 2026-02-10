10 REM Day-of-week calculator (Zeller's congruence)
20 DATA 2026,1,1,2026,7,4,2000,1,1,1969,7,20
30 FOR T = 1 TO 4
40 READ YR, MO, DY
50 GOSUB 200
60 PRINT USING "####-##-##  "; YR; MO; DY;
70 PRINT D$
80 NEXT T
90 PRINT "Calendar OK"
100 END
200 REM Zeller's congruence
210 Q = DY : M = MO : K = YR
220 IF M < 3 THEN M = M + 12 : K = K - 1
230 J = INT(K / 100) : KK = K - J * 100
240 H = Q + INT(13*(M+1)/5) + KK + INT(KK/4) + INT(J/4) - 2*J
250 H = H - INT(H / 7) * 7
260 IF H < 0 THEN H = H + 7
270 DIM DN$(6)
280 DN$(0) = "Saturday" : DN$(1) = "Sunday" : DN$(2) = "Monday"
290 DN$(3) = "Tuesday" : DN$(4) = "Wednesday" : DN$(5) = "Thursday" : DN$(6) = "Friday"
300 D$ = DN$(H)
310 ERASE DN$
320 RETURN
