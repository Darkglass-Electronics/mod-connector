// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "host.hpp"
#include "lv2.hpp"
#include "websocket.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtWebSockets/QWebSocket>

// --------------------------------------------------------------------------------------------------------------------
// utility function that copies nested objects without deleting old values

static void copyJsonObjectValue(QJsonObject& dst, const QJsonObject& src)
{
    for (const QString& key : src.keys())
    {
        const QJsonValue& value(src[key]);

        if (value.isObject())
        {
            QJsonObject obj;
            if (dst.contains(key))
                obj = dst[key].toObject();

            copyJsonObjectValue(obj, src[key].toObject());

            dst[key] = obj;
        }
        else
        {
            dst[key] = src[key];
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

struct Connector : WebSocketServer::Callbacks
{
    Host host;
    Lv2World lv2world;
    WebSocketServer wsServer;
    bool ok = false;

    // keep current state in memory
    QJsonObject state;

    Connector()
        : host(),
          wsServer(this)
    {
        if (! host.last_error.empty())
        {
            fprintf(stderr, "Failed to initialize host connection: %s\n", host.last_error.c_str());
            return;
        }

        if (! wsServer.last_error.empty())
        {
            fprintf(stderr, "Failed to initialize websocket server: %s\n", wsServer.last_error.c_str());
            return;
        }

        if (! wsServer.listen(13371))
        {
            fprintf(stderr, "Failed to start websocket server: %s\n", wsServer.last_error.c_str());
            return;
        }

        ok = true;
        state["type"] = "state";
    }

    void newWebSocketConnection(QWebSocket* const ws) override
    {
        if (! state.contains("plugins"))
        {
            QJsonArray plugins;

            if (const uint32_t pcount = lv2world.get_plugin_count())
            {
                for (uint32_t i=0; i<pcount; ++i)
                {
                    if (const Lv2Plugin* const plugin = lv2world.get_plugin(i))
                    {
                        QJsonObject jsonObj;
                        lv2_plugin_to_json(plugin, jsonObj);
                        plugins.append(jsonObj);
                    }
                }
            }

            state["plugins"] = plugins;

            QJsonArray categories;
            for (uint32_t i = 0; i < kLv2CategoryCount; ++i)
                categories.append(lv2_category_name(static_cast<Lv2Category>(i)));

            state["categories"] = categories;
        }

        ws->sendTextMessage(QJsonDocument(state).toJson(QJsonDocument::Compact));
    }

    void messageReceived(const QString& msg) override
    {
        const QJsonObject msgObj = QJsonDocument::fromJson(msg.toUtf8()).object();

        if (msgObj["type"] == "state")
        {
            QJsonObject stateObj;
            if (state.contains("state"))
                stateObj = state["state"].toObject();

            copyJsonObjectValue(stateObj, msgObj["state"].toObject());

            state["state"] = stateObj;
        }
    }
};

// --------------------------------------------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("mod-connector");
    app.setApplicationVersion("0.0.1");
    app.setOrganizationName("Darkglass");

    Connector connector;
    return connector.ok ? app.exec() : 1;
}

// --------------------------------------------------------------------------------------------------------------------
