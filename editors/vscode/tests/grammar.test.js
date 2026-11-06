const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const textmate = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

test('TextMate tokenization covers source and stops unterminated strings at EOL', async () => {
  await oniguruma.loadWASM(fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer);
  const registry = new textmate.Registry({
    onigLib: Promise.resolve({
      createOnigScanner: patterns => new oniguruma.OnigScanner(patterns),
      createOnigString: text => new oniguruma.OnigString(text)
    }),
    loadGrammar: async () => JSON.parse(fs.readFileSync(path.join(__dirname, '../syntaxes/gwbasic.tmLanguage.json'), 'utf8'))
  });
  try {
    const grammar = await registry.loadGrammar('source.gwbasic');
    const source = '10 IF A$ = "REM quoted" THEN PRINT &HFF, &O77, 1.2E-3!, .5#, INPUT$(1): REM done';
    const result = grammar.tokenizeLine(source);
    const scope = text => result.tokens.find(token => token.startIndex <= source.indexOf(text) && token.endIndex > source.indexOf(text)).scopes;
    for (const [text, expected] of [
      ['10', 'entity.name.label.gwbasic'], ['IF', 'keyword.control.gwbasic'],
      ['REM quoted', 'string.quoted.double.gwbasic'], ['&HFF', 'constant.numeric.hex.gwbasic'],
      ['&O77', 'constant.numeric.octal.gwbasic'], ['1.2E-3!', 'constant.numeric.gwbasic'],
      ['INPUT$', 'support.function.builtin.gwbasic'], ['REM done', 'comment.line.rem.gwbasic']
    ]) assert.ok(scope(text).includes(expected), `${text}: ${scope(text)}`);
    for (const literal of ['&HFF', '&O77', '1.2E-3!', '.5#']) {
      const start = source.indexOf(literal);
      const token = result.tokens.find(item => item.startIndex === start);
      assert.equal(token?.endIndex, start + literal.length, `full numeric span: ${literal}`);
    }
    const dotted = grammar.tokenizeLine('10 A.REM=1:PRINT A.REM');
    assert.ok(dotted.tokens.every(token => !token.scopes.some(item => item.startsWith('comment.'))));
    assert.ok(dotted.tokens.some(token => token.scopes.includes('keyword.other.gwbasic')));
    const unterminated = grammar.tokenizeLine('20 PRINT "unfinished');
    const next = grammar.tokenizeLine('30 INPUT A$', unterminated.ruleStack);
    assert.ok(next.tokens.some(token => token.scopes.includes('keyword.other.gwbasic')));
    assert.ok(next.tokens.every(token => !token.scopes.includes('string.quoted.double.gwbasic')));
    const comment = grammar.tokenizeLine("40 ' IF PRINT");
    assert.ok(comment.tokens.some(token => token.scopes.includes('comment.line.apostrophe.gwbasic')));
  } finally { registry.dispose(); }
});
