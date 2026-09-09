#!/bin/bash

set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/base-test.sh"

run_node_test <<'JS'
const fs = require('fs')
const vm = require('vm')
const scope = vm.createContext({})
vm.runInContext(fs.readFileSync(path.join(root, 'shell/services/PluginInput.js'), 'utf8'), scope)
const full = [{x: 0, y: 0, width: 800, height: 500}]
const contains = (rects, x, y) => rects.some(r => x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height)
for (const position of ['top', 'bottom', 'left', 'right']) {
  const vertical = position === 'left' || position === 'right'
  const bar = {position, visible: true, size: 26, x: vertical ? 0 : 100, y: vertical ? 100 : 0,
    width: vertical ? 26 : 40, height: vertical ? 40 : 26}
  const edge = position === 'bottom' ? 487 : position === 'right' ? 787 : 13
  const point = along => vertical ? [edge, along] : [along, edge]
  const mask = scope.barMask(full, bar, 800, 500)
  assert(contains(mask, ...point(120)), position + ' preserves the own slot')
  assert(!contains(mask, ...point(80)) && !contains(mask, ...point(160)), position + ' leaves neighbors click-through')
  assert(contains(mask, 400, 250), position + ' preserves panel and dismissal input')
  assertEqual(scope.barMask([], bar, 800, 500).length, 0, position + ' never invents input')
  const partial = [{x: 400, y: 240, width: 20, height: 20}]
  assertDeepEqual(JSON.parse(JSON.stringify(scope.barMask(partial, bar, 800, 500))), partial, position + ' preserves sparse masks')
  assertDeepEqual(scope.barMask(full, {...bar, visible: false}, 800, 500), full, position + ' releases a hidden bar strip')
}
assertDeepEqual(scope.barMask(full, null, 800, 500), full, 'unplaced workers keep their mask')
pass('host bar input isolation on all four edges')
JS
