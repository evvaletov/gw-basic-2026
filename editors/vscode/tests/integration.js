const vscode = require('vscode');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

async function task(command) {
  let subscription, timer, execution;
  const finished = new Promise(resolve => {
    subscription = vscode.tasks.onDidEndTaskProcess(event => {
      if (event.execution.task.definition.type === 'gwbasic') resolve(event);
    });
  });
  const deadline = new Promise((resolve, reject) => {
    timer = setTimeout(() => reject(new Error(`${command} did not finish`)), 45000);
  });
  try {
    return await Promise.race([deadline, (async () => {
      execution = await vscode.commands.executeCommand(command);
      assert.ok(execution, `${command} did not start a task`);
      const event = await finished;
      assert.equal(event.exitCode, 0, `${command} failed`);
      return execution.task;
    })()]);
  } catch (error) {
    execution?.terminate();
    throw error;
  } finally { clearTimeout(timer); subscription.dispose(); }
}

async function run() {
  const root = process.env.GWBASIC_TEST_ROOT;
  const repo = process.env.GWBASIC_TEST_REPO;
  const workspace = path.join(root, 'workspace');
  const programs = path.join(workspace, "programs ' $() ;");
  fs.mkdirSync(programs);
  const tools = path.join(root, "tools ' $() ;"); fs.mkdirSync(tools);
  for (const binary of ['gwbasic', 'gwbasic-compile']) {
    fs.symlinkSync(path.join(repo, 'build', binary), path.join(tools, binary));
  }
  const filename = path.join(programs, '$(touch INJECTED).bas');
  fs.writeFileSync(filename, '10 SYSTEM\n');
  const document = await vscode.workspace.openTextDocument(filename);
  assert.equal(document.languageId, 'gwbasic', '.bas language association');
  const editor = await vscode.window.showTextDocument(document);
  const extension = vscode.extensions.getExtension('evvaletov.gwbasic-2026');
  assert.ok(extension); await extension.activate();
  const config = vscode.workspace.getConfiguration('gwbasic', document.uri);
  for (const [key, value] of Object.entries({
    interpreterPath: path.join(tools, 'gwbasic'), compilerPath: path.join(tools, 'gwbasic-compile'),
    runtimePath: repo, compileArgs: ['-c', '--safe']
  })) await config.update(key, value, vscode.ConfigurationTarget.WorkspaceFolder);
  // A source-relative file distinguishes the task cwd from the workspace root.
  fs.writeFileSync(path.join(programs, 'input.txt'), 'INPUT OK\n');
  const program = '10 OPEN "input.txt" FOR INPUT AS #1\n20 LINE INPUT #1,A$:CLOSE #1\n' +
    '30 OPEN "result.txt" FOR OUTPUT AS #1\n40 PRINT #1,A$:CLOSE #1\n50 PRINT "COMPILED OK"\n60 SYSTEM\n';
  await editor.edit(edit => edit.replace(new vscode.Range(0, 0, document.lineCount, 0), program));
  assert.equal(document.isDirty, true);
  const executed = await task('gwbasic.run');
  assert.ok(executed.execution instanceof vscode.ProcessExecution);
  assert.equal(document.isDirty, false);
  assert.equal(fs.readFileSync(filename, 'utf8'), program);
  assert.equal(fs.readFileSync(path.join(programs, 'result.txt'), 'utf8').trim(), 'INPUT OK');
  await task('gwbasic.compile');
  const output = filename.slice(0, -4);
  assert.match(execFileSync(output, { cwd: programs, encoding: 'utf8', timeout: 5000 }), /COMPILED OK/);
  assert.equal(fs.existsSync(path.join(programs, 'INJECTED')), false);
  // Exercise the contributed snippet using VS Code's own snippet parser.
  const scratch = await vscode.workspace.openTextDocument({ language: 'gwbasic' });
  const snippetEditor = await vscode.window.showTextDocument(scratch);
  const snippets = JSON.parse(fs.readFileSync(path.join(extension.extensionPath, 'snippets/gwbasic.json'), 'utf8'));
  await snippetEditor.insertSnippet(new vscode.SnippetString(snippets['Input prompt'].body.join('\n')));
  assert.match(scratch.getText(), /10 INPUT "Prompt"; A\$/);
  await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
  // No-workspace-file scope uses a global task, even while another root is open.
  const outside = path.join(root, 'outside.bas'); fs.writeFileSync(outside, '10 SYSTEM\n');
  const loose = await vscode.workspace.openTextDocument(outside);
  await vscode.window.showTextDocument(loose);
  await vscode.workspace.getConfiguration('gwbasic').update('interpreterPath', path.join(tools, 'gwbasic'), vscode.ConfigurationTarget.Global);
  const looseTask = await task('gwbasic.run');
  assert.equal(looseTask.scope, vscode.TaskScope.Global);
  const variableFile = path.join(root, '${command:gwbasic.testShouldNotRun}.bas');
  fs.writeFileSync(variableFile, '10 SYSTEM\n');
  let invoked = false, started = false;
  const command = vscode.commands.registerCommand('gwbasic.testShouldNotRun', () => { invoked = true; return 'BAD'; });
  const listener = vscode.tasks.onDidStartTask(() => { started = true; });
  try {
    await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(variableFile));
    assert.equal(await vscode.commands.executeCommand('gwbasic.run'), undefined);
    assert.equal(invoked, false, 'filename invoked a task variable command');
    assert.equal(started, false, 'variable filename launched a task');
  } finally { command.dispose(); listener.dispose(); }
  console.log('PASS: VSIX activation, saved run, native compile, literal paths, source cwd, snippets, loose files, task-variable guard');
}

module.exports = { run };
