#pragma once

#ifndef COLLABSESSION_H
#define COLLABSESSION_H

// Real-time collaboration ("multiplayer") session for two users.
//
// One OpenToonz instance hosts (listens on a TCP port), the other joins it by
// address (e.g. a Tailscale IP). Messages are newline-delimited compact JSON
// objects with a "t" (type) field.
//
// Stage 1 implements the connection itself and presence: each side sees the
// other's current frame/column (highlighted in the Xsheet) and the other's
// cursor in the viewer. Other modules (xsheet sync, strokes) plug in through
// send() and the messageReceived() signal.

#include "tgeometry.h"

#include <QObject>
#include <QColor>
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QToolButton>

class QTcpServer;
class QTcpSocket;
class QTimer;

//=============================================================================

class CollabSession final : public QObject {
  Q_OBJECT

public:
  enum State { Idle, Hosting, Connecting, Connected };

  static const int ProtocolVersion = 1;
  static const quint16 DefaultPort = 45678;

  static CollabSession *instance();

  bool host(quint16 port, const QString &name, QString *error = nullptr);
  void join(const QString &address, quint16 port, const QString &name);
  void leave();

  State state() const { return m_state; }
  bool isConnected() const { return m_state == Connected; }
  bool isHost() const { return m_isHost; }
  QString localName() const { return m_localName; }
  QString statusText() const;

  // --- peer presence ---
  QString peerName() const { return m_peerName; }
  QColor peerColor() const { return QColor(255, 140, 0); }
  bool hasPeerPresence() const { return isConnected() && m_peerRow >= 0; }
  int peerRow() const { return m_peerRow; }
  int peerCol() const { return m_peerCol; }

  bool hasPeerCursor() const { return isConnected() && m_peerCursorValid; }
  TPointD peerCursor() const { return m_peerCursor; }
  int peerCursorRow() const { return m_peerCursorRow; }

  // --- local presence (called by the viewer) ---
  void setLocalCursor(const TPointD &worldPos);
  void clearLocalCursor();

  // --- generic messaging for other collaboration modules ---
  void send(const QJsonObject &msg);

signals:
  void stateChanged();
  void peerPresenceChanged();
  void peerCursorChanged();
  void messageReceived(const QJsonObject &msg);
  void statusMessage(const QString &text);

private slots:
  void onNewConnection();
  void onConnected();
  void onReadyRead();
  void onDisconnected();
  void onSocketError();
  void onLocalPresenceChanged();
  void flush();

private:
  CollabSession();
  ~CollabSession();

  void attachSocket(QTcpSocket *socket);
  void handleMessage(const QJsonObject &msg);
  void sendHello();
  void sendPresence();
  void resetPeer();
  void setState(State state);
  void ensureAppConnections();

  State m_state = Idle;
  bool m_isHost = false;
  QString m_localName;

  QTcpServer *m_server = nullptr;
  QTcpSocket *m_socket = nullptr;
  QByteArray m_inBuffer;
  QTimer *m_flushTimer = nullptr;

  bool m_appConnected = false;
  bool m_presenceDirty = false;
  bool m_cursorDirty   = false;
  bool m_localCursorValid = false;
  TPointD m_localCursor;

  QString m_peerName;
  int m_peerRow = -1, m_peerCol = 0;
  bool m_peerCursorValid = false;
  TPointD m_peerCursor;
  int m_peerCursorRow = -1;
};

//=============================================================================
// Button shown in the top bar: status + Host / Join / Leave menu.

class CollabButton final : public QToolButton {
  Q_OBJECT

public:
  CollabButton(QWidget *parent = nullptr);

private slots:
  void updateStatus();
  void onHost();
  void onJoin();
  void onLeave();

private:
  QAction *m_hostAct, *m_joinAct, *m_leaveAct;
};

#endif  // COLLABSESSION_H
