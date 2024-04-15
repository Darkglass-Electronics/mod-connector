// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "host.hpp"
#include "lv2.hpp"
#include "websocket.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
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

struct Connector : QObject,
                   WebSocketServer::Callbacks
{
    Host host;
    Lv2World lv2world;
    WebSocketServer wsServer;
    bool ok = false;
    bool verboseLogs = false;

    // keep current state in memory
    QJsonObject state;
    struct {
        QString bank = "1";
        QString preset = "1";
    } current;

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

        if (const char* const log = std::getenv("MOD_LOG"))
        {
            if (std::atoi(log) != 0)
                verboseLogs = true;
        }

        ok = true;
        state["type"] = "state";

        QFile stateFile("state.json");
        if (stateFile.open(QIODevice::ReadOnly|QIODevice::Text))
        {
            state["state"] = QJsonDocument::fromJson(stateFile.readAll()).object();
            handleStateChanges(state["state"].toObject());
        }
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

            if (verboseLogs)
            {
                puts(QJsonDocument(stateObj).toJson().constData());
            }

            const QJsonObject msgStateObj(msgObj["state"].toObject());
            copyJsonObjectValue(stateObj, msgStateObj);

            state["state"] = stateObj;

            handleStateChanges(msgStateObj);
            saveStateLater();
        }
    }

    void loadCurrent()
    {
        host.remove(-1);
        handleStateChanges(state["state"].toObject());
    }

    void handleStateChanges(const QJsonObject& stateObj)
    {
        if (stateObj.contains("bank"))
        {
            const QString newbank = stateObj["bank"].toString();

            if (current.bank != newbank)
            {
                current.bank = newbank;
                current.preset = "1";
                loadCurrent();
            }
        }

        const QJsonObject bank(stateObj["banks"].toObject()[current.bank].toObject());
        if (bank.isEmpty())
            return;

        if (bank.contains("preset"))
        {
            const QString newpreset = bank["preset"].toString();

            if (current.preset != newpreset)
            {
                current.preset = newpreset;
                loadCurrent();
            }
        }

        const QJsonObject chains(bank["presets"].toObject()[current.preset].toObject()["chain"].toObject());
        if (chains.isEmpty())
            return;

        for (const QString& c : chains.keys())
        {
            const QJsonObject chain(chains[c].toObject());

            // add/remove plugin if changing "uri"
            if (chain.contains("uri"))
            {
                const QString uri = chain["uri"].toString();
                const int cid = c.toInt();

                host.remove(cid);

                if (uri != "-")
                    host.add(uri.toUtf8().constData(), cid);
            }
        }

        puts(QJsonDocument(chains).toJson().constData());
    }

    // ----------------------------------------------------------------------------------------------------------------
    // delayed save handling

    bool stateChangedRecently = false;

    void saveStateLater()
    {
        if (stateChangedRecently)
            return;

        stateChangedRecently = true;
        QTimer::singleShot(1000, this, &Connector::slot_saveStateNow);
    }

private slots:
    void slot_saveStateNow()
    {
        stateChangedRecently = false;

        QFile stateFile("state.json");
        if (stateFile.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text))
            stateFile.write(QJsonDocument(state["state"].toObject()).toJson());
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
