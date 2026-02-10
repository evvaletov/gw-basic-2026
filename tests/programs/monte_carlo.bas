10 REM Monte Carlo pi estimation
20 RANDOMIZE 42
30 INSIDE% = 0
40 N% = 10000
50 FOR I = 1 TO N%
60 X = RND(1) * 2 - 1
70 Y = RND(1) * 2 - 1
80 IF X*X + Y*Y <= 1 THEN INSIDE% = INSIDE% + 1
90 NEXT I
100 PE = 4 * INSIDE% / N%
110 PRINT USING "Pi estimate: #.####"; PE
120 PRINT USING "Actual pi:   #.####"; 3.14159265#
130 PRINT "Monte Carlo OK"
