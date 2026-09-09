#!/bin/bash

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/base-test.sh"

if [[ -z ${OMARCHY_TEST_PLUGIN_HOST:-} ]]; then
  pass "set OMARCHY_TEST_PLUGIN_HOST to test explicit filesystem grant selection"
  exit 0
fi

run_node_test <<'JS'
const fs = require('fs')
const os = require('os')
const vm = require('vm')
const { spawnSync } = require('child_process')
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'omarchy-grant-selection-'))
const home = path.join(temp, 'home')
const plugin = path.join(home, '.config/omarchy/plugins/test.selection')
const selected = path.join(temp, 'selected data')
fs.mkdirSync(plugin, { recursive: true })
fs.mkdirSync(selected)
fs.writeFileSync(path.join(selected, 'unchanged.txt'), 'original')
fs.writeFileSync(path.join(plugin, 'worker.qml'), 'import Quickshell\nShellRoot {}\n')
fs.writeFileSync(path.join(plugin, 'manifest.json'), JSON.stringify({
  schemaVersion: 1, id: 'test.selection', name: 'Selection', version: '1', kinds: ['panel'],
  entryPoints: { panel: 'worker.qml' },
  sandbox: { version: 1, entryPoint: 'worker.qml', requests: {
    filesystem: [{ name: 'data', access: 'readwrite', required: true }],
    settings: { read: ['theme'], write: ['volume'], required: true }, openUrls: true, network: true, storage: true,
    exec: { printf: { executable: '/usr/bin/printf', required: [], tree: {next: [
      {arg: {kind: 'exact', value: 'hello'}, then: {end: 'greet'}},
      {arg: {kind: 'exact', value: 'bye'}, then: {end: 'farewell'}}
    ]} } },
    http: {
      catalog: { required: false, scope: {origin: 'https://example.test', method: 'GET', path: '/catalog', query: {limit: {required: true, value: {kind: 'exact', value: '10'}}}}}
    }
  } }
}))
const env = { ...process.env, HOME: home, XDG_CONFIG_HOME: path.join(home, '.config'),
  OMARCHY_PATH: root, OMARCHY_PLUGIN_STORE: path.join(temp, 'store'),
  OMARCHY_PLUGIN_HOST: process.env.OMARCHY_TEST_PLUGIN_HOST, PATH: `${root}/bin:/usr/bin` }
function run(name, args, success = true) {
  const result = spawnSync(name, args, { env, encoding: 'utf8', timeout: 10000 })
  assertEqual(result.status === 0, success, `${name} exit status matches expected outcome`)
  if (success) return result.stdout
  return result.stderr
}
try {
  const review = JSON.parse(run('omarchy-plugin-review', ['test.selection', '--json']))
  const text = run('omarchy-plugin-review', ['test.selection'])
  assert(text.includes('Folder data: read-write, required'), 'CLI review explains required writable access')
  assert(text.includes('Own setting volume: write, required'), 'CLI review includes required settings')
  assert(text.includes('Open HTTP(S) links in browser sessions (optional)'), 'CLI review includes optional web links')
  assert(text.includes('HTTP catalog: GET https://example.test/catalog, optional') && text.includes('"value":"10"'), 'CLI review displays the exact HTTP endpoint and query restriction')
  const args = ['test.selection', '--revision', review.revision, '--yes']
  run('omarchy-plugin-approve', args)
  function record() { return JSON.parse(fs.readFileSync(path.join(temp, 'store/test.selection.json'), 'utf8')) }
  assertEqual(record().grants.storage, false, 'storage defaults to denied')
  assert(text.includes('Private persistent storage; saved data remains after revocation'), 'review explains storage persistence')
  run('omarchy-plugin-approve', args.concat(['--allow-storage']))
  assertEqual(record().grants.storage, true, 'explicit storage selection reaches the native record')
  run('omarchy-plugin-approve', args)
  assertEqual(record().grants.storage, false, 'reapproval does not implicitly retain storage')
  assert(text.includes('Host executable printf: /usr/bin/printf') && text.includes('greet'), 'CLI shows requested executable and tree')
  run('omarchy-plugin-approve', args.concat(['--exec', 'printf:greet']))
  assertDeepEqual(record().grants.exec.printf.selected, ['greet'], 'only the explicitly selected terminal is granted')
  assert(/^[0-9a-f]{64}$/.test(record().grants.exec.printf.executable.digest), 'exec approval binds executable bytes')
  const execPrior = JSON.stringify(record())
  run('omarchy-plugin-approve', args.concat(['--exec', 'printf:other']), false)
  assertEqual(JSON.stringify(record()), execPrior, 'unknown terminal cannot replace prior approval')
  run('omarchy-plugin-approve', args)
  assertDeepEqual(record().grants.exec, {}, 'reapproval without exec selections drops previous command access')
  assertDeepEqual(record().grants.filesystem, {}, 'required requests do not silently become selected grants')
  run('omarchy-plugin-approve', args.concat(['--read', `data=${selected}`]))
  assertEqual(record().grants.filesystem.data.access, 'read', 'read-only selection never grants writes')
  run('omarchy-plugin-approve', args.concat(['--write', `data=${selected}`]))
  assertEqual(record().grants.filesystem.data.access, 'readwrite', 'explicit write selection preserves read-write scope')
  assertEqual(record().grants.filesystem.data.path, selected, 'a path containing spaces stays one literal selected resource')
  assertEqual(record().activeUnit, null, 'approving writable access never starts the plugin')
  run('omarchy-plugin-approve', args.concat(['--read-setting', 'theme', '--write-setting', 'volume']))
  assertDeepEqual(record().grants.settings, {read: ['theme'], write: ['volume']}, 'settings read and write select independent exact keys')
  run('omarchy-plugin-approve', args.concat(['--http', 'catalog']))
  assert(require('node:util').isDeepStrictEqual(record().grants.http.catalog, review.requests.http.catalog.scope), 'HTTP selection copies only the exact reviewed scope')
  assertEqual(record().grants.network, false, 'scoped HTTP never grants direct networking')
  const httpPrior = JSON.stringify(record())
  run('omarchy-plugin-approve', args.concat(['--http', 'other']), false)
  assertEqual(JSON.stringify(record()), httpPrior, 'unrequested HTTP scope cannot replace a prior approval')
  run('omarchy-plugin-approve', args.concat(['--http', 'catalog', '--allow-network']), false)
  assertEqual(JSON.stringify(record()), httpPrior, 'raw networking cannot bypass selected HTTP scope')
  run('omarchy-plugin-approve', args.concat(['--account', 'service=fixture-user']), false)
  assertEqual(JSON.stringify(record()), httpPrior, 'unsupported account option cannot replace a prior approval')
  run('omarchy-plugin-approve', args.concat(['--read-setting', 'theme', '--write-setting', 'volume']))
  assert(!Object.hasOwn(record().grants, 'accounts'), 'saved grants have no application-specific account authority')
  const restored = JSON.stringify(record())
  run('omarchy-plugin-approve', args.concat(['--write-setting', 'theme']), false)
  assertEqual(JSON.stringify(record()), restored, 'read permission cannot be widened to writes')
  run('omarchy-plugin-approve', args.concat(['--allow-settings']), false)
  assertEqual(JSON.stringify(record()), restored, 'removed broad settings flag has no compatibility route')
  run('omarchy-plugin-approve', args.concat(['--read', `data=${selected}`, '--write', `data=${selected}`]), false)
  assertEqual(JSON.stringify(record()), restored, 'ambiguous selections preserve the prior approval')
  assertEqual(fs.readFileSync(path.join(selected, 'unchanged.txt'), 'utf8'), 'original', 'review and approval do not modify selected data')

  const source = fs.readFileSync(path.join(root, 'shell/plugins/panels/plugin-review/Review.qml'), 'utf8')
  const scope = { revision: review, busy: false, pluginId: 'test.selection', network: false, http: [], exec: {},
    notifications: false, settings: {read: [], write: []}, openUrls: false, storage: false, media: '', folders: {data: selected}, writableFolders: {},
    run: (operation, args) => { scope.args = args } }
  vm.createContext(scope)
  for (const name of ['requestLabel', 'setWritable', 'toggleHttp', 'toggleExec', 'approve']) {
    const start = source.indexOf(`  function ${name}(`)
    const end = source.indexOf('\n  }', start) + 4
    vm.runInContext(source.slice(start, end), scope)
  }
  scope.approve()
  assert(scope.args.includes('--read') && !scope.args.includes('--write'), 'reviewer draft defaults to read-only, even for a writable request')
  assert(!scope.args.includes('--allow-storage'), 'reviewer storage defaults to denied')
  scope.storage = true
  scope.approve()
  assert(scope.args.includes('--allow-storage'), 'reviewer storage selection reaches the canonical CLI')
  scope.setWritable('data', true)
  scope.approve()
  assert(scope.args.includes('--write') && !scope.args.includes('--read'), 'reviewer explicitly selected writes reach the canonical command')
  scope.folders = {constructor: selected}
  scope.writableFolders = {}
  scope.approve()
  assert(!scope.args.includes('--write'), 'inherited object properties cannot implicitly select writable access')
  assertEqual(scope.requestLabel('settings', 'Settings'), 'Settings · required', 'reviewer preserves required request labels')
  scope.settings = {read: ['theme'], write: ['volume']}
  scope.approve()
  assert(scope.args.includes('--read-setting') && scope.args.includes('--write-setting'), 'reviewer carries selected read and write keys to the CLI')
  scope.network = true
  scope.toggleHttp('catalog')
  scope.approve()
  assert(scope.args.includes('--http') && scope.args.includes('catalog') && !scope.args.includes('--allow-network'), 'selecting an HTTP scope clears raw network authority and reaches the CLI')
  scope.toggleExec('printf', 'greet')
  scope.approve()
  assert(scope.args.includes('--exec') && scope.args.includes('printf:greet') && !scope.args.includes('printf:farewell'), 'reviewer sends only explicitly selected command leaves')
} finally {
  fs.rmSync(temp, { recursive: true, force: true })
}
JS
