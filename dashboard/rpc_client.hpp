#pragma once
// All gRPC work for the dashboard happens here, on two std::jthreads:
//   - stream thread:  Subscribe() and forward samples
//   - control thread: poll ListDevices() and send SetSampleRate()
// Results reach the GUI only through Qt signals. Because the receivers live
// in the GUI thread, Qt queues those signals across threads automatically, so
// no widget is ever touched from a worker thread.

#include <QObject>
#include <QString>
#include <QVector>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

struct ChannelChunk {
  quint32 device = 0;
  quint32 channel = 0;
  quint64 t0_us = 0;
  quint32 period_us = 0;
  QVector<float> values;
};
using ChunkList = QVector<ChannelChunk>;

struct DeviceRow {
  quint32 id = 0;
  QString name;
  QString kind;
  bool connected = false;
  quint32 rate_hz = 0;
  quint64 frames = 0;
  quint64 crc_errors = 0;
  quint64 frames_lost = 0;
  quint64 reconnects = 0;
};
using DeviceRows = QVector<DeviceRow>;

Q_DECLARE_METATYPE(ChannelChunk)
Q_DECLARE_METATYPE(ChunkList)
Q_DECLARE_METATYPE(DeviceRow)
Q_DECLARE_METATYPE(DeviceRows)

class RpcClient : public QObject {
  Q_OBJECT
 public:
  explicit RpcClient(QObject* parent = nullptr);
  ~RpcClient() override;

  void connectTo(const QString& address);  // (re)starts both threads
  void disconnectFromServer();
  void requestSampleRate(quint32 device, quint32 hz);  // thread-safe

 signals:
  void samplesReceived(ChunkList chunks);
  void devicesUpdated(DeviceRows rows);
  void serverStateChanged(bool reachable, QString message);
  void rateResult(quint32 device, bool ok, QString message);
  void streamStats(quint64 batches, quint64 dropped);

 private:
  void streamLoop(std::stop_token st, std::string address);
  void controlLoop(std::stop_token st, std::string address);

  std::mutex mu_;
  std::condition_variable_any cv_;
  std::deque<std::pair<quint32, quint32>> pending_rates_;

  std::jthread stream_;
  std::jthread control_;
};
