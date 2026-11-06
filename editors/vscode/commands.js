const fs = require('node:fs');
const path = require('node:path');

function literalTaskValue(value) {
  // VS Code resolves task variables even in ProcessExecution. Reject their
  // syntax before it can rewrite a filename or invoke a command/input resolver.
  if (value.includes('${')) {
    throw new Error('VS Code task variables (${...}) are not supported in paths or arguments. Rename the file or use literal settings.');
  }
  return value;
}

// Resolve relative paths in the document's own root of a multi-root workspace.
// A bare executable name is looked up on PATH by ProcessExecution.
function executablePath(value, base, setting) {
  if (typeof value !== 'string' || !value.trim() || value.includes('\0')) {
    throw new Error(`Set ${setting} to an executable name or path.`);
  }
  return literalTaskValue(/[/\\]/.test(value) && !path.isAbsolute(value) ? path.resolve(base, value) : value);
}

function runtimePath(config, binary, base) {
  const configured = config.get('runtimePath', '');
  if (typeof configured !== 'string' || configured.includes('\0')) {
    throw new Error('Set gwbasic.runtimePath to the directory containing include/ and build/.');
  }
  const candidates = configured ? [path.resolve(base, configured)] : [
    ...(path.isAbsolute(binary) ? [path.resolve(path.dirname(binary), '..')] : []), base
  ];
  const found = candidates.find(root => fs.existsSync(path.join(root, 'include', 'gwrt.h')) &&
    fs.existsSync(path.join(root, 'build', 'libgwrt.a')));
  if (!found) {
    throw new Error('Set gwbasic.runtimePath to a built GW-BASIC repository or extracted Linux release (containing include/gwrt.h and build/libgwrt.a).');
  }
  return literalTaskValue(found);
}

function createCommands(vscode) {
  // Avoid racing two asynchronous saves of the same document.
  const pending = new Set();

  async function execute(operation) {
    const document = vscode.window.activeTextEditor?.document;
    if (!document || document.languageId !== 'gwbasic') {
      void vscode.window.showErrorMessage('GW-BASIC: Open a GW-BASIC source file first.');
      return;
    }
    if (pending.has(document)) return;
    pending.add(document);
    try {
      if (!vscode.workspace.isTrusted) {
        throw new Error('Trust this workspace before running or compiling BASIC programs.');
      }
      if (document.isUntitled) {
        throw new Error('Save this GW-BASIC document to disk, then run the command again.');
      }
      if (document.uri.scheme !== 'file') {
        throw new Error('Run and Compile require a file on the extension host filesystem.');
      }
      const file = literalTaskValue(document.uri.fsPath);
      if (document.isDirty && !await document.save()) return;
      if (document.isClosed || document.isDirty) {
        throw new Error('The document changed during saving. Save it and run the command again.');
      }
      const cwd = path.dirname(file);
      const folder = vscode.workspace.getWorkspaceFolder(document.uri);
      const base = folder ? folder.uri.fsPath : cwd;
      const config = vscode.workspace.getConfiguration('gwbasic', document.uri);
      const compiling = operation === 'compile';
      const setting = compiling ? 'compilerPath' : 'interpreterPath';
      const binary = executablePath(config.get(setting, compiling ? 'gwbasic-compile' : 'gwbasic'),
        base, `gwbasic.${setting}`);
      let args = [];
      if (compiling) {
        args = config.get('compileArgs', ['-c']);
        if (!Array.isArray(args) || args.some(arg => typeof arg !== 'string' || arg.includes('\0'))) {
          throw new Error('Set gwbasic.compileArgs to an array of argument strings.');
        }
        args = args.map(literalTaskValue);
        const explicitRuntime = args.indexOf('--runtime');
        if (explicitRuntime >= 0 && (!args[explicitRuntime + 1] || args[explicitRuntime + 1].startsWith('-'))) {
          throw new Error('--runtime needs a directory argument in gwbasic.compileArgs.');
        }
        if ((args.includes('-c') || args.includes('--emit-obj')) && explicitRuntime < 0) {
          args.push('--runtime', runtimePath(config, binary, base));
        }
      }
      args.push(file);
      const task = new vscode.Task(
        { type: 'gwbasic', operation, file }, folder || vscode.TaskScope.Global,
        `${compiling ? 'Compile' : 'Run'} ${path.basename(file)}`, 'GW-BASIC',
        new vscode.ProcessExecution(binary, args, { cwd }), []
      );
      task.presentationOptions = {
        reveal: vscode.TaskRevealKind.Always, focus: !compiling,
        panel: vscode.TaskPanelKind.Dedicated, clear: true
      };
      if (compiling) task.group = vscode.TaskGroup.Build;
      return await vscode.tasks.executeTask(task);
    } catch (error) {
      void vscode.window.showErrorMessage(`GW-BASIC: ${error instanceof Error ? error.message : String(error)}`);
    } finally {
      pending.delete(document);
    }
  }

  return { execute };
}

module.exports = { createCommands };
