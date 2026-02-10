10 REM Statistics: mean, variance, std dev
20 N% = 10
30 DIM X(9)
40 DATA 23,45,12,67,34,89,56,78,90,11
50 FOR I = 0 TO 9 : READ X(I) : NEXT I
60 REM Mean
70 S = 0
80 FOR I = 0 TO 9 : S = S + X(I) : NEXT I
90 MEAN = S / N%
100 PRINT USING "Mean:     ###.##"; MEAN
110 REM Variance
120 V = 0
130 FOR I = 0 TO 9 : V = V + (X(I) - MEAN) * (X(I) - MEAN) : NEXT I
140 VR = V / N%
150 PRINT USING "Variance: ####.##"; VR
160 PRINT USING "Std Dev:  ###.##"; SQR(VR)
170 PRINT "Stats calc OK"
