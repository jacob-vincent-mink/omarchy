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
