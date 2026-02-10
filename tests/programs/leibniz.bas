10 REM Leibniz formula for pi
20 S = 0
30 FOR I = 0 TO 999
40 S = S + (-1)^I / (2*I+1)
50 NEXT I
60 PRINT "Pi approx:"; 4*S
