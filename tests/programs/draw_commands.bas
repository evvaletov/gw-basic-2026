10 REM DRAW command comprehensive test
20 SCREEN 2
30 REM --- Bug 1: M command parsing (absolute) ---
40 DRAW "M100,50"
50 PRINT "M abs:"; POINT(100,50)
60 REM --- Bug 1: M command parsing (relative) ---
70 DRAW "BM10,10 M+20,+10"
80 PRINT "M rel:"; POINT(30,20)
90 REM --- Bug 2: S scale semantics ---
100 REM Default S4: U with no arg moves 1 pixel
110 DRAW "BM50,50 U"
120 PRINT "S4 U:"; POINT(50,49)
130 REM S8: U with no arg moves 2 pixels
140 DRAW "BM50,50 S8 U"
150 PRINT "S8 U:"; POINT(50,48)
160 REM S8: U5 moves 10 pixels (5*8/4=10)
170 DRAW "BM50,50 S8 U5"
180 PRINT "S8 U5:"; POINT(50,40)
190 REM --- Bug 3: A rotation ---
200 DRAW "BM80,80 A1 R10"
210 PRINT "A1 R:"; POINT(80,70)
220 DRAW "A2 BM80,80 R10"
230 PRINT "A2 R:"; POINT(70,80)
240 DRAW "A0"
250 REM --- TA arbitrary rotation ---
260 DRAW "BM60,60 TA90 R10"
270 PRINT "TA90 R:"; POINT(60,50)
280 DRAW "TA0"
290 REM --- =variable; substitution ---
300 D%=20
310 DRAW "BM10,150 R=D%;"
320 PRINT "=var:"; POINT(15,150)
330 REM --- X substring execution ---
340 A$="U10R10D10L10"
350 DRAW "BM120,120"
360 DRAW "XA$;"
370 PRINT "X sub:"; POINT(120,110)
380 PRINT "X sub:"; POINT(130,120)
390 REM --- Scale on relative M ---
400 DRAW "S8 BM50,100 M+10,+0"
410 PRINT "S M+:"; POINT(70,100)
420 DRAW "S4"
430 PRINT "DONE"
