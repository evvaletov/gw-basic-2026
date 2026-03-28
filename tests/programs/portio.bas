10 REM Test OUT/INP/WAIT/MOTOR port I/O
20 REM --- INP from unhandled port returns 0xFF (floating bus)
30 PRINT "INP(0)="; INP(0)
40 REM --- INP from game port returns 0xF0
50 PRINT "INP(&H201)="; INP(&H201)
60 REM --- OUT to unhandled port does not crash
70 OUT 0, 0
80 REM --- COM1 LSR reports transmitter ready
90 PRINT "COM1 LSR="; INP(&H3FD)
100 REM --- PPI port B read/write
110 OUT &H61, 0
120 PRINT "PPI="; INP(&H61)
130 REM --- CGA mode register
140 OUT &H3D8, 0
150 PRINT "CGA MODE="; INP(&H3D8)
160 REM --- MOTOR is silently accepted
170 MOTOR
180 MOTOR 0
190 MOTOR 1
200 PRINT "MOTOR OK"
210 REM --- STICK and STRIG return defaults
220 PRINT "STICK(0)="; STICK(0)
230 PRINT "STRIG(0)="; STRIG(0)
240 PRINT "DONE"
250 SYSTEM
