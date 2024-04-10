// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "websocket.hpp"

#include <QtCore/QCoreApplication>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>

// --------------------------------------------------------------------------------------------------------------------

struct WebSocket::Impl : QObject
{
    Impl(std::string& last_error)
        : last_error(last_error),
          ws_server("", QWebSocketServer::NonSecureMode)
    {
        connect(&ws_server, &QWebSocketServer::closed, this, &WebSocket::Impl::slot_closed);
        connect(&ws_server, &QWebSocketServer::newConnection, this, &WebSocket::Impl::slot_newConnection);
    }

    ~Impl()
    {
        for (QWebSocket* conn : conns)
        {
            conn->close();
            conn->deleteLater();
        }
    }

    bool listen(const uint16_t port)
    {
        last_error.clear();
        
        if (! ws_server.listen(QHostAddress::Any, port))
        {
            last_error = ws_server.errorString().toStdString();
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

        QWebSocket* conn;
        
        while ((conn = ws_server.nextPendingConnection()) != nullptr)
        {
            conns.append(conn);
            connect(conn, &QWebSocket::connected, this, &WebSocket::Impl::slot_connected);
            connect(conn, &QWebSocket::disconnected, this, &WebSocket::Impl::slot_disconnected);
            connect(conn, &QWebSocket::textMessageReceived, this, &WebSocket::Impl::slot_textMessageReceived);

            conn->ping();
        }

        if (timerId == 0)
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

        if (QWebSocket* const conn = reinterpret_cast<QWebSocket*>(sender()))
            conns.remove(conns.indexOf(conn));

        if (conns.empty() && timerId != 0)
        {
            killTimer(timerId);
            timerId = 0;
        }
    }

    void slot_textMessageReceived(const QString& message)
    {
        printf("slot_textMessageReceived '%s'\n", message.toUtf8().constData());

        // init message, report current state
        if (message == "init")
        {
        }
    }

    // ----------------------------------------------------------------------------------------------------------------

private:
    std::string& last_error;
    int timerId = 0;
    QWebSocketServer ws_server;
    QList<QWebSocket*> conns;

    void timerEvent(QTimerEvent* const event)
    {
        if (event->timerId() == timerId)
        {
            for (QWebSocket* conn : conns)
                conn->ping();
        }

        QObject::timerEvent(event);
    }
};

// --------------------------------------------------------------------------------------------------------------------

WebSocket::WebSocket() : impl(new Impl(last_error)) {}
WebSocket::~WebSocket() { delete impl; }

bool WebSocket::listen(const uint16_t port)
{
    return impl->listen(port);
}

// --------------------------------------------------------------------------------------------------------------------
