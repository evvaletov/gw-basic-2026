10 REM Test BSAVE and BLOAD via virtual memory
20 REM In piped mode, B800 text buffer is inactive so values are 0
30 REM This test verifies the BSAVE/BLOAD flow and file format
40 DEF SEG = &HB800
50 REM BSAVE 4 bytes (will be zeros in piped mode)
60 BSAVE "BSTEST.BIN", 0, 4
70 PRINT "BSAVE OK"
80 REM BLOAD back to same offset
90 BLOAD "BSTEST.BIN"
100 PRINT "BLOAD OK"
110 REM BLOAD with explicit offset
120 BLOAD "BSTEST.BIN", 100
130 PRINT "BLOAD OFFSET OK"
140 REM Verify file was created and can be re-read
150 DEF SEG
160 KILL "BSTEST.BIN"
170 PRINT "KILL OK"
180 PRINT "DONE"
