10 REM Mini text adventure (deterministic, no INPUT)
20 REM Uses DATA to simulate player choices
30 DATA 1,2,1,1
40 ROOM% = 1 : TURNS% = 0
50 DIM R$(3)
60 R$(1) = "Dark cave" : R$(2) = "Forest path" : R$(3) = "Treasure room"
70 GOSUB 200
80 IF ROOM% = 3 THEN PRINT "You found the treasure!" : GOTO 150
90 READ CH%
100 TURNS% = TURNS% + 1
110 ON CH% GOSUB 300, 400
120 GOTO 70
150 PRINT "Turns taken:"; TURNS%
160 PRINT "Text adventure OK"
170 END
200 REM Print room
210 PRINT "Room: "; R$(ROOM%)
220 RETURN
300 REM Go north
310 IF ROOM% = 1 THEN ROOM% = 2 : RETURN
320 IF ROOM% = 2 THEN ROOM% = 3 : RETURN
330 RETURN
400 REM Go south
410 IF ROOM% = 2 THEN ROOM% = 1 : RETURN
420 IF ROOM% = 3 THEN ROOM% = 2 : RETURN
430 RETURN
