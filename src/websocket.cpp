// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "websocket.hpp"

#include <QtCore/QObject>
#include <QtWebSockets/QWebSocketServer>

#include <QCoreApplication>

#include <unistd.h>

// --------------------------------------------------------------------------------------------------------------------

struct WebSocket::Impl : QObject
{
    Impl(std::string& last_error)
        : last_error(last_error),
          ws("", QWebSocketServer::NonSecureMode)
    {
        connect(&ws, &QWebSocketServer::closed, this, &WebSocket::Impl::slot_closed);
        connect(&ws, &QWebSocketServer::newConnection, this, &WebSocket::Impl::slot_newConnection);
//         connect(&ws, &QAbstractSocket::readyRead, this, &WebSocket::Impl::slot_readyRead);
    }

    bool listen(const uint16_t port)
    {
        last_error.clear();
        
        if (! ws.listen(QHostAddress::Any, port))
        {
            last_error = ws.errorString().toStdString();
            return false;
        }

        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------

private slots:
    void slot_closed()
    {
    }

    void slot_newConnection()
    {
        QWebSocket* conn;
        
        while ((conn = ws.nextPendingConnection()) != nullptr)
            conns.append(conn);
    }

    void slot_readyRead()
    {
    }

    // ----------------------------------------------------------------------------------------------------------------

private:
    std::string& last_error;
    QWebSocketServer ws;
    QList<QWebSocket*> conns;
};

// --------------------------------------------------------------------------------------------------------------------

WebSocket::WebSocket() : impl(new Impl(last_error)) {}
WebSocket::~WebSocket() { delete impl; }

bool WebSocket::listen(const uint16_t port)
{
    return impl->listen(port);
}

// --------------------------------------------------------------------------------------------------------------------
