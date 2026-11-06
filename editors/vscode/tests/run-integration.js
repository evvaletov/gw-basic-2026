// Install the actual VSIX in an isolated profile, then test its packaged code.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { downloadAndUnzipVSCode, resolveCliArgsFromVSCodeExecutablePath, runTests } = require('@vscode/test-electron');

async function main() {
  const root = path.resolve(__dirname, '..');
  const manifest = require('../package.json');
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'gwbasic-vscode-'));
  try {
    const vscodeExecutablePath = process.env.VSCODE_EXECUTABLE_PATH || await downloadAndUnzipVSCode(process.env.VSCODE_TEST_VERSION || 'stable');
    // Supply our own temporary profile instead of the helper's cached profile.
    const [cli, ...cliArgs] = resolveCliArgsFromVSCodeExecutablePath(vscodeExecutablePath, { reuseMachineInstall: true });
    const profile = ['--user-data-dir', path.join(temporary, 'profile'), '--extensions-dir', path.join(temporary, 'extensions')];
    const vsix = path.join(root, 'dist', `${manifest.name}-${manifest.version}.vsix`);
    const install = spawnSync(cli, [...cliArgs, ...profile, '--install-extension', vsix, '--force'], {
      encoding: 'utf8', timeout: 60000, env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' }
    });
    process.stdout.write(install.stdout || ''); process.stderr.write(install.stderr || '');
    if (install.error || install.status !== 0) throw install.error || new Error(`VSIX installation failed: ${install.status}`);
    const extension = path.join(temporary, 'extensions', `${manifest.publisher}.${manifest.name}-${manifest.version}`);
    if (!fs.existsSync(path.join(extension, 'commands.js'))) throw new Error('Installed extension is missing commands.js');
    if (fs.existsSync(path.join(extension, 'tests'))) throw new Error('Tests leaked into the VSIX');
    const workspace = path.join(temporary, 'workspace'); fs.mkdirSync(workspace);
    fs.mkdirSync(path.join(workspace, '.vscode'));
    fs.writeFileSync(path.join(workspace, '.vscode/settings.json'), JSON.stringify({
      'files.autoSave': 'off', 'telemetry.telemetryLevel': 'off',
      'terminal.integrated.enablePersistentSessions': false
    }));
    await runTests({
      vscodeExecutablePath, reuseMachineInstall: true, extensionDevelopmentPath: extension,
      extensionTestsPath: path.join(__dirname, 'integration.js'),
      extensionTestsEnv: { GWBASIC_TEST_ROOT: temporary, GWBASIC_TEST_REPO: path.resolve(root, '../..') },
      launchArgs: [workspace, ...profile, '--skip-welcome', '--skip-release-notes',
        '--disable-workspace-trust', '--disable-gpu',
        ...(process.env.GWBASIC_TEST_WAYLAND ? ['--ozone-platform=wayland'] : [])]
    });
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
