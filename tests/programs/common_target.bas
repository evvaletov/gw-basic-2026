10 REM Target for COMMON test
20 IF X = 42 THEN PRINT "X preserved" ELSE PRINT "FAIL: X lost"
30 IF N$ = "hello" THEN PRINT "N$ preserved" ELSE PRINT "FAIL: N$ lost"
40 IF Y = 0 THEN PRINT "Y cleared" ELSE PRINT "FAIL: Y kept"
50 PRINT "COMMON test passed"
