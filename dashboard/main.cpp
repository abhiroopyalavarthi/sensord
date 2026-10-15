// sensord-dashboard [--server ADDR] [--screenshot FILE --after SECONDS]

#include <QApplication>
#include <QScreen>
#include <QTimer>

#include "main_window.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  QString server = "localhost:50051";
  QString shot;
  double after = 3.0;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == "--server" && i + 1 < args.size()) server = args[++i];
    else if (args[i] == "--screenshot" && i + 1 < args.size()) shot = args[++i];
    else if (args[i] == "--after" && i + 1 < args.size()) after = args[++i].toDouble();
  }

  MainWindow w(server);
  w.show();

  // Used to grab the README screenshot without a human at the screen.
  if (!shot.isEmpty()) {
    QTimer::singleShot(static_cast<int>(after * 1000), &w, [&w, shot] {
      w.grab().save(shot);
      QApplication::quit();
    });
  }
  return app.exec();
}
