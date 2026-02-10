10 REM 3x3 Matrix multiplication
20 DIM A(2,2), B(2,2), C(2,2)
30 REM Matrix A (identity-like)
40 DATA 1,2,3,4,5,6,7,8,9
50 REM Matrix B
60 DATA 9,8,7,6,5,4,3,2,1
70 FOR I = 0 TO 2 : FOR J = 0 TO 2 : READ A(I,J) : NEXT J : NEXT I
80 FOR I = 0 TO 2 : FOR J = 0 TO 2 : READ B(I,J) : NEXT J : NEXT I
90 REM Multiply
100 FOR I = 0 TO 2
110 FOR J = 0 TO 2
120 C(I,J) = 0
130 FOR K = 0 TO 2
140 C(I,J) = C(I,J) + A(I,K) * B(K,J)
150 NEXT K
160 NEXT J
170 NEXT I
180 REM Print result
190 FOR I = 0 TO 2
200 PRINT USING "####"; C(I,0); C(I,1); C(I,2)
210 NEXT I
220 PRINT "Matrix mult OK"
