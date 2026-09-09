// An isolated panel cannot forward clicks to other host bar widgets. Keep
// their strip click-through, intersecting the worker's mask with its own slot
// and the canvas outside the bar. Never add input the worker did not request.
function barMask(rectangles, bar, width, height) {
  if (!bar || !bar.visible) return rectangles
  var content = {x: 0, y: 0, width: width, height: height}
  if (bar.position === "left" || bar.position === "right") {
    var size = Math.min(width, bar.size)
    content.width -= size
    if (bar.position === "left") content.x = size
  } else {
    var size = Math.min(height, bar.size)
    content.height -= size
    if (bar.position !== "bottom") content.y = size
  }
  var slot = {x: bar.x, y: bar.y, width: bar.width, height: bar.height}
  if (bar.position === "bottom") slot.y += height - bar.size
  if (bar.position === "right") slot.x += width - bar.size
  var result = []
  for (var i = 0; i < rectangles.length; i++) {
    var rect = rectangles[i]
    for (var area of [content, slot]) {
      var x = Math.max(rect.x, area.x)
      var y = Math.max(rect.y, area.y)
      var right = Math.min(rect.x + rect.width, area.x + area.width)
      var bottom = Math.min(rect.y + rect.height, area.y + area.height)
      if (right > x && bottom > y)
        result.push({x: x, y: y, width: right - x, height: bottom - y})
    }
  }
  return result
}

function intersect(rect, area) {
  var x = Math.max(rect.x, area.x), y = Math.max(rect.y, area.y)
  var right = Math.min(rect.x + rect.width, area.x + area.width)
  var bottom = Math.min(rect.y + rect.height, area.y + area.height)
  return right > x && bottom > y ? {x: x, y: y, width: right - x, height: bottom - y} : null
}

function barSlots(bars, width, height) {
  var bounds = {x: 0, y: 0, width: width, height: height}
  var result = []
  for (var bar of bars) {
    if (!bar || !bar.visible) continue
    var slot = {x: bar.x, y: bar.y, width: bar.width, height: bar.height}
    if (bar.position === "bottom") slot.y += height - bar.size
    if (bar.position === "right") slot.x += width - bar.size
    var clipped = intersect(slot, bounds)
    if (clipped) result.push(clipped)
  }
  return result
}

// All slots form a union; applying the single-slot mask repeatedly would erase
// two instances of the same plugin sharing one bar edge.
function barMasks(rectangles, bars, width, height, panelsAllowed) {
  var content = {x: 0, y: 0, width: width, height: height}
  var left = 0, right = 0, top = 0, bottom = 0
  for (var bar of bars) {
    if (!bar || !bar.visible) continue
    if (bar.position === "left") left = Math.max(left, bar.size)
    else if (bar.position === "right") right = Math.max(right, bar.size)
    else if (bar.position === "bottom") bottom = Math.max(bottom, bar.size)
    else top = Math.max(top, bar.size)
  }
  content = {x: left, y: top, width: Math.max(0, width - left - right), height: Math.max(0, height - top - bottom)}
  var areas = barSlots(bars, width, height)
  if (panelsAllowed) areas.push(content)
  var result = []
  for (var rect of rectangles) {
    for (var area of areas) {
      var clipped = intersect(rect, area)
      if (clipped) result.push(clipped)
      if (result.length > 512) return []
    }
  }
  return result
}
