#pragma once
#include "omarchy-plugin-host/src/qt.rs.h"
#include <QQuickItem>
#include <QRegion>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <array>
#include <optional>

// Trusted host component: never load this module into the plugin worker.
class PluginView : public QQuickItem {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
  Q_PROPERTY(bool resizing READ resizing NOTIFY stateChanged)
  Q_PROPERTY(QString error READ error NOTIFY stateChanged)
public:
  explicit PluginView(QQuickItem *parent = nullptr);
  bool ready() const { return m_ready; }
  bool resizing() const { return m_resizing; }
  QString error() const { return m_error; }
  bool contains(const QPointF &) const override;
  Q_INVOKABLE void start(const QString &store, const QString &id, const QString &controller,
    int logicalWidth, int logicalHeight, int scale = 1);
  Q_INVOKABLE void stop();
  Q_INVOKABLE void configure(int logicalWidth, int logicalHeight, int scale = 1);
  Q_INVOKABLE void dismiss();
  void acknowledge(quint64 serial);
  void fail(const QString &message);
signals:
  void stateChanged();
protected:
  QSGNode *updatePaintNode(QSGNode *, UpdatePaintNodeData *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void hoverMoveEvent(QHoverEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void keyReleaseEvent(QKeyEvent *) override;
  void focusOutEvent(QFocusEvent *) override;
private:
  void poll();
  void input(uint32_t kind, uint32_t code, QPointF point = {});
  std::optional<rust::Box<omarchy::Session>> m_session;
  std::array<std::optional<rust::Box<omarchy::NativeBuffer>>, 2> m_buffers;
  QTimer m_timer;
  QSize m_viewport;
  QSize m_requestedViewport;
  int m_scale = 1;
  int m_requestedScale = 1;
  QRegion m_mask;
  QString m_error;
  bool m_started = false;
  bool m_ready = false;
  bool m_resizing = true;
  int m_pending = -1;
  int m_current = -1;
  quint64 m_serial = 0;
  quint64 m_generation = 0;
  quint64 m_requestedAfter = 0;
};
