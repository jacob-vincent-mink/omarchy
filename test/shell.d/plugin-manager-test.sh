#!/bin/bash

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/base-test.sh"

run_node_test <<'JS'
const fs = require('fs')
const vm = require('vm')
const functions = file => [...fs.readFileSync(path.join(root, file), 'utf8').matchAll(/^  function \w+\([^\n]*\) \{[\s\S]*?^  \}/gm)].map(match => match[0]).join('\n')
const c = vm.createContext({
  plugins: [], selected: null, source: 'https://example.test/plugin', yolo: false, trustConfirmed: false,
  inspected: null, selectedId: '', adding: true, confirmRemove: false, busy: false, operation: '', error: '', notice: '',
  command: {}, installed() {}, Qt: {callLater() {}}
})
vm.runInContext(functions('shell/plugins/panels/plugins/Model.qml'), c)
assertEqual(c.inspect(), true, 'source inspection can start without granting trust')
assertDeepEqual(c.command.command, ['omarchy-plugin-add', c.source, '--inspect', '--json'], 'inspection neither installs nor enables')
assertEqual(c.inspect(), false, 'UI never overlaps installation commands')
c.finish(0, JSON.stringify({id:'test.demo', commit:'a'.repeat(40), installed:false}), '')
assertEqual(c.inspected.id, 'test.demo', 'validated revision is shown')
c.yolo = true
assertEqual(c.add(), false, 'YOLO requires separate explicit trust confirmation')
c.trustConfirmed = true
assertEqual(c.add(), true, 'confirmed YOLO can be installed')
assertDeepEqual(c.command.command, ['omarchy-plugin-add', c.source, '--commit', 'a'.repeat(40), '--json', '--yes', '--yolo'], 'install pins the inspected commit and explicitly selects YOLO')
c.finish(1, '', 'source changed since validation')
assertEqual(c.error, 'source changed since validation', 'failed installation keeps actionable feedback')
assertEqual(c.adding, true, 'failed installation keeps the form open')
c.selected = {id:'test.demo'}
assertEqual(c.action('remove'), false, 'first remove click only asks for confirmation')
assertEqual(c.action('remove'), true, 'confirmed removal invokes the lifecycle command')
assertDeepEqual(c.command.command, ['omarchy-plugin-remove', 'test.demo', '--yes'], 'removal is scoped to the selected identity')
c.finish(0, 'Removed', '')
assertEqual(c.confirmRemove, false, 'removal confirmation clears after completion')
c.finish(0, 'not-json', '') // Previous remove result does not parse external prose.
c.operation = 'list'
c.plugins = [{id:'kept'}]
c.finish(0, 'not-json', '')
assertEqual(c.plugins[0].id, 'kept', 'invalid list results preserve last good state')

JS
