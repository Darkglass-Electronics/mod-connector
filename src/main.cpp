// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "connector.hpp"
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

static void lv2_plugin_to_json(const Lv2Plugin* const plugin, QJsonObject& json)
{
    json["uri"] = QString::fromStdString(plugin->uri);
    json["name"] = QString::fromStdString(plugin->name);
    json["category"] = lv2_category_name(plugin->category);

    QJsonArray ports;

    for (size_t i = 0; i < plugin->ports.size(); ++i)
    {
        QJsonObject port;
        const Lv2Port& lv2port(plugin->ports[i]);

        port["symbol"] = QString::fromStdString(lv2port.symbol);
        port["name"] = QString::fromStdString(lv2port.name);

        QJsonArray flags;
        if (lv2port.flags & Lv2PortIsAudio)
            flags.append("audio");
        if (lv2port.flags & Lv2PortIsControl)
            flags.append("control");
        if (lv2port.flags & Lv2PortIsOutput)
            flags.append("output");
        if (lv2port.flags & Lv2ParameterToggled)
            flags.append("toggled");
        if (lv2port.flags & Lv2ParameterInteger)
            flags.append("integer");
        if (lv2port.flags & Lv2ParameterHidden)
            flags.append("hidden");
        port["flags"] = flags;

        if (lv2port.flags & Lv2PortIsControl)
        {
            port["default"] = lv2port.def;
            port["minimum"] = lv2port.min;
            port["maximum"] = lv2port.max;
        }

        ports.append(port);
    }

    json["ports"] = ports;
}

// --------------------------------------------------------------------------------------------------------------------

struct WebSocketConnector : QObject,
                            HostConnector,
                            WebSocketServer::Callbacks
{
    WebSocketServer wsServer;
    bool verboseLogs = false;

    // keep current state in memory
    QJsonObject stateJson;

    WebSocketConnector()
        : HostConnector(),
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

        stateJson["type"] = "state";

        QFile stateFile("state.json");
        if (stateFile.open(QIODevice::ReadOnly|QIODevice::Text))
        {
            stateJson["state"] = QJsonDocument::fromJson(stateFile.readAll()).object();
            handleStateChanges(stateJson["state"].toObject());
        }
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

    void handleStateChanges(const QJsonObject& stateObj)
    {
        bool bankchanged = false;
        bool blockschanged = false;

        if (stateObj.contains("bank"))
        {
            const int newbank = stateObj["bank"].toInt() - 1;

            if (current.bank != newbank)
            {
                printf("DEBUG: bank changed to %d\n", newbank);
                current.bank = newbank;
                bankchanged = true;
            }
            else
            {
                printf("DEBUG: bank remains as %d\n", newbank);
            }
        }
        else
        {
            printf("DEBUG: state has no current bank info\n");
        }

        const QJsonObject banks(stateObj["banks"].toObject());
        for (const QString& bankid : banks.keys())
        {
            const QJsonObject bank(banks[bankid].toObject());
            const int bankidi = bankid.toInt() - 1;
            auto& bankdata(current.banks[bankidi]);

            const QJsonObject presets(bank["presets"].toObject());
            for (const QString& presetid : presets.keys())
            {
                const QJsonObject preset(presets[presetid].toObject());
                const int presetidi = presetid.toInt() - 1;
                auto& presetdata(bankdata.presets[presetidi]);

                // if we are changing the current preset, send changes to mod-host
                const bool islive = !bankchanged && current.bank == bankidi && current.preset == presetidi;

                printf("DEBUG: now handling bank %d, live %d\n", bankidi, islive);

                const QJsonObject blocks(bank["blocks"].toObject());
                for (const QString& blockid : blocks.keys())
                {
                    const QJsonObject block(blocks[blockid].toObject());
                    const int blockidi = blockid.toInt() - 1;
                    auto& blockdata(presetdata.blocks[blockidi]);

                    printf("DEBUG: now handling block %d\n", blockidi);

                    if (block.contains("uri"))
                    {
                        const std::string uri = block["uri"].toString().toStdString();
                        blockdata.uri = uri;

                        if (islive)
                        {
                            blockschanged = true;
                            host.remove(blockidi);

                            if (uri != "-")
                            {
                                if (host.add(uri.c_str(), blockidi))
                                    printf("DEBUG: block %d loaded plugin %s\n", blockidi, uri.c_str());
                                else
                                    printf("DEBUG: block %d failed loaded plugin %s: %s\n",
                                            blockidi, uri.c_str(), host.last_error.c_str());

                                hostDisconnectForNewBlock(blockidi);
                            }
                            else
                            {
                                printf("DEBUG: block %d has no plugin\n", blockidi);
                            }
                        }
                    }
                    else
                    {
                        printf("DEBUG: block %d has no URI\n", blockidi);
                    }

                    if (block.contains("parameters"))
                    {
                        const QJsonObject parameters(block["parameters"].toObject());
                        for (const QString& parameterid : parameters.keys())
                        {
                            const QJsonObject parameter(parameters[parameterid].toObject());
                            const int parameteridi = parameterid.toInt() - 1;
                            auto& parameterdata(blockdata.parameters[parameteridi]);

                            if (parameter.contains("symbol"))
                            {
                                const std::string symbol = parameter["symbol"].toString().toStdString();
                                parameterdata.symbol = symbol;
                            }

                            if (parameter.contains("value"))
                            {
                                const float value = parameter["value"].toDouble();
                                parameterdata.value = value;

                                if (islive)
                                {
                                    const std::string symbol = parameterdata.symbol;
                                    host.param_set(blockidi, symbol.c_str(), value);
                                }
                            }
                        }
                    }
                }
            }
        }

        // puts(QJsonDocument(blocks).toJson().constData());

        if (bankchanged)
            hostLoadCurrent();
        else if (blockschanged)
            hostConnectBetweenBlocks();
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

    WebSocketConnector connector;

#ifdef HAVE_SYSTEMD
    if (connector.ok)
        sd_notify(0, "READY=1");
#endif

    return connector.ok ? app.exec() : 1;
}

// --------------------------------------------------------------------------------------------------------------------
