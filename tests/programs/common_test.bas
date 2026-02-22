10 REM COMMON test - verify variables preserved across CHAIN
20 COMMON X, N$
30 X = 42
40 N$ = "hello"
50 Y = 99
60 CHAIN "tests/programs/common_target.bas"
