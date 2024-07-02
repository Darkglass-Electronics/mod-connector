// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "connector.hpp"
#include "lv2.hpp"
#include "websocket.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtWebSockets/QWebSocket>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

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

struct WebSocketConnector : Connector,
                            WebSocketServer::Callbacks
{
    WebSocketServer wsServer;
    bool verboseLogs = false;

    WebSocketConnector()
        : Connector(),
          wsServer(this)
    {
        if (! ok)
            return;

        if (const char* const log = std::getenv("MOD_LOG"))
        {
            if (std::atoi(log) != 0)
                verboseLogs = true;
        }

        // reset ok state unless webserver init is good
        ok = false;

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
    }

    // handle new websocket connection
    // this will send the current state to the socket client, along with the list of plugins and categories
    void newWebSocketConnection(QWebSocket* const ws) override
    {
        if (! stateJson.contains("plugins"))
        {
            QJsonArray plugins;

            if (const uint32_t pcount = lv2world.get_plugin_count())
            {
                for (uint32_t i = 0; i < pcount; ++i)
                {
                    if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_index(i))
                    {
                        QJsonObject jsonObj;
                        lv2_plugin_to_json(plugin, jsonObj);
                        plugins.append(jsonObj);
                    }
                }
            }

            stateJson["plugins"] = plugins;

            QJsonArray categories;
            for (uint32_t i = 0; i < kLv2CategoryCount; ++i)
                categories.append(lv2_category_name(static_cast<Lv2Category>(i)));

            stateJson["categories"] = categories;
        }

        ws->sendTextMessage(QJsonDocument(stateJson).toJson(QJsonDocument::Compact));
    }

    // websocket message received, typically to indicate state changes
    void messageReceived(const QString& msg) override
    {
        const QJsonObject msgObj = QJsonDocument::fromJson(msg.toUtf8()).object();

        if (msgObj["type"] == "state")
        {
            QJsonObject stateObj;
            if (stateJson.contains("state"))
                stateObj = stateJson["state"].toObject();

            const QJsonObject msgStateObj(msgObj["state"].toObject());
            copyJsonObjectValue(stateObj, msgStateObj);

            stateJson["state"] = stateObj;

            if (verboseLogs)
            {
                puts(QJsonDocument(msgStateObj).toJson().constData());
            }

            handleStateChanges(msgStateObj);
            saveStateLater();
        }
    }

    // ----------------------------------------------------------------------------------------------------------------
    // delayed save handling

    bool stateChangedRecently = false;

    void saveStateLater()
    {
        if (stateChangedRecently)
            return;

        stateChangedRecently = true;
        QTimer::singleShot(1000, this, &WebSocketConnector::slot_saveStateNow);
    }

private slots:
    void slot_saveStateNow()
    {
        stateChangedRecently = false;

        QFile stateFile("state.json");
        if (stateFile.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text))
            stateFile.write(QJsonDocument(stateJson["state"].toObject()).toJson());
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

#ifdef HAVE_SYSTEMD
    if (connector.ok)
        sd_notify(0, "READY=1");
#endif

    return connector.ok ? app.exec() : 1;
}

// --------------------------------------------------------------------------------------------------------------------
