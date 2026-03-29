"""Pygments lexer for GW-BASIC."""

from pygments.lexer import RegexLexer
from pygments.token import (
    Comment, Keyword, Name, Number, Operator, Punctuation, String, Text,
)


class GWBasicLexer(RegexLexer):
    name = 'GW-BASIC'
    aliases = ['gwbasic', 'basic']
    filenames = ['*.bas', '*.BAS']
    mimetypes = ['text/x-basic']

    tokens = {
        'root': [
            # REM comments (to end of line)
            (r"(?i)\bREM\b.*$", Comment.Single),
            # Single-quote comments
            (r"'.*$", Comment.Single),

            # String literals
            (r'"[^"]*"', String.Double),

            # Line numbers at start of line
            (r'^\s*\d+', Name.Label),

            # Hex and octal literals
            (r'&[Hh][0-9A-Fa-f]+', Number.Hex),
            (r'&[Oo]?[0-7]+', Number.Oct),

            # Floating point
            (r'\d+\.\d*([eEdD][+-]?\d+)?[!#]?', Number.Float),
            (r'\d+[eEdD][+-]?\d+[!#]?', Number.Float),

            # Integer
            (r'\d+[%!#]?', Number.Integer),

            # Statements / keywords
            (r'(?i)\b(AUTO|BEEP|BLOAD|BSAVE|CALL|CALLS|CHAIN|CHDIR|CIRCLE'
             r'|CLEAR|CLOSE|CLS|COLOR|COM|COMMON|CONT|DATA|DEF\s+SEG'
             r'|DEF\s+FN|DEFDBL|DEFINT|DEFSNG|DEFSTR|DELETE|DIM|DRAW'
             r'|EDIT|ELSE|END|ENVIRON|ERASE|ERROR|FIELD|FILES|FOR|GET'
             r'|GOSUB|GOTO|IF|INPUT|KEY|KILL|LCOPY|LET|LINE\s+INPUT'
             r'|LINE|LIST|LLIST|LOAD|LOCATE|LPRINT|LSET|MERGE|MKDIR'
             r'|MOTOR|NAME|NEW|NEXT|ON|OPEN|OPTION\s+BASE|OUT|PAINT'
             r'|PALETTE|PLAY|POKE|PRESET|PRINT|PSET|PUT|RANDOMIZE'
             r'|READ|REM|RENUM|RESET|RESTORE|RESUME|RETURN|RMDIR|RSET'
             r'|RUN|SAVE|SCREEN|SHELL|SOUND|STOP|SWAP|SYSTEM|THEN|TIMER'
             r'|TO|TRON|TROFF|USING|VIEW|WAIT|WEND|WHILE|WIDTH|WINDOW'
             r'|WRITE)\b', Keyword),

            # Built-in functions
            (r'(?i)\b(ABS|ASC|ATN|CDBL|CHR\$|CINT|COS|CSNG|CSRLIN'
             r'|CVD|CVI|CVS|DATE\$|EOF|ENVIRON\$|ERDEV\$?|ERL|ERR'
             r'|EXP|FIX|FN|FRE|HEX\$|INKEY\$|INP|INPUT\$|INSTR|INT'
             r'|IOCTL\$|LEFT\$|LEN|LOC|LOF|LOG|LPOS|MID\$|MKD\$'
             r'|MKI\$|MKS\$|OCT\$|PEEK|PEN|PMAP|POINT|POS|RIGHT\$'
             r'|RND|SGN|SIN|SPACE\$|SPC|SQR|STICK|STR\$|STRIG'
             r'|STRING\$|TAB|TAN|TIME\$|TIMER|USR|VAL|VARPTR\$?)\b',
             Name.Builtin),

            # Logical / arithmetic operators as keywords
            (r'(?i)\b(AND|EQV|IMP|MOD|NOT|OR|XOR|STEP)\b', Operator.Word),

            # Operators
            (r'[+\-*/\\^<>=]', Operator),
            (r'<>|<=|>=', Operator),

            # Punctuation
            (r'[(),;:#]', Punctuation),

            # Identifiers (variable names)
            (r'[A-Za-z][A-Za-z0-9]*[%!#$]?', Name),

            # Whitespace
            (r'\s+', Text),
        ],
    }
