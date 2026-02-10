10 REM WRITE#/INPUT# and EOF test
20 OPEN "gwbasic_wi_test.txt" FOR OUTPUT AS #1
30 WRITE #1, "Alice", 25
40 WRITE #1, "Bob", 30
50 CLOSE #1
60 OPEN "gwbasic_wi_test.txt" FOR INPUT AS #1
70 WHILE NOT EOF(1)
80 INPUT #1, N$, A
90 PRINT N$; A
100 WEND
110 CLOSE #1
120 KILL "gwbasic_wi_test.txt"
