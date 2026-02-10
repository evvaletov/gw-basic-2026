10 REM GOSUB/RETURN test
20 PRINT "Main program"
30 GOSUB 100
40 PRINT "Back from first sub"
50 GOSUB 200
60 PRINT "Back from second sub"
70 END
100 REM First subroutine
110 PRINT "In first subroutine"
120 GOSUB 200
130 PRINT "Back in first sub"
140 RETURN
200 REM Second subroutine
210 PRINT "In second subroutine"
220 RETURN
