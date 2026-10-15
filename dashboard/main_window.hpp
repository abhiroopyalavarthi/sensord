#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

#include "plot_widget.hpp"
#include "rpc_client.hpp"

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(const QString& server, QWidget* parent = nullptr);

 private slots:
  void onConnectClicked();
  void onApplyRate();
  void onDevices(const DeviceRows& rows);
  void onServerState(bool up, const QString& msg);
  void onRateResult(quint32 device, bool ok, const QString& msg);
  void onStats(quint64 batches, quint64 dropped);

 private:
  RpcClient rpc_;
  QLineEdit* server_;
  QPushButton* connect_;
  QLabel* state_;
  QLabel* stats_;
  QTableWidget* table_;
  QSpinBox* rate_;
  QPushButton* apply_;
  PlotWidget* plot_;
};
