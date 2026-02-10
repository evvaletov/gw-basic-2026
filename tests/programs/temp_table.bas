10 REM Temperature conversion table
20 PRINT "  Celsius  Fahrenheit"
30 PRINT "  -------  ----------"
40 FOR C = -40 TO 100 STEP 20
50 F = C * 9 / 5 + 32
60 PRINT USING "  ###.#      ###.#"; C; F
70 NEXT C
80 PRINT "Temp table OK"
