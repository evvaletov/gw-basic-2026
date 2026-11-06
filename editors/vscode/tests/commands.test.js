const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createCommands } = require('../commands');

function fixture(settings = {}) {
  const document = {
    languageId: 'gwbasic', uri: { scheme: 'file', fsPath: '/second root/games/a $()\' file.bas' },
    isDirty: false, isClosed: false, isUntitled: false,
    async save() { this.isDirty = false; return true; }
  };
  const folder = { uri: { fsPath: '/second root' } };
  const tasks = [], errors = [];
  const api = {
    window: { activeTextEditor: { document }, showErrorMessage: async text => errors.push(text) },
    workspace: {
      isTrusted: true, getWorkspaceFolder: uri => { assert.equal(uri, document.uri); return folder; },
      getConfiguration: (section, uri) => {
        assert.equal(section, 'gwbasic'); assert.equal(uri, document.uri);
        return { get: (name, fallback) => settings[name] === undefined ? fallback : settings[name] };
      }
    },
    Task: class {
      constructor(definition, scope, name, source, execution, problemMatchers) {
        Object.assign(this, { definition, scope, name, source, execution, problemMatchers });
      }
    },
    ProcessExecution: class { constructor(process, args, options) { Object.assign(this, { process, args, options }); } },
    TaskScope: { Global: 1 }, TaskGroup: { Build: 'build' },
    TaskRevealKind: { Always: 1 }, TaskPanelKind: { Dedicated: 2 },
    tasks: { executeTask: async task => { tasks.push(task); return { task }; } }
  };
  return { document, folder, tasks, errors, api, commands: createCommands(api) };
}

test('run waits for saving and retains the original document if the editor changes', async () => {
  const f = fixture({ interpreterPath: './tools/my interpreter' });
  f.document.isDirty = true;
  let finish;
  f.document.save = () => new Promise(resolve => { finish = () => { f.document.isDirty = false; resolve(true); }; });
  const running = f.commands.execute('run');
  assert.equal(f.tasks.length, 0);
  await f.commands.execute('compile'); // duplicate while save is pending
  f.api.window.activeTextEditor = undefined;
  finish();
  await running;
  assert.equal(f.tasks.length, 1);
  assert.deepEqual({ ...f.tasks[0].execution }, {
    process: '/second root/tools/my interpreter',
    args: [f.document.uri.fsPath], options: { cwd: '/second root/games' }
  });
  assert.equal(f.tasks[0].scope, f.folder);
  assert.equal(f.tasks[0].presentationOptions.focus, true);
});

for (const [name, change, message] of [
  ['cancelled save', f => { f.document.isDirty = true; f.document.save = async () => false; }, null],
  ['failed save', f => { f.document.isDirty = true; f.document.save = async () => { throw new Error('disk full'); }; }, /disk full/],
  ['edits during save', f => { f.document.isDirty = true; f.document.save = async () => true; }, /changed during saving/],
  ['closed document', f => { f.document.isClosed = true; }, /changed during saving/],
  ['untitled', f => { f.document.isUntitled = true; }, /Save this/],
  ['virtual document', f => { f.document.uri.scheme = 'git'; }, /filesystem/],
  ['task variable in filename', f => { f.document.uri.fsPath = '/files/${command:danger}.bas'; }, /task variables/],
  ['untrusted workspace', f => { f.api.workspace.isTrusted = false; }, /Trust this workspace/],
  ['wrong language', f => { f.document.languageId = 'javascript'; }, /Open a GW-BASIC/],
  ['no editor', f => { f.api.window.activeTextEditor = undefined; }, /Open a GW-BASIC/]
]) {
  test(`${name} starts no process`, async () => {
    const f = fixture(); change(f); await f.commands.execute('run');
    assert.equal(f.tasks.length, 0);
    if (message) assert.match(f.errors.join('\n'), message);
    else assert.deepEqual(f.errors, []);
  });
}

test('loose files use a global task and their own directory', async () => {
  const f = fixture(); f.api.workspace.getWorkspaceFolder = () => undefined;
  await f.commands.execute('run');
  assert.equal(f.tasks[0].scope, f.api.TaskScope.Global);
  assert.equal(f.tasks[0].execution.process, 'gwbasic');
});

test('compile arguments are literal and explicit runtime overrides detection', async () => {
  const args = ['-c', '--runtime', '/runtime $()/with spaces', '-o', 'my output.c'];
  const f = fixture({ compileArgs: args });
  await f.commands.execute('compile');
  assert.deepEqual(f.tasks[0].execution.args, [...args, f.document.uri.fsPath]);
  assert.equal(args.length, 5);
  assert.equal(f.tasks[0].group, 'build');
  assert.equal(f.tasks[0].presentationOptions.focus, false);
});

test('runtime detection uses configured root, compiler location or owning workspace', async t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'gwbasic-runtime-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  fs.mkdirSync(path.join(root, 'include')); fs.mkdirSync(path.join(root, 'build'));
  fs.writeFileSync(path.join(root, 'include/gwrt.h'), ''); fs.writeFileSync(path.join(root, 'build/libgwrt.a'), '');
  for (const settings of [{ runtimePath: root }, { compilerPath: path.join(root, 'build/gwbasic-compile') }, {}]) {
    const f = fixture(settings);
    f.folder.uri.fsPath = Object.keys(settings).length ? path.join(root, 'unrelated') : root;
    await f.commands.execute('compile');
    assert.equal(f.tasks.length, 1, f.errors.join('\n'));
    assert.deepEqual(f.tasks[0].execution.args, ['-c', '--runtime', root, f.document.uri.fsPath]);
  }
});

test('C-only compilation does not require a native runtime', async () => {
  const f = fixture({ compileArgs: [] }); await f.commands.execute('compile');
  assert.deepEqual(f.tasks[0].execution.args, [f.document.uri.fsPath]);
});

for (const settings of [
  { interpreterPath: '' }, { interpreterPath: 'bad\0path' }, { compilerPath: 17 },
  { interpreterPath: '${env:TOOL}' }, { compileArgs: ['-o', '${input:output}'] },
  { compileArgs: null }, { compileArgs: ['-c', 5] }, { compileArgs: ['--runtime'] },
  { runtimePath: '/missing-gwbasic-runtime' }
]) {
  test(`invalid configuration is actionable: ${JSON.stringify(settings)}`, async () => {
    const f = fixture(settings); await f.commands.execute('interpreterPath' in settings ? 'run' : 'compile');
    assert.equal(f.tasks.length, 0); assert.equal(f.errors.length, 1);
  });
}

test('task launch errors are reported and allow retry', async () => {
  const f = fixture();
  f.api.tasks.executeTask = async () => { throw new Error('executable missing'); };
  await f.commands.execute('run'); await f.commands.execute('run');
  assert.equal(f.errors.length, 2); assert.match(f.errors[0], /executable missing/);
});
