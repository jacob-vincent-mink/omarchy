#!/bin/bash

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/base-test.sh"

if [[ -z ${OMARCHY_TEST_PLUGIN_HOST:-} ]]; then
  pass "set OMARCHY_TEST_PLUGIN_HOST to a built runtime for plugin command integration"
  exit 0
fi

run_node_test <<'JS'
const fs = require('fs')
const os = require('os')
const { spawnSync } = require('child_process')
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'omarchy-plugin-review-test-'))
const home = path.join(temp, 'home')
const source = path.join(temp, 'source')
const stubs = path.join(temp, 'stubs')
const store = path.join(temp, 'state')
const installed = path.join(home, '.config/omarchy/plugins/acme.review')
for (const directory of [home, source, stubs]) fs.mkdirSync(directory)
const env = {
  ...process.env, HOME: home, OMARCHY_PATH: root,
  OMARCHY_PLUGIN_HOST: process.env.OMARCHY_TEST_PLUGIN_HOST,
  OMARCHY_PLUGIN_STORE: store, PATH: `${stubs}:${root}/bin:${process.env.PATH}`
}
function run(command, args, success = true) {
  const result = spawnSync(command, args, { env, encoding: 'utf8', timeout: 10000 })
  if (success && result.status !== 0) throw new Error(`${command}: ${result.stderr}\n${result.stdout}`)
  if (!success && result.status === 0) throw new Error(`${command} unexpectedly succeeded`)
  return result.stdout
}
try {
  const shellSource = fs.readFileSync(path.join(root, 'shell/shell.qml'), 'utf8')
  function method(name, closing) {
    const start = shellSource.indexOf(`function ${name}(`)
    return shellSource.slice(start, shellSource.indexOf(closing, start) + closing.length).replace(/: string/g, '')
  }
  const scope = {
    Util: { canonicalWidgetId: id => id, isPlainObject: value => value && typeof value === 'object' && !Array.isArray(value) },
    shellConfig: { plugins: [{ id: 'acme.review', sandbox: true, old: true }, { id: 'other', untouched: true }], bar: { layout: { right: [{ id: 'acme.review', type: 'command', exec: 'untouched' }] } } },
    sandboxedPlugins: { status: () => ({ state: 'running' }) },
    persistShellConfig: config => { scope.shellConfig = config }
  }
  scope.shell = scope
  const vm = require('vm')
  vm.createContext(scope)
  vm.runInContext(method('saveSandboxSettings', '\n    }'), scope)
  for (const value of ['[]', 'null', '{"sandbox":false}', '{"id":"other"}', '{"__proto__":{}}', '{"constructor":{}}', '{"prototype":{}}']) {
    assertEqual(scope.saveSandboxSettings('acme.review', value), 'invalid settings', 'host rejects structural or non-object settings: ' + value)
  }
  assertEqual(scope.saveSandboxSettings('other', '{}'), 'plugin is not active', 'host rejects entries without the sandbox marker')
  assertEqual(scope.saveSandboxSettings('acme.review', '{"width":80}'), 'ok', 'active plugin can save its own settings')
  assertDeepEqual(scope.shellConfig.plugins, [{ id: 'acme.review', sandbox: true, old: true, width: 80 }, { id: 'other', untouched: true }], 'host preserves unrelated settings, identity, sandbox marker and other entries')
  assertEqual(scope.saveSandboxSettings('acme.review', '{"type":"command","exec":"malicious"}'), 'ok', 'worker strings remain inert settings in its own sandbox entry')
  assertDeepEqual(scope.shellConfig.bar.layout.right, [{ id: 'acme.review', type: 'command', exec: 'untouched' }], 'sandbox settings cannot change a same-id legacy bar command or QML entry')
  scope.sandboxedPlugins.status = () => ({ state: 'disabled' })
  assertEqual(scope.saveSandboxSettings('acme.review', '{}'), 'plugin is not active', 'host rejects a save after deactivation')

  fs.writeFileSync(path.join(stubs, 'omarchy-shell'), '#!/bin/bash\nif [[ $* == "shell listPlugins" ]]; then echo "[]"; else echo "ok"; fi\n', { mode: 0o755 })
  fs.writeFileSync(path.join(source, 'manifest.json'), JSON.stringify({
    schemaVersion: 1, id: 'acme.review', name: 'Review fixture', version: '1', kinds: ['panel'],
    entryPoints: { panel: 'worker.qml' },
    sandbox: { version: 1, entryPoint: 'worker.qml', requests: { network: true, notifications: true, settings: {read: ['width'], write: ['width']}, filesystem: [{name: 'notes'}] } }
  }))
  fs.writeFileSync(path.join(source, 'worker.qml'), 'import Quickshell\nShellRoot {}\n')
  run('git', ['-C', source, 'init', '-q'])
  run('git', ['-C', source, 'add', '.'])
  run('git', ['-C', source, '-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-qm', 'Fixture'])
  run('omarchy-plugin-add', [source, '--yes'])
  assert(fs.existsSync(path.join(installed, '.git')), 'existing plugin add owns the sandbox Git checkout')
  run('omarchy-plugin-disable', ['acme.review'])
  assert(!fs.existsSync(store), 'disabling a never-reviewed plugin creates no store')
  run('omarchy-plugin-review', ['acme.review', '--ui'])
  assert(!fs.existsSync(store), 'opening the reviewer does not import or approve before its own command runs')
  const review = JSON.parse(run('omarchy-plugin-review', ['acme.review', '--json']))
  assertEqual(review.id, 'acme.review', 'review selects the installed catalog identity')
  assertEqual(review.requests.network, true, 'review shows requested network access')
  assert(!fs.existsSync(path.join(store, 'acme.review.json')), 'review creates no approval')
  run('omarchy-plugin-disable', ['acme.review'])
  assert(!fs.existsSync(path.join(store, 'acme.review.json')), 'disabling a reviewed plugin creates no approval')
  assert(run('omarchy-plugin-review', ['acme.review']).includes('No plugin code was run'), 'human review explains the snapshot boundary')
  run('omarchy-plugin-approve', ['acme.review', '--revision', review.revision], false)
  run('omarchy-plugin-approve', ['acme.review', '--revision', review.revision, '--read', `notes=${source}`, '--allow-notifications', '--read-setting', 'width', '--write-setting', 'width', '--yes'])
  let record = JSON.parse(fs.readFileSync(path.join(store, 'acme.review.json')))
  assertEqual(record.grants.network, false, 'approval does not infer requested network access')
  assertEqual(record.grants.notifications, true, 'approval records the selected notification grant')
  assertDeepEqual(record.grants.settings, {read: ['width'], write: ['width']},  'approval records only explicitly selected own-settings access')
  assertEqual(record.grants.filesystem.notes.path, source, 'approval records the selected folder')
  assertEqual(record.activeUnit, null, 'approval does not start a plugin')
  let listed = JSON.parse(run('omarchy-plugin-list', ['--json'])).find(row => row.id === 'acme.review')
  assertEqual(listed.approved, true, 'plugin list exposes exact-revision approval')
  assertEqual(listed.enabled, false, 'plugin list does not confuse approval with activation')
  assert(run('omarchy-plugin-list', []).includes('approved'), 'human list distinguishes approved from enabled')
  fs.appendFileSync(path.join(source, 'worker.qml'), '// New revision\n')
  run('git', ['-C', source, 'add', '.'])
  run('git', ['-C', source, '-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-qm', 'Update fixture'])
  assert(run('omarchy-plugin-update', ['acme.review', '--yes']).includes('grants are unchanged'), 'existing update explains re-review')
  const updated = JSON.parse(run('omarchy-plugin-review', ['acme.review', '--json']))
  assert(updated.revision !== review.revision, 'changed checkout receives a different review digest')
  record = JSON.parse(fs.readFileSync(path.join(store, 'acme.review.json')))
  assertEqual(record.revision, review.revision, 'reviewing an update preserves the old approval')
  run('omarchy-plugin-disable', ['acme.review'])
  record = JSON.parse(fs.readFileSync(path.join(store, 'acme.review.json')))
  assertEqual(record.enabled, false, 'existing plugin disable revokes sandbox admission')
  run('omarchy-plugin-approve', ['acme.review', '--revision', updated.revision, '--yes'])
  record = JSON.parse(fs.readFileSync(path.join(store, 'acme.review.json')))
  assertEqual(record.revision, updated.revision, 'explicit reapproval selects the updated snapshot')
  assertEqual(record.grants.notifications, false, 'reapproval does not silently carry old grants forward')
  assertDeepEqual(record.grants.settings, {read: [], write: []},  'reapproval does not retain own-settings access implicitly')
  fs.writeFileSync(path.join(stubs, 'omarchy-shell'), '#!/bin/bash\nif [[ $1 == "-q" ]]; then exit 0; else exit 1; fi\n', { mode: 0o755 })
  listed = JSON.parse(run('omarchy-plugin-list', ['--json'])).find(row => row.id === 'acme.review')
  assertEqual(listed.approved, true, 'approval remains visible when the shell is offline')
  assert(run('omarchy-plugin-remove', ['acme.review', '--yes']).includes('approval revoked'), 'sandbox removal works when the shell is offline')
  assert(!fs.existsSync(installed), 'remove deletes the selected disposable checkout')
  record = JSON.parse(fs.readFileSync(path.join(store, 'acme.review.json')))
  assertEqual(record.enabled, false, 'remove revokes approval even without a legacy enabled entry')
  assert(fs.existsSync(path.join(store, 'revisions', updated.revision)), 'remove preserves reviewed snapshots')
} finally {
  fs.rmSync(temp, { recursive: true, force: true })
}
JS
