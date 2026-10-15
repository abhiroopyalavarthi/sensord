#include "main_window.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {
enum Col { kId, kName, kKind, kLink, kRate, kFrames, kCrc, kLost, kRecon, kCols };
}

MainWindow::MainWindow(const QString& server, QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("sensord dashboard");
  resize(1200, 720);

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);

  // Top bar: server address + connection state.
  auto* top = new QHBoxLayout;
  top->addWidget(new QLabel("Server:"));
  server_ = new QLineEdit(server);
  server_->setMaximumWidth(240);
  top->addWidget(server_);
  connect_ = new QPushButton("Connect");
  top->addWidget(connect_);
  state_ = new QLabel("not connected");
  top->addWidget(state_, 1);
  stats_ = new QLabel;
  top->addWidget(stats_);
  root->addLayout(top);

  // Left: device table + sample rate control. Right: plots.
  auto* split = new QSplitter(Qt::Horizontal);
  auto* left = new QWidget;
  auto* lv = new QVBoxLayout(left);
  lv->setContentsMargins(0, 0, 0, 0);
  table_ = new QTableWidget(0, kCols);
  table_->setHorizontalHeaderLabels(
      {"ID", "Name", "Kind", "Link", "Rate", "Frames", "CRC err", "Lost", "Reconn"});
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  lv->addWidget(table_);

  auto* rate_row = new QHBoxLayout;
  rate_row->addWidget(new QLabel("Sample rate for selected device:"));
  rate_ = new QSpinBox;
  rate_->setRange(1, 20000);
  rate_->setValue(250);
  rate_->setSuffix(" Hz");
  rate_row->addWidget(rate_);
  apply_ = new QPushButton("Apply");
  rate_row->addWidget(apply_);
  rate_row->addStretch(1);
  lv->addLayout(rate_row);
  split->addWidget(left);

  plot_ = new PlotWidget;
  split->addWidget(plot_);
  split->setStretchFactor(0, 0);
  split->setStretchFactor(1, 1);
  split->setSizes({470, 730});
  root->addWidget(split, 1);
  setCentralWidget(central);

  connect(connect_, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
  connect(apply_, &QPushButton::clicked, this, &MainWindow::onApplyRate);
  // RpcClient emits from its worker threads; these connections are queued
  // onto the GUI thread automatically.
  connect(&rpc_, &RpcClient::samplesReceived, plot_, &PlotWidget::addSamples);
  connect(&rpc_, &RpcClient::devicesUpdated, plot_, &PlotWidget::setDevices);
  connect(&rpc_, &RpcClient::devicesUpdated, this, &MainWindow::onDevices);
  connect(&rpc_, &RpcClient::serverStateChanged, this, &MainWindow::onServerState);
  connect(&rpc_, &RpcClient::rateResult, this, &MainWindow::onRateResult);
  connect(&rpc_, &RpcClient::streamStats, this, &MainWindow::onStats);

  onConnectClicked();
}

void MainWindow::onConnectClicked() {
  state_->setText("connecting to " + server_->text() + "...");
  rpc_.connectTo(server_->text().trimmed());
}

void MainWindow::onApplyRate() {
  const auto rows = table_->selectionModel()->selectedRows();
  if (rows.isEmpty()) {
    statusBar()->showMessage("Select a device first", 3000);
    return;
  }
  const quint32 id = table_->item(rows.first().row(), kId)->text().toUInt();
  rpc_.requestSampleRate(id, static_cast<quint32>(rate_->value()));
}

void MainWindow::onDevices(const DeviceRows& rows) {
  // Keep the selection across refreshes.
  int selected = -1;
  if (auto sel = table_->selectionModel()->selectedRows(); !sel.isEmpty()) selected = sel.first().row();

  table_->setRowCount(static_cast<int>(rows.size()));
  for (int i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    auto set = [&](int col, const QString& text) {
      auto* item = table_->item(i, col);
      if (!item) {
        item = new QTableWidgetItem;
        table_->setItem(i, col, item);
      }
      item->setText(text);
    };
    set(kId, QString::number(r.id));
    set(kName, r.name);
    set(kKind, r.kind);
    set(kLink, r.connected ? "up" : "DOWN");
    table_->item(i, kLink)->setForeground(r.connected ? QColor(0x2E, 0x9E, 0x55) : QColor(0xD9, 0x3B, 0x30));
    set(kRate, QString::number(r.rate_hz));
    set(kFrames, QString::number(r.frames));
    set(kCrc, QString::number(r.crc_errors));
    set(kLost, QString::number(r.frames_lost));
    set(kRecon, QString::number(r.reconnects));
  }
  if (selected >= 0 && selected < rows.size()) table_->selectRow(selected);
}

void MainWindow::onServerState(bool up, const QString& msg) {
  state_->setText(msg);
  state_->setStyleSheet(up ? "color: #2E9E55;" : "color: #D93B30;");
}

void MainWindow::onRateResult(quint32, bool ok, const QString& msg) {
  statusBar()->showMessage(ok ? msg : "Rate change failed: " + msg, 4000);
}

void MainWindow::onStats(quint64 batches, quint64 dropped) {
  stats_->setText(QStringLiteral("batches %1  |  dropped %2").arg(batches).arg(dropped));
}
