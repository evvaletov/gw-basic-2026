10 REM Test DEF SEG / PEEK / POKE
20 REM --- Default segment: PEEK returns 0 ---
30 PRINT "PEEK(0)="; PEEK(0)
40 REM --- BIOS data area: video mode ---
50 DEF SEG = &H40
60 PRINT "VIDEO MODE="; PEEK(&H49)
70 REM --- BIOS data area: columns ---
80 PRINT "COLUMNS="; PEEK(&H4A)
90 REM --- BIOS data area: timer ticks (low byte, just check it runs) ---
100 T! = PEEK(&H6C)
110 IF T! >= 0 THEN PRINT "TIMER TICKS OK" ELSE PRINT "TIMER TICKS FAIL"
120 REM --- CGA text buffer: POKE a character (no TUI in piped mode) ---
130 DEF SEG = &HB800
140 POKE 0, 65
150 REM In piped mode TUI is not active, so PEEK returns 0
160 P = PEEK(0)
170 PRINT "B800 PEEK="; P
180 REM --- DEF SEG reset ---
190 DEF SEG
200 PRINT "RESET PEEK="; PEEK(0)
210 REM --- Boundary values ---
220 DEF SEG = 0
230 PRINT "SEG 0 PEEK="; PEEK(0)
240 DEF SEG = 65535
250 PRINT "SEG FFFF PEEK="; PEEK(0)
260 REM --- Done ---
270 PRINT "DONE"
