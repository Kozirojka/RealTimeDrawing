#include "collabsession.h"

// Tnz6 includes
#include "tapp.h"

// TnzLib includes
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"

// TnzCore includes
#include "tenv.h"
#include "tmsgcore.h"

// Qt includes
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMenu>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>

TEnv::StringVar CollabUserName("CollabUserName", "");
TEnv::StringVar CollabLastAddress("CollabLastAddress", "");
TEnv::IntVar CollabLastPort("CollabLastPort", CollabSession::DefaultPort);

namespace {

const int FlushIntervalMs = 40;      // ~25 updates per second at most
const int MaxLineSize     = 64 << 20;  // protection against garbage input

QString defaultUserName() {
  QString name = QString::fromStdString(std::string(CollabUserName));
  if (!name.isEmpty()) return name;
  name = qEnvironmentVariable("USERNAME");
  if (name.isEmpty()) name = qEnvironmentVariable("USER");
  if (name.isEmpty()) name = "Artist";
  return name;
}

}  // namespace

//=============================================================================
// CollabSession
//-----------------------------------------------------------------------------

CollabSession *CollabSession::instance() {
  static CollabSession *session = new CollabSession();
  return session;
}

//-----------------------------------------------------------------------------

CollabSession::CollabSession() : QObject() {
  m_flushTimer = new QTimer(this);
  m_flushTimer->setInterval(FlushIntervalMs);
  connect(m_flushTimer, SIGNAL(timeout()), this, SLOT(flush()));
}

CollabSession::~CollabSession() {}

//-----------------------------------------------------------------------------

void CollabSession::ensureAppConnections() {
  if (m_appConnected) return;
  m_appConnected = true;
  TApp *app      = TApp::instance();
  connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
          SLOT(onLocalPresenceChanged()));
  connect(app->getCurrentColumn(), SIGNAL(columnIndexSwitched()), this,
          SLOT(onLocalPresenceChanged()));
}

//-----------------------------------------------------------------------------

bool CollabSession::host(quint16 port, const QString &name, QString *error) {
  leave();
  ensureAppConnections();
  m_localName = name;
  m_isHost    = true;

  m_server = new QTcpServer(this);
  if (!m_server->listen(QHostAddress::Any, port)) {
    if (error) *error = m_server->errorString();
    delete m_server;
    m_server = nullptr;
    m_isHost = false;
    return false;
  }
  connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
  setState(Hosting);
  return true;
}

//-----------------------------------------------------------------------------

void CollabSession::join(const QString &address, quint16 port,
                         const QString &name) {
  leave();
  ensureAppConnections();
  m_localName = name;
  m_isHost    = false;

  QTcpSocket *socket = new QTcpSocket(this);
  attachSocket(socket);
  setState(Connecting);
  socket->connectToHost(address, port);
}

//-----------------------------------------------------------------------------

void CollabSession::leave() {
  m_flushTimer->stop();
  if (m_socket) {
    QTcpSocket *socket = m_socket;
    m_socket           = nullptr;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
  }
  if (m_server) {
    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;
  }
  m_inBuffer.clear();
  m_isHost = false;
  resetPeer();
  setState(Idle);
}

//-----------------------------------------------------------------------------

QString CollabSession::statusText() const {
  switch (m_state) {
  case Idle:
    return tr("Multiplayer");
  case Hosting:
    return tr("Hosting: waiting...");
  case Connecting:
    return tr("Connecting...");
  case Connected:
    return tr("Connected: %1").arg(m_peerName.isEmpty() ? tr("peer")
                                                         : m_peerName);
  }
  return QString();
}

//-----------------------------------------------------------------------------

void CollabSession::attachSocket(QTcpSocket *socket) {
  m_socket = socket;
  m_inBuffer.clear();
  socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
  connect(socket, SIGNAL(connected()), this, SLOT(onConnected()));
  connect(socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
  connect(socket, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
  connect(socket, SIGNAL(error(QAbstractSocket::SocketError)), this,
          SLOT(onSocketError()));
}

//-----------------------------------------------------------------------------

void CollabSession::onNewConnection() {
  while (m_server && m_server->hasPendingConnections()) {
    QTcpSocket *incoming = m_server->nextPendingConnection();
    if (m_socket) {
      // Only two people per session.
      QJsonObject busy{{"t", "busy"}};
      incoming->write(QJsonDocument(busy).toJson(QJsonDocument::Compact) +
                      '\n');
      incoming->disconnectFromHost();
      connect(incoming, SIGNAL(disconnected()), incoming,
              SLOT(deleteLater()));
      continue;
    }
    attachSocket(incoming);
    onConnected();
  }
}

//-----------------------------------------------------------------------------

void CollabSession::onConnected() {
  // The session becomes "Connected" after the peer's hello arrives.
  sendHello();
  m_presenceDirty = true;
  m_cursorDirty   = m_localCursorValid;
  m_flushTimer->start();
}

//-----------------------------------------------------------------------------

void CollabSession::onReadyRead() {
  if (!m_socket) return;
  m_inBuffer += m_socket->readAll();

  int pos;
  while ((pos = m_inBuffer.indexOf('\n')) >= 0) {
    QByteArray line = m_inBuffer.left(pos);
    m_inBuffer.remove(0, pos + 1);
    if (line.trimmed().isEmpty()) continue;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
    handleMessage(doc.object());
    if (!m_socket) return;  // the message may have closed the session
  }

  if (m_inBuffer.size() > MaxLineSize) {
    emit statusMessage(tr("Collaboration: invalid data received."));
    leave();
  }
}

//-----------------------------------------------------------------------------

void CollabSession::onDisconnected() {
  bool wasConnected = (m_state == Connected);
  QString peer      = m_peerName;

  if (m_socket) {
    m_socket->disconnect(this);
    m_socket->deleteLater();
    m_socket = nullptr;
  }
  m_flushTimer->stop();
  m_inBuffer.clear();
  resetPeer();

  if (m_isHost && m_server) {
    // Keep hosting so the peer can reconnect.
    setState(Hosting);
    if (wasConnected)
      emit statusMessage(tr("%1 left the session.").arg(peer));
  } else {
    setState(Idle);
    if (wasConnected) emit statusMessage(tr("Disconnected from the host."));
  }
}

//-----------------------------------------------------------------------------

void CollabSession::onSocketError() {
  if (!m_socket) return;
  QString errorString = m_socket->errorString();
  if (m_state == Connecting) {
    DVGui::warning(tr("Could not connect to the host:\n%1").arg(errorString));
    leave();
  }
  // other errors are followed by disconnected()
}

//-----------------------------------------------------------------------------

void CollabSession::handleMessage(const QJsonObject &msg) {
  const QString type = msg.value("t").toString();

  if (type == "hello") {
    int version = msg.value("v").toInt();
    if (version != ProtocolVersion) {
      DVGui::warning(
          tr("The collaborator uses an incompatible version of the "
             "multiplayer build (protocol %1, this build %2).")
              .arg(version)
              .arg(ProtocolVersion));
      if (m_isHost)
        onDisconnected();
      else
        leave();
      return;
    }
    m_peerName = msg.value("name").toString().left(64);
    setState(Connected);
    emit statusMessage(tr("%1 joined the session.").arg(m_peerName));
    return;
  }

  if (type == "busy") {
    DVGui::warning(tr("The host already has a collaborator connected."));
    leave();
    return;
  }

  if (type == "presence") {
    m_peerRow = msg.value("row").toInt(-1);
    m_peerCol = msg.value("col").toInt(0);
    emit peerPresenceChanged();
    return;
  }

  if (type == "cursor") {
    m_peerCursorValid = msg.value("on").toBool();
    m_peerCursor      = TPointD(msg.value("x").toDouble(),
                                msg.value("y").toDouble());
    m_peerCursorRow   = msg.value("row").toInt(-1);
    emit peerCursorChanged();
    return;
  }

  if (m_state == Connected) emit messageReceived(msg);
}

//-----------------------------------------------------------------------------

void CollabSession::send(const QJsonObject &msg) {
  if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
    return;
  m_socket->write(QJsonDocument(msg).toJson(QJsonDocument::Compact));
  m_socket->write("\n", 1);
}

//-----------------------------------------------------------------------------

void CollabSession::sendHello() {
  send(QJsonObject{{"t", "hello"},
                   {"v", ProtocolVersion},
                   {"name", m_localName},
                   {"host", m_isHost}});
}

//-----------------------------------------------------------------------------

void CollabSession::sendPresence() {
  TApp *app = TApp::instance();
  send(QJsonObject{{"t", "presence"},
                   {"row", app->getCurrentFrame()->getFrame()},
                   {"col", app->getCurrentColumn()->getColumnIndex()}});
}

//-----------------------------------------------------------------------------

void CollabSession::onLocalPresenceChanged() {
  m_presenceDirty = true;
  // a cursor shown on another frame should follow the frame change
  if (m_localCursorValid) m_cursorDirty = true;
}

//-----------------------------------------------------------------------------

void CollabSession::setLocalCursor(const TPointD &worldPos) {
  if (m_localCursorValid && m_localCursor == worldPos) return;
  m_localCursorValid = true;
  m_localCursor      = worldPos;
  m_cursorDirty      = true;
}

void CollabSession::clearLocalCursor() {
  if (!m_localCursorValid) return;
  m_localCursorValid = false;
  m_cursorDirty      = true;
}

//-----------------------------------------------------------------------------

void CollabSession::flush() {
  if (!m_socket) return;
  if (m_presenceDirty) {
    m_presenceDirty = false;
    sendPresence();
  }
  if (m_cursorDirty) {
    m_cursorDirty = false;
    send(QJsonObject{
        {"t", "cursor"},
        {"on", m_localCursorValid},
        {"x", m_localCursor.x},
        {"y", m_localCursor.y},
        {"row", TApp::instance()->getCurrentFrame()->getFrame()}});
  }
}

//-----------------------------------------------------------------------------

void CollabSession::resetPeer() {
  bool hadPresence  = m_peerRow >= 0;
  bool hadCursor    = m_peerCursorValid;
  m_peerName        = QString();
  m_peerRow         = -1;
  m_peerCol         = 0;
  m_peerCursorValid = false;
  m_peerCursorRow   = -1;
  if (hadPresence) emit peerPresenceChanged();
  if (hadCursor) emit peerCursorChanged();
}

//-----------------------------------------------------------------------------

void CollabSession::setState(State state) {
  if (m_state == state) return;
  m_state = state;
  emit stateChanged();
  // presence highlight depends on the connection state too
  emit peerPresenceChanged();
  emit peerCursorChanged();
}

//=============================================================================
// CollabButton
//-----------------------------------------------------------------------------

CollabButton::CollabButton(QWidget *parent) : QToolButton(parent) {
  setObjectName("CollabButton");
  setPopupMode(QToolButton::InstantPopup);
  setToolButtonStyle(Qt::ToolButtonTextOnly);
  setAutoRaise(true);

  QMenu *menu = new QMenu(this);
  m_hostAct   = menu->addAction(tr("Host Session..."));
  m_joinAct   = menu->addAction(tr("Join Session..."));
  menu->addSeparator();
  m_leaveAct = menu->addAction(tr("Leave Session"));
  setMenu(menu);

  connect(m_hostAct, SIGNAL(triggered()), this, SLOT(onHost()));
  connect(m_joinAct, SIGNAL(triggered()), this, SLOT(onJoin()));
  connect(m_leaveAct, SIGNAL(triggered()), this, SLOT(onLeave()));

  CollabSession *session = CollabSession::instance();
  connect(session, SIGNAL(stateChanged()), this, SLOT(updateStatus()));
  connect(session, &CollabSession::statusMessage, this,
          [](const QString &text) { DVGui::info(text); });
  updateStatus();
}

//-----------------------------------------------------------------------------

void CollabButton::updateStatus() {
  CollabSession *session = CollabSession::instance();
  CollabSession::State st = session->state();
  setText(session->statusText());

  QString color;
  switch (st) {
  case CollabSession::Idle:
    color = "";
    break;
  case CollabSession::Hosting:
  case CollabSession::Connecting:
    color = "color: #e0b000;";
    break;
  case CollabSession::Connected:
    color = "color: #3cc83c;";
    break;
  }
  setStyleSheet(color.isEmpty() ? QString()
                                : QString("QToolButton { %1 }").arg(color));

  bool idle = st == CollabSession::Idle;
  m_hostAct->setEnabled(idle);
  m_joinAct->setEnabled(idle);
  m_leaveAct->setEnabled(!idle);
}

//-----------------------------------------------------------------------------

void CollabButton::onHost() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Host Multiplayer Session"));

  QLineEdit *nameEdit = new QLineEdit(defaultUserName(), &dialog);
  QSpinBox *portBox   = new QSpinBox(&dialog);
  portBox->setRange(1024, 65535);
  portBox->setValue((int)CollabLastPort);

  QStringList addresses;
  for (const QHostAddress &addr : QNetworkInterface::allAddresses()) {
    if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback())
      continue;
    QString text = addr.toString();
    // Tailscale assigns addresses from 100.64.0.0/10
    if (addr.isInSubnet(QHostAddress("100.64.0.0"), 10))
      text += tr("  (Tailscale - give this one to your friend)");
    addresses << text;
  }
  QLabel *addrLabel = new QLabel(
      tr("Your addresses:\n%1").arg(addresses.isEmpty()
                                        ? tr("(none found)")
                                        : addresses.join("\n")),
      &dialog);
  addrLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

  QFormLayout *form = new QFormLayout();
  form->addRow(tr("Your name:"), nameEdit);
  form->addRow(tr("Port:"), portBox);

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Start Hosting"));
  connect(buttons, SIGNAL(accepted()), &dialog, SLOT(accept()));
  connect(buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));

  QVBoxLayout *layout = new QVBoxLayout(&dialog);
  layout->addLayout(form);
  layout->addWidget(addrLabel);
  layout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted) return;

  QString name = nameEdit->text().trimmed();
  if (name.isEmpty()) name = defaultUserName();
  CollabUserName = name.toStdString();
  CollabLastPort = portBox->value();

  QString error;
  if (!CollabSession::instance()->host((quint16)portBox->value(), name,
                                       &error))
    DVGui::warning(tr("Could not start the session:\n%1").arg(error));
}

//-----------------------------------------------------------------------------

void CollabButton::onJoin() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Join Multiplayer Session"));

  QLineEdit *nameEdit = new QLineEdit(defaultUserName(), &dialog);
  QLineEdit *addrEdit = new QLineEdit(
      QString::fromStdString(std::string(CollabLastAddress)), &dialog);
  addrEdit->setPlaceholderText(tr("e.g. 100.101.102.103"));
  QSpinBox *portBox = new QSpinBox(&dialog);
  portBox->setRange(1024, 65535);
  portBox->setValue((int)CollabLastPort);

  QFormLayout *form = new QFormLayout();
  form->addRow(tr("Your name:"), nameEdit);
  form->addRow(tr("Host address:"), addrEdit);
  form->addRow(tr("Port:"), portBox);

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Join"));
  connect(buttons, SIGNAL(accepted()), &dialog, SLOT(accept()));
  connect(buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));

  QVBoxLayout *layout = new QVBoxLayout(&dialog);
  layout->addLayout(form);
  layout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted) return;

  QString address = addrEdit->text().trimmed();
  if (address.isEmpty()) return;
  QString name = nameEdit->text().trimmed();
  if (name.isEmpty()) name = defaultUserName();
  CollabUserName    = name.toStdString();
  CollabLastAddress = address.toStdString();
  CollabLastPort    = portBox->value();

  CollabSession::instance()->join(address, (quint16)portBox->value(), name);
}

//-----------------------------------------------------------------------------

void CollabButton::onLeave() { CollabSession::instance()->leave(); }
