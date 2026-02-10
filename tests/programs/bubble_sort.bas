10 REM Bubble sort with string comparison
20 DIM A$(9)
30 DATA "banana","apple","cherry","date","elderberry"
40 DATA "fig","grape","honeydew","kiwi","lemon"
50 FOR I = 0 TO 9 : READ A$(I) : NEXT I
60 REM Sort
70 FOR I = 0 TO 8
80 FOR J = 0 TO 8 - I
90 IF A$(J) > A$(J+1) THEN SWAP A$(J), A$(J+1)
100 NEXT J
110 NEXT I
120 REM Print sorted
130 FOR I = 0 TO 9 : PRINT A$(I) : NEXT I
140 PRINT "Bubble sort OK"
