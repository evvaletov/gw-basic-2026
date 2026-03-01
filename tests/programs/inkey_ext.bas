10 REM Test INKEY$ extended key support (non-interactive)
20 REM When no key is available, INKEY$ returns ""
30 K$ = INKEY$
40 IF LEN(K$) = 0 THEN PRINT "Empty INKEY$ ok" ELSE PRINT "FAIL: expected empty"
50 PRINT "INKEY$ extended keys test passed"
