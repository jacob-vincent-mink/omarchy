//! Test-only port packaging. The widget and visual controls are copied from the
//! repository, not reimplemented here. Never add host imports or mounts to make
//! their ambient services work: this bundle uses data-only theme defaults and a
//! worker-local MPRIS adapter instead.
use std::{fs, path::Path};

pub fn package(destination: &Path) {
  let shell = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../shell");
  for directory in ["Ui", "Commons"] {
    fs::create_dir(destination.join(directory)).unwrap();
  }
  for file in [
    "Ui/BarWidget.qml",
    "Ui/Button.qml",
    "Ui/BorderSurface.qml",
    "Ui/BorderOverlay.qml",
    "Ui/PanelSeparator.qml",
    "Commons/Util.qml",
    "Commons/Border.qml",
    "Commons/BorderGeometry.js",
  ] {
    fs::copy(shell.join(file), destination.join(file)).unwrap();
  }
  fs::write(destination.join("Ui/qmldir"), "module qs.Ui\nBarWidget 1.0 BarWidget.qml\nButton 1.0 Button.qml\nBorderSurface 1.0 BorderSurface.qml\nBorderOverlay 1.0 BorderOverlay.qml\nPanelSeparator 1.0 PanelSeparator.qml\nPopupCard 1.0 PopupCard.qml\n").unwrap();
  fs::write(destination.join("Commons/qmldir"),
    "module qs.Commons\nsingleton Color 1.0 Color.qml\nsingleton Style 1.0 Style.qml\nsingleton Border 1.0 Border.qml\nsingleton Util 1.0 Util.qml\n").unwrap();
  fs::copy(
    shell.join("plugins/services/media/BarWidget.qml"),
    destination.join("MediaWidget.qml"),
  )
  .unwrap();
  fs::copy(
    shell.join("plugins/services/media/MediaModel.js"),
    destination.join("MediaModel.js"),
  )
  .unwrap();

  // Keep the actual theme calculations, but not the host theme watchers and
  // compositor probes. Fail at packaging if those source boundaries change.
  let color = fs::read_to_string(shell.join("Commons/Color.qml")).unwrap();
  let color = color.split_once("  // Startup load only.").unwrap().0;
  fs::write(
    destination.join("Commons/Color.qml"),
    format!("{color}}}\n").replace("import Quickshell.Io\n", ""),
  )
  .unwrap();
  let style = fs::read_to_string(shell.join("Commons/Style.qml")).unwrap();
  let pure = style.split_once("  function refresh() {").unwrap().0;
  let apply = style
    .split_once("  function applyShellValues(values) {")
    .unwrap()
    .1
    .split_once("  property Process hyprctlProc:")
    .unwrap()
    .0;
  fs::write(
    destination.join("Commons/Style.qml"),
    format!("{pure}  function applyShellValues(values) {{{apply}}}\n")
      .replace("import Quickshell.Io\n", ""),
  )
  .unwrap();

  // Private xdg-popup dismissal comes from the host-owned compositor, not a
  // HyprlandFocusGrab against the real desktop. Preserve layout and animation.
  let popup = fs::read_to_string(shell.join("Ui/PopupCard.qml")).unwrap();
  let (before, rest) = popup
    .split_once("  // Outside-click dismissal via Hyprland's focus grab.")
    .unwrap();
  let after = rest.split_once("  anchor {").unwrap().1;
  fs::write(
    destination.join("Ui/PopupCard.qml"),
    format!("{before}  onVisibleChanged: if (!visible && open) {{ hostDismissed = true; root.close() }}\n\n  anchor {{{after}")
      .replace("import Quickshell.Hyprland\n", "")
      .replace("  visible: open || card.opacity > 0", "  property bool hostDismissed: false\n  visible: !hostDismissed && (open || card.opacity > 0)")
      .replace("  onOpenChanged: {", "  onOpenChanged: {\n    if (open) hostDismissed = false"),
  )
  .unwrap();
  fs::write(
    destination.join("worker.qml"),
    include_str!("media-widget.qml"),
  )
  .unwrap();
  fs::write(destination.join("cover.svg"), r##"<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#eeb344"/><circle cx="32" cy="32" r="20" fill="#304050"/><circle cx="32" cy="32" r="6" fill="#eeb344"/></svg>"##).unwrap();
}
