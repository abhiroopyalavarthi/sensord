#pragma once
// Strip-chart plot: one lane per device, every channel of that device drawn
// in its own colour. The right edge is "now" for each device's own clock, so
// devices with unrelated clocks (e.g. an ESP32) still line up.

#include <QMap>
#include <QPair>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <deque>

#include "rpc_client.hpp"

class PlotWidget : public QWidget {
  Q_OBJECT
 public:
  explicit PlotWidget(QWidget* parent = nullptr);

  void setWindowSeconds(double s) { window_s_ = s; }

 public slots:
  void addSamples(const ChunkList& chunks);
  void setDevices(const DeviceRows& rows);

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  struct Point {
    qint64 t_us;
    float v;
  };
  struct Trace {
    std::deque<Point> pts;
  };
  using Key = QPair<quint32, quint32>;  // device, channel

  QMap<Key, Trace> traces_;
  QMap<quint32, DeviceRow> devices_;
  double window_s_ = 5.0;
  bool dirty_ = false;
  QTimer repaint_;
};
