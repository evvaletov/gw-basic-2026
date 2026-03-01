10 REM PRINT USING edge cases
20 REM --- Asterisk fill ---
30 PRINT USING "**###.##"; 1.5
40 PRINT USING "**###.##"; -1.5
50 PRINT USING "**###.##"; 12345.67
60 REM --- Asterisk fill + dollar ---
70 PRINT USING "**$##.##"; 1.5
80 PRINT USING "**$##.##"; -1.5
90 REM --- Dollar sign ---
100 PRINT USING "$$###.##"; 1.5
110 PRINT USING "$$###.##"; -1.5
120 REM --- Thousands separator ---
130 PRINT USING "###,###.##"; 1234.56
140 PRINT USING "###,###.##"; 123456.78
150 PRINT USING "$$#,###.##"; 1234.56
160 REM --- Scientific notation ---
170 PRINT USING "#.##^^^^"; 1234.5
180 PRINT USING "#.##^^^^"; 0.0001
190 PRINT USING "#.##^^^^"; -0.0001
200 PRINT USING "#.##^^^^"; 0
210 REM --- Trailing signs ---
220 PRINT USING "###.##-"; 123.45
230 PRINT USING "###.##-"; -123.45
240 PRINT USING "###.##+"; 123.45
250 PRINT USING "###.##+"; -123.45
260 PRINT USING "+###.##"; 123.45
270 PRINT USING "+###.##"; -123.45
280 REM --- Overflow ---
290 PRINT USING "##.##"; 123.45
300 REM --- Zero ---
310 PRINT USING "###.##"; 0
320 PRINT "DONE"
