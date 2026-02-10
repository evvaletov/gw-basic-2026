10 REM Random-access file I/O test
20 OPEN "R", #1, "/tmp/gwbasic_random.dat", 32
30 FIELD #1, 20 AS N$, 4 AS A$, 8 AS S$
40 LSET N$ = "Alice"
50 LSET A$ = MKI$(25) + "  "
60 LSET S$ = MKD$(50000.5)
70 PUT #1, 1
80 LSET N$ = "Bob"
90 LSET A$ = MKI$(30) + "  "
100 LSET S$ = MKD$(75000.25)
110 PUT #1, 2
120 GET #1, 1
130 PRINT LEFT$(N$, 5); CVI(A$);
140 GET #1, 2
150 PRINT LEFT$(N$, 3); CVI(A$)
160 CLOSE #1
