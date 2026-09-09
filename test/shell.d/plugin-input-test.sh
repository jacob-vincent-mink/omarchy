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
const bars = [
  {position: 'top', visible: true, size: 26, x: 100, y: 0, width: 40, height: 26},
  {position: 'top', visible: true, size: 26, x: 200, y: 0, width: 40, height: 26},
  {position: 'right', visible: true, size: 26, x: 0, y: 100, width: 26, height: 40},
  {position: 'bottom', visible: false, size: 26, x: 300, y: 0, width: 40, height: 26}
]
for (const allowed of [true, false]) {
  const masks = scope.barMasks(full, bars, 800, 500, allowed)
  assert(contains(masks, 120, 13) && contains(masks, 220, 13) && contains(masks, 787, 120), 'multiple own placements form a union')
  assert(!contains(masks, 170, 13) && !contains(masks, 787, 170), 'all neighboring bar slots remain click-through')
  assertEqual(contains(masks, 400, 250), allowed, 'non-owner output suppresses panel input unless roaming is approved')
  assertEqual(scope.barMasks([], bars, 800, 500, allowed).length, 0, 'multiple slots cannot invent worker input')
}
assertEqual(scope.barMasks(full, [], 800, 500, false).length, 0, 'unplaced non-owner output has no input')
assertDeepEqual(JSON.parse(JSON.stringify(scope.barMasks(full, [], 800, 500, true))), full, 'unplaced approved output preserves worker input')
const outside = [{...bars[0], x: -10, width: 20}]
assertDeepEqual(JSON.parse(JSON.stringify(scope.barSlots(outside, 800, 500))), [{x: 0, y: 0, width: 10, height: 26}], 'slots clip to output bounds')
assertEqual(scope.barMasks(Array(513).fill(full[0]), [], 800, 500, true).length, 0, 'mask complexity fails closed')
JS
