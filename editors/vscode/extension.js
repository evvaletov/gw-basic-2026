const vscode = require('vscode');
const { createCommands } = require('./commands');

function activate(context) {
  const commands = createCommands(vscode);
  for (const operation of ['run', 'compile']) {
    context.subscriptions.push(vscode.commands.registerCommand(
      `gwbasic.${operation}`, () => commands.execute(operation)
    ));
  }
}

module.exports = { activate };
