10 REM ON ERROR GOTO / RESUME test
20 ON ERROR GOTO 100
30 PRINT "Before error"
40 ERROR 5
50 PRINT "After resume"
60 PRINT "Triggering div by zero"
70 X = 1 / 0
80 PRINT "After second resume"
90 PRINT "Error handler OK" : END
100 PRINT "Caught error"; ERR
110 RESUME NEXT
