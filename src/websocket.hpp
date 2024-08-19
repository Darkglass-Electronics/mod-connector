// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

class QString;
class QWebSocket;

// --------------------------------------------------------------------------------------------------------------------

struct WebSocketServer
{
    struct Callbacks {
        virtual ~Callbacks() {}
        virtual void newWebSocketConnection(QWebSocket* ws) = 0;
        virtual void messageReceived(const QString& message) = 0;
    };

   /**
    * string describing the last error, in case any operation fails.
    * will also be set during initialization in case of mod-host connection failure.
    */
    std::string last_error;

    bool listen(uint16_t port);

    WebSocketServer(Callbacks* callbacks);
    ~WebSocketServer();

private:
    struct Impl;
    Impl* const impl;
};

// --------------------------------------------------------------------------------------------------------------------
