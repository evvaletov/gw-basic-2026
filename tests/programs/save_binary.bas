10 REM Binary SAVE/LOAD round-trip test
20 REM Create a test program, save as binary default, load binary back
30 OPEN "gwbasic_binsrc.bas" FOR OUTPUT AS #1
40 PRINT #1, "10 PRINT "+CHR$(34)+"Binary round-trip ok"+CHR$(34)
50 PRINT #1, "20 SAVE "+CHR$(34)+"gwbasic_roundtrip.bas"+CHR$(34)
60 PRINT #1, "30 KILL "+CHR$(34)+"gwbasic_binsrc.bas"+CHR$(34)
70 PRINT #1, "40 KILL "+CHR$(34)+"gwbasic_roundtrip.bas"+CHR$(34)
80 CLOSE #1
90 LOAD "gwbasic_binsrc.bas",R
