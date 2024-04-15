// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "websocket.hpp"

#include <QtCore/QFile>
#include <QtCore/QTimerEvent>
#include <QtNetwork/QSslKey>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>

// --------------------------------------------------------------------------------------------------------------------

struct WebSocketServer::Impl : QObject
{
    Impl(Callbacks* const callbacks, std::string& lastError)
        : callbacks(callbacks),
          lastError(lastError),
          wsServer("", std:.getenv("SSL_CERT") != nullptr ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode)
    {
        if (const char* const certfile = std:.getenv("SSL_CERT"))
        {
            QSslConfiguration sslconfig = wsServer.sslConfiguration();
            sslconfig.setLocalCertificate(QSslCertificate::fromPath(certfile).first());
            QFile keyFile(std::getenv("SSL_KEY"));
            if (keyFile.open(QIODevice::ReadOnly))
            {
                sslconfig.setPrivateKey(QSslKey(keyFile.readAll(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey));
                keyFile.close();
            }
            wsServer.setSslConfiguration(sslconfig);
        }

        connect(&wsServer, &QWebSocketServer::closed, this, &WebSocketServer::Impl::slot_closed);
        connect(&wsServer, &QWebSocketServer::newConnection, this, &WebSocketServer::Impl::slot_newConnection);
    }

    ~Impl()
    {
        for (QWebSocket* conn : wsConns)
        {
            conn->close();
            conn->deleteLater();
        }
    }

    bool listen(const uint16_t port)
    {
        lastError.clear();

        if (! wsServer.listen(QHostAddress::Any, port))
        {
            lastError = wsServer.errorString().toStdString();
            return false;
        }

        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // server slots

private slots:
    void slot_closed()
    {
    }

    void slot_newConnection()
    {
        printf("slot_newConnection\n");

        QWebSocket* ws;

        while ((ws = wsServer.nextPendingConnection()) != nullptr)
        {
            wsConns.append(ws);
            connect(ws, &QWebSocket::connected, this, &WebSocketServer::Impl::slot_connected);
            connect(ws, &QWebSocket::disconnected, this, &WebSocketServer::Impl::slot_disconnected);
            connect(ws, &QWebSocket::textMessageReceived, this, &WebSocketServer::Impl::slot_textMessageReceived);

            callbacks->newWebSocketConnection(ws);
        }

        if (! wsConns.empty() && timerId == 0)
            timerId = startTimer(1000);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // connection slots

    void slot_connected()
    {
        printf("connected\n");
    }

    void slot_disconnected()
    {
        printf("disconnected\n");

        if (QWebSocket* const conn = dynamic_cast<QWebSocket*>(sender()))
            wsConns.removeAt(wsConns.indexOf(conn));

        if (wsConns.empty() && timerId != 0)
        {
            killTimer(timerId);
            timerId = 0;
        }
    }

    void slot_textMessageReceived(const QString& message)
    {
        callbacks->messageReceived(message);
    }

    // ----------------------------------------------------------------------------------------------------------------

private:
    Callbacks* const callbacks;
    std::string& lastError;
    int timerId = 0;
    QList<QWebSocket*> wsConns;
    QWebSocketServer wsServer;

    void timerEvent(QTimerEvent* const event) override
    {
        if (event->timerId() == timerId)
        {
            for (QWebSocket* conn : wsConns)
                conn->ping();
        }

        QObject::timerEvent(event);
    }
};

// --------------------------------------------------------------------------------------------------------------------

WebSocketServer::WebSocketServer(Callbacks* const callbacks)
    : impl(new Impl(callbacks, last_error)) {}

WebSocketServer::~WebSocketServer() { delete impl; }

bool WebSocketServer::listen(const uint16_t port)
{
    return impl->listen(port);
}

// --------------------------------------------------------------------------------------------------------------------
