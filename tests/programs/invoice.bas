10 REM Invoice with PRINT USING formatting
20 PRINT "================================"
30 PRINT "         INVOICE"
40 PRINT "================================"
50 PRINT ""
60 DATA "Widget A",12,4.99
70 DATA "Widget B",5,12.50
80 DATA "Gizmo C",3,29.99
90 DATA "Part D",100,0.75
100 DATA "END",0,0
110 TOTAL = 0
120 PRINT "Item          Qty    Price     Amount"
130 PRINT "------------- ---  -------  ---------"
140 READ ITEM$, QTY%, PRICE
150 IF ITEM$ = "END" THEN 190
160 AMT = QTY% * PRICE
170 TOTAL = TOTAL + AMT
180 PRINT USING "\             \###  $$#,###.##  $$#,###.##"; ITEM$; QTY%; PRICE; AMT
185 GOTO 140
190 PRINT "                             ---------"
200 PRINT USING "                    Total:  $$#,###.##"; TOTAL
210 PRINT ""
220 PRINT "Invoice OK"
