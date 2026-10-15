#include "plot_widget.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
const QColor kChannelColors[] = {QColor(0xE4, 0x57, 0x4C), QColor(0x3D, 0x8B, 0xD9),
                                 QColor(0x4C, 0xB0, 0x6B), QColor(0xE8, 0xA3, 0x3D),
                                 QColor(0x9B, 0x6B, 0xD6)};
}

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
  setMinimumSize(500, 300);
  setAutoFillBackground(true);
  QPalette p = palette();
  p.setColor(QPalette::Window, QColor(0x16, 0x18, 0x1D));
  setPalette(p);
  // Repaint at most ~30 fps, and only if new data arrived.
  connect(&repaint_, &QTimer::timeout, this, [this] {
    if (dirty_) {
      dirty_ = false;
      update();
    }
  });
  repaint_.start(33);
}

void PlotWidget::addSamples(const ChunkList& chunks) {
  for (const auto& c : chunks) {
    auto& tr = traces_[Key(c.device, c.channel)];
    for (int i = 0; i < c.values.size(); ++i)
      tr.pts.push_back({static_cast<qint64>(c.t0_us + quint64(i) * c.period_us), c.values[i]});
    // Trim to the visible window (plus a little).
    const qint64 newest = tr.pts.back().t_us;
    const qint64 keep_from = newest - static_cast<qint64>(window_s_ * 1.1e6);
    while (!tr.pts.empty() && tr.pts.front().t_us < keep_from) tr.pts.pop_front();
  }
  dirty_ = true;
}

void PlotWidget::setDevices(const DeviceRows& rows) {
  devices_.clear();
  for (const auto& r : rows) devices_.insert(r.id, r);
  dirty_ = true;
}

void PlotWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  // Lanes: every device we know about, from ListDevices or from data.
  QSet<quint32> ids;
  for (auto it = devices_.cbegin(); it != devices_.cend(); ++it) ids.insert(it.key());
  for (auto it = traces_.cbegin(); it != traces_.cend(); ++it) ids.insert(it.key().first);
  QList<quint32> lanes = ids.values();
  std::sort(lanes.begin(), lanes.end());

  if (lanes.isEmpty()) {
    p.setPen(QColor(0x9A, 0xA0, 0xA6));
    p.drawText(rect(), Qt::AlignCenter, "waiting for data...");
    return;
  }

  const int label_w = 110;
  const double lane_h = double(height()) / double(lanes.size());
  const double plot_w = width() - label_w - 10;

  for (int li = 0; li < lanes.size(); ++li) {
    const quint32 dev = lanes[li];
    const QRectF lane(label_w, li * lane_h + 4, plot_w, lane_h - 8);
    const bool known = devices_.contains(dev);
    const bool up = !known || devices_[dev].connected;

    p.setPen(QColor(0x2A, 0x2E, 0x36));
    p.setBrush(QColor(0x1C, 0x1F, 0x26));
    p.drawRoundedRect(lane, 4, 4);

    // Label.
    p.setPen(up ? QColor(0xDD, 0xDF, 0xE2) : QColor(0x88, 0x8C, 0x92));
    const QString name = known ? devices_[dev].name : QStringLiteral("device %1").arg(dev);
    p.drawText(QRectF(6, lane.top(), label_w - 10, lane.height() / 2),
               Qt::AlignLeft | Qt::AlignBottom, name);
    QString sub = known ? QStringLiteral("%1 Hz").arg(devices_[dev].rate_hz) : QString();
    p.setPen(up ? QColor(0x7F, 0xC8, 0x8E) : QColor(0xE4, 0x57, 0x4C));
    p.drawText(QRectF(6, lane.center().y(), label_w - 10, lane.height() / 2),
               Qt::AlignLeft | Qt::AlignTop, up ? sub : QStringLiteral("disconnected"));

    // Autoscale across this device's channels.
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    qint64 newest = 0;
    for (auto it = traces_.cbegin(); it != traces_.cend(); ++it) {
      if (it.key().first != dev || it.value().pts.empty()) continue;
      newest = std::max(newest, it.value().pts.back().t_us);
      for (const auto& pt : it.value().pts) {
        if (!std::isfinite(pt.v)) continue;
        lo = std::min(lo, pt.v);
        hi = std::max(hi, pt.v);
      }
    }
    if (newest != 0) {  // draw traces (a device may have no data yet)
      if (hi - lo < 1e-6f) {
        hi += 0.5f;
        lo -= 0.5f;
      }
      const float pad = (hi - lo) * 0.08f;
      lo -= pad;
      hi += pad;

      p.save();
      p.setClipRect(lane);
      for (auto it = traces_.cbegin(); it != traces_.cend(); ++it) {
        if (it.key().first != dev) continue;
        const auto& pts = it.value().pts;
        if (pts.size() < 2) continue;
        QPainterPath path;
        bool started = false;
        // Decimate: at most ~2 points per horizontal pixel.
        const std::size_t stride = std::max<std::size_t>(1, pts.size() / std::size_t(plot_w * 2));
        for (std::size_t i = 0; i < pts.size(); i += stride) {
          const double age_s = double(newest - pts[i].t_us) * 1e-6;
          const double x = lane.right() - age_s / window_s_ * lane.width();
          const double y = lane.bottom() - double(pts[i].v - lo) / double(hi - lo) * lane.height();
          if (!started) {
            path.moveTo(x, y);
            started = true;
          } else {
            path.lineTo(x, y);
          }
        }
        QColor c = kChannelColors[it.key().second % 5];
        if (!up) c = QColor(0x55, 0x58, 0x5E);
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
      }
      p.restore();
    }

    if (!up) {
      p.setPen(QColor(0xE4, 0x57, 0x4C));
      QFont f = p.font();
      f.setBold(true);
      p.setFont(f);
      p.drawText(lane, Qt::AlignCenter, "DISCONNECTED");
      p.setFont(font());
    }
  }
}
