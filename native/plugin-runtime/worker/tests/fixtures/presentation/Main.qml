import QtQuick
import Omarchy.PluginPresentation 1.0

Item {
  objectName: String(Color.foreground) === "#123456"
    && Style.bar.statusSlot === 23 && Style.bar.position === "bottom"
      ? "presentation-themed" : "presentation-loaded"
  width: 320
  height: 240

  BorderSurface {}
  Button {}
  CursorSurface {}
  Dropdown {}
  KeyboardPanel {}
  Panel {}
  PanelActionButton {}
  PanelHero {}
  PanelKeyCatcher {}
  PanelSectionHeader {}
  PanelSeparator {}
  PanelSlider {}
  PrivateStorage {}
  TextField {}
  ToolTip {}
  Toggle {}
  ToggleSwitch {}
}
