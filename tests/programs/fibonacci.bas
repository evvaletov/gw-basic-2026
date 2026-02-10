10 REM Fibonacci sequence - double precision
20 A# = 0 : B# = 1
30 FOR I = 1 TO 20
40 PRINT USING "##.  ##########"; I; A#
50 C# = A# + B#
60 A# = B#
70 B# = C#
80 NEXT I
90 PRINT "Fibonacci OK"
