#include "pluginview.hpp"
#include "rust/cxx.h"
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPointer>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cmath>

namespace {
class BufferNode : public QSGSimpleTextureNode {
public:
  explicit BufferNode(PluginView *view, quint64 generation) : generation(generation), view(view) {
    auto window = view->window();
    before = QObject::connect(window, &QQuickWindow::beforeRendering, window, [this] {
      if (!fence) return;
      auto gl = QOpenGLContext::currentContext()->extraFunctions();
      auto result = gl->glClientWaitSync(fence, 0, 0); // Never wait on the Qt render thread.
      if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
        gl->glDeleteSync(fence);
        fence = nullptr;
        const auto completed = serial;
        serial = 0;
        QMetaObject::invokeMethod(this->view, [owner = this->view, completed] {
          if (owner) owner->acknowledge(completed);
        }, Qt::QueuedConnection);
      } else if (result == GL_WAIT_FAILED) {
        report("GPU completion check failed");
      }
    }, Qt::DirectConnection);
    after = QObject::connect(window, &QQuickWindow::afterFrameEnd, window, [this] {
      if (!serial) return;
      if (!fence) {
        auto gl = QOpenGLContext::currentContext()->extraFunctions();
        fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl->glFlush();
        if (!fence) { report("GPU completion fence unavailable"); return; }
      }
      QMetaObject::invokeMethod(this->view, &QQuickItem::update, Qt::QueuedConnection);
    }, Qt::DirectConnection);
  }
  ~BufferNode() override {
    QObject::disconnect(before);
    QObject::disconnect(after);
    for (auto wrapper : wrappers) delete wrapper;
    if (auto context = QOpenGLContext::currentContext()) {
      auto gl = context->extraFunctions();
      if (fence) gl->glDeleteSync(fence);
      gl->glDeleteTextures(2, ids.data());
    }
    auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    if (destroy) for (auto image : images) if (image != EGL_NO_IMAGE_KHR) destroy(display, image);
  }
  void report(const QString &message) {
    QMetaObject::invokeMethod(view, [owner = view, message] { if (owner) owner->fail(message); }, Qt::QueuedConnection);
  }
  bool import(QQuickWindow *window, const std::array<std::optional<rust::Box<omarchy::NativeBuffer>>, 2> &buffers) {
    auto context = QOpenGLContext::currentContext();
    if (!context) return false;
    auto gl = context->extraFunctions();
    auto create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    using BindImage = void (*)(GLenum, void *);
    auto target = reinterpret_cast<BindImage>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    display = eglGetCurrentDisplay();
    if (!create || !target || display == EGL_NO_DISPLAY) return false;
    gl->glGenTextures(2, ids.data());
    for (int slot = 0; slot < 2; ++slot) {
      if (!buffers[slot]) return false;
      const auto b = (*buffers[slot])->info(); // Validated in Rust; FD stays Rust-owned.
      const EGLint attributes[] = {EGL_WIDTH, EGLint(b.width), EGL_HEIGHT, EGLint(b.height),
        EGL_LINUX_DRM_FOURCC_EXT, 0x34325241, EGL_DMA_BUF_PLANE0_FD_EXT, b.fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(b.stride),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0, EGL_NONE};
      images[slot] = create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
      if (images[slot] == EGL_NO_IMAGE_KHR) return false;
      gl->glBindTexture(GL_TEXTURE_2D, ids[slot]);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      target(GL_TEXTURE_2D, images[slot]);
      if (gl->glGetError() != GL_NO_ERROR) return false;
      wrappers[slot] = QNativeInterface::QSGOpenGLTexture::fromNative(ids[slot], window,
        QSize(b.width, b.height), QQuickWindow::TextureHasAlphaChannel);
      if (!wrappers[slot]) return false;
    }
    return true;
  }
  std::array<QSGTexture *, 2> wrappers{};
  const quint64 generation;
  quint64 serial = 0;
private:
  QPointer<PluginView> view;
  QMetaObject::Connection before, after;
  std::array<GLuint, 2> ids{};
  std::array<EGLImageKHR, 2> images{};
  EGLDisplay display = EGL_NO_DISPLAY;
  GLsync fence = nullptr;
};

uint32_t buttonCode(Qt::MouseButton button) {
  switch (button) {
    case Qt::LeftButton: return 0x110;
    case Qt::RightButton: return 0x111;
    case Qt::MiddleButton: return 0x112;
    default: return 0;
  }
}
}

PluginView::PluginView(QQuickItem *parent) : QQuickItem(parent) {
  setFlag(ItemHasContents);
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
  setAcceptHoverEvents(true);
  m_timer.setInterval(16);
  connect(&m_timer, &QTimer::timeout, this, &PluginView::poll);
}

void PluginView::start(const QString &store, const QString &id, const QString &controller, int w, int h, int scale, const QString &context) {
  if (m_started) { fail("PluginView cannot be restarted; create a new host item"); return; }
  m_started = true;
  try {
    const auto root = store.toUtf8(), name = id.toUtf8(), program = controller.toUtf8(), json = context.toUtf8();
    m_session.emplace(omarchy::begin(root.constData(), name.constData(), program.constData(), w, h, scale, json.constData()));
    m_requestedViewport = QSize(w, h);
    m_requestedScale = scale;
    m_timer.start();
  } catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
}

void PluginView::setContext(const QString &context) {
  if (!m_session) return;
  try {
    const auto json = context.toUtf8();
    omarchy::context(**m_session, json.constData());
  } catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
}

void PluginView::configure(int w, int h, int scale) {
  if (!m_session) return;
  if (m_requestedViewport == QSize(w, h) && m_requestedScale == scale) return;
  try {
    omarchy::configure(**m_session, w, h, scale);
    m_requestedViewport = QSize(w, h);
    m_requestedScale = scale;
    m_resizing = true;
    m_requestedAfter = m_generation;
    setFocus(false);
    emit stateChanged();
  } catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
}

void PluginView::stop() {
  m_timer.stop();
  m_session.reset(); // Rust thread observes disconnect and stops only its service.
  for (auto &buffer : m_buffers) buffer.reset();
  m_mask = QRegion();
  m_pending = -1;
  m_current = -1;
  m_serial = 0;
  m_ready = false;
  m_hasSurface = false;
  m_presented = false;
  m_panelOpen = false;
  m_panelSerial = 0;
  m_widgetSize = QSize(0, 0);
  m_panelSwitchDirection = 0;
  m_resizing = true;
  setFocus(false);
  update();
  emit stateChanged();
  emit panelChanged();
  emit widgetSizeChanged();
}
void PluginView::fail(const QString &message) { m_error = message; stop(); }

void PluginView::poll() {
  if (!m_session) return;
  try {
    for (int count = 0; count < 8; ++count) {
      auto event = omarchy::next(**m_session);
      switch (event.kind) {
        case omarchy::EventKind::Empty: return;
        case omarchy::EventKind::Ready: m_ready = true; emit stateChanged(); break;
        case omarchy::EventKind::PanelState:
          m_panelOpen = event.panel_open;
          m_panelSerial = event.panel_serial;
          emit panelChanged();
          break;
        case omarchy::EventKind::WidgetSize:
          m_widgetSize = QSize(event.width, event.height);
          emit widgetSizeChanged();
          break;
        case omarchy::EventKind::PanelSwitch: {
          const int direction = event.switch_forward ? 1 : -1;
          if (hasActiveFocus() && m_panelSwitchDirection == direction
              && m_panelSwitchAge.isValid() && m_panelSwitchAge.elapsed() <= 1000) {
            m_panelSwitchDirection = 0;
            emit panelSwitchRequested(direction);
          }
          break;
        }
        case omarchy::EventKind::Configured:
          m_generation = event.generation;
          m_viewport = QSize(event.width, event.height);
          m_scale = event.scale;
          m_mask = QRegion();
          m_hasSurface = false;
          for (auto &buffer : m_buffers) buffer.reset();
          m_pending = -1;
          m_current = -1;
          m_resizing = true;
          emit stateChanged();
          break;
        case omarchy::EventKind::Buffer: m_buffers[event.slot].emplace(std::move(event.buffer)); break;
        case omarchy::EventKind::Frame:
          m_pending = int(event.slot); m_serial = event.serial; update(); break;
        case omarchy::EventKind::Mask: {
          // Every mapped surface contributes a clip row, even when it has an
          // empty input region. No worker-authored readiness claim is needed.
          m_hasSurface = !event.regions.empty();
          QRegion result, current;
          QRect clip;
          for (const auto &region : event.regions) {
            const QRect rect(region.x, region.y, region.width, region.height);
            if (region.operation == 0) { result += current.intersected(clip); current = QRegion(); clip = rect; }
            else if (region.operation == 1) current += rect;
            else current -= rect;
          }
          const auto next = (result + current.intersected(clip)).intersected(QRect(QPoint(), m_viewport));
          if (next.rectCount() > 1024) { fail("Plugin input region is too complex"); return; }
          if (m_mask != next) { m_mask = next; emit stateChanged(); update(); }
          break;
        }
        case omarchy::EventKind::Failed: fail(QString::fromUtf8(event.error.data(), event.error.size())); return;
        default: fail("Unknown native event"); return;
      }
    }
  } catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
}
void PluginView::acknowledge(quint64 serial) {
  if (!m_session || serial != m_serial) return;
  try {
    omarchy::presented(**m_session, serial);
    m_serial = 0;
    if (!m_presented && m_hasSurface) { m_presented = true; emit stateChanged(); }
    if (m_resizing && m_generation != m_requestedAfter && m_viewport == m_requestedViewport && m_scale == m_requestedScale) {
      m_resizing = false;
      emit stateChanged();
      // The host window's input mask changes on readiness. Schedule a frame
      // so Quickshell polishes that mask even if plugin content is static.
      update();
    }
  }
  catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
}

bool PluginView::contains(const QPointF &point) const {
  return m_ready && !m_resizing && width() > 0 && height() > 0 && std::isfinite(point.x()) && std::isfinite(point.y())
    && boundingRect().contains(point)
    && m_mask.contains(QPoint(point.x() * m_viewport.width() / width(), point.y() * m_viewport.height() / height()));
}
QVariantList PluginView::inputRegions() const {
  QVariantList regions;
  if (!m_ready || m_resizing) return regions;
  for (const auto &rect : m_mask) regions.append(rect);
  return regions;
}
void PluginView::input(uint32_t kind, uint32_t code, QPointF point) {
  if (!m_ready || m_resizing || !m_session || width() <= 0 || height() <= 0) return;
  if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return;
  // Mouse grabs can deliver releases outside the item. Keep private coordinates bounded.
  const auto x = qBound(0.0, point.x() * m_viewport.width() / width(), double(m_viewport.width() - 1));
  const auto y = qBound(0.0, point.y() * m_viewport.height() / height(), double(m_viewport.height() - 1));
  try { omarchy::input(**m_session, kind, code, int(x), int(y)); }
  catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
}
void PluginView::mousePressEvent(QMouseEvent *event) { m_panelSwitchDirection = 0; emit focusRequested(); forceActiveFocus(); input(0, buttonCode(event->button()), event->position()); event->accept(); }
void PluginView::mouseReleaseEvent(QMouseEvent *event) { input(1, buttonCode(event->button()), event->position()); event->accept(); }
void PluginView::mouseMoveEvent(QMouseEvent *event) { input(2, 0, event->position()); event->accept(); }
void PluginView::hoverEnterEvent(QHoverEvent *event) { input(2, 0, event->position()); event->accept(); }
void PluginView::hoverMoveEvent(QHoverEvent *event) { input(2, 0, event->position()); event->accept(); }
void PluginView::hoverLeaveEvent(QHoverEvent *event) { input(6, 0); event->accept(); }
void PluginView::wheelEvent(QWheelEvent *event) {
  // Unlike button releases, wheels have no implicit grab. Do not redirect an
  // event outside the worker's current mask (including during a resize).
  if (!m_session || !contains(event->position())) { event->ignore(); return; }
  const auto pixels = event->pixelDelta();
  const auto delta = pixels.isNull() ? event->angleDelta() : pixels;
  const bool ended = event->phase() == Qt::ScrollEnd;
  const uint32_t source = pixels.isNull() ? 0 : 1;
  if (delta.isNull() && !ended) { event->accept(); return; }
  const auto point = event->position();
  const int x = qBound(0, int(point.x() * m_viewport.width() / width()), m_viewport.width() - 1);
  const int y = qBound(0, int(point.y() * m_viewport.height() / height()), m_viewport.height() - 1);
  try {
    // Qt has already applied the user's natural-scroll direction; do not invert twice.
    const int horizontal = qRound(qBound(-4096.0, source == 1 ? double(delta.x()) * m_viewport.width() / width() : double(delta.x()), 4096.0));
    const int vertical = qRound(qBound(-4096.0, source == 1 ? double(delta.y()) * m_viewport.height() / height() : double(delta.y()), 4096.0));
    if (horizontal || vertical) omarchy::scroll(**m_session, source, x, y, horizontal, vertical);
    if (ended) omarchy::scroll(**m_session, 2, x, y, 0, 0);
  } catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
  event->accept();
}
void PluginView::key(QKeyEvent *event, bool pressed) {
  if (m_ready && !m_resizing && m_session && !event->isAutoRepeat()
      && event->nativeScanCode() >= 8 && (!pressed || event->nativeVirtualKey() != 0)) {
    try { omarchy::key(**m_session, event->nativeScanCode(), event->nativeVirtualKey(), pressed); }
    catch (const rust::Error &error) { fail(QString::fromUtf8(error.what())); }
  }
  event->accept();
}
void PluginView::keyPressEvent(QKeyEvent *event) {
  m_panelSwitchDirection = 0;
  if (!event->isAutoRepeat() && (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab)
      && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
    m_panelSwitchDirection = event->key() == Qt::Key_Backtab || (event->modifiers() & Qt::ShiftModifier) ? -1 : 1;
    m_panelSwitchAge.start();
  }
  key(event, true);
}
void PluginView::keyReleaseEvent(QKeyEvent *event) { key(event, false); }
void PluginView::dismiss() { m_panelSwitchDirection = 0; input(5, 0); setFocus(false); }
void PluginView::focusOutEvent(QFocusEvent *event) { m_panelSwitchDirection = 0; input(5, 0); QQuickItem::focusOutEvent(event); }

QSGNode *PluginView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
  if (!m_ready) { delete old; return nullptr; }
  auto node = static_cast<BufferNode *>(old);
  if (m_pending >= 0 || (!node && m_current >= 0)) {
    if (node && node->generation != m_generation) { delete node; node = nullptr; }
    if (!node) {
      node = new BufferNode(this, m_generation);
      if (!node->import(window(), m_buffers)) {
        node->report("Plugin buffer import requires a compatible OpenGL/EGL renderer");
        delete node;
        m_pending = -1;
        return nullptr;
      }
    }
    if (m_pending >= 0) m_current = m_pending;
    node->setTexture(node->wrappers[m_current]);
    node->markDirty(QSGNode::DirtyMaterial);
    node->serial = m_serial;
    m_pending = -1;
  }
  if (node) node->setRect(boundingRect());
  return node;
}
