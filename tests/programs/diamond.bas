10 REM Diamond pattern using STRING$ and SPACE$
20 N% = 7
30 REM Top half
40 FOR I = 1 TO N%
50 PRINT SPACE$(N% - I); STRING$(2 * I - 1, "*")
60 NEXT I
70 REM Bottom half
80 FOR I = N% - 1 TO 1 STEP -1
90 PRINT SPACE$(N% - I); STRING$(2 * I - 1, "*")
100 NEXT I
110 PRINT "Diamond OK"
