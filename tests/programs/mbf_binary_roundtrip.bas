10 REM MBF binary save/load round-trip test
20 REM Creates a program with float constants, saves binary, loads back
30 REM Uses a flag file to detect second run and break the loop
40 OPEN "gwbasic_mbfrt_inner.bas" FOR OUTPUT AS #1
50 PRINT #1, "10 PRINT 3.14"
60 PRINT #1, "20 PRINT -100.5"
70 PRINT #1, "30 PRINT 2.71828#"
80 PRINT #1, "40 ON ERROR GOTO 100"
90 PRINT #1, "50 OPEN "+CHR$(34)+"gwbasic_mbfrt_flag"+CHR$(34)+" FOR INPUT AS #1"
100 PRINT #1, "60 CLOSE #1"
110 PRINT #1, "70 KILL "+CHR$(34)+"gwbasic_mbfrt_flag"+CHR$(34)
120 PRINT #1, "80 KILL "+CHR$(34)+"gwbasic_mbfrt_bin.bas"+CHR$(34)
130 PRINT #1, "90 END"
140 PRINT #1, "100 RESUME 110"
150 PRINT #1, "110 OPEN "+CHR$(34)+"gwbasic_mbfrt_flag"+CHR$(34)+" FOR OUTPUT AS #1"
160 PRINT #1, "120 CLOSE #1"
170 PRINT #1, "130 SAVE "+CHR$(34)+"gwbasic_mbfrt_bin.bas"+CHR$(34)
180 PRINT #1, "140 KILL "+CHR$(34)+"gwbasic_mbfrt_inner.bas"+CHR$(34)
190 PRINT #1, "150 LOAD "+CHR$(34)+"gwbasic_mbfrt_bin.bas"+CHR$(34)+",R"
200 CLOSE #1
210 LOAD "gwbasic_mbfrt_inner.bas",R
