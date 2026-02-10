10 REM Number guessing game (self-playing with fixed seed)
20 RANDOMIZE 99
30 SECRET% = INT(RND(1) * 100) + 1
40 REM Play using binary search
50 LO% = 1 : HI% = 100
60 TRIES% = 0
70 WHILE LO% <= HI%
80 GUESS% = INT((LO% + HI%) / 2)
90 TRIES% = TRIES% + 1
100 IF GUESS% = SECRET% THEN PRINT "Found"; SECRET%; "in"; TRIES%; "tries" : GOTO 150
110 IF GUESS% < SECRET% THEN LO% = GUESS% + 1 ELSE HI% = GUESS% - 1
120 WEND
150 IF TRIES% <= 7 THEN PRINT "Number guess OK" ELSE PRINT "Too many tries"
