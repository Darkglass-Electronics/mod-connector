// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
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

    // keep current bank in memory
    QJsonObject bankJson;

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

        bankJson["type"] = "bank";

        QFile bankFile("bank.json");
        if (bankFile.open(QIODevice::ReadOnly|QIODevice::Text))
        {
            bankJson["bank"] = QJsonDocument::fromJson(bankFile.readAll()).object();
            handleBankChanges(bankJson["bank"].toObject());
        }
    }

    // handle new websocket connection
    // this will send the current bank to the socket client, along with the list of plugins and categories
    void newWebSocketConnection(QWebSocket* const ws) override
    {
        if (! bankJson.contains("plugins"))
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

            bankJson["plugins"] = plugins;

            QJsonArray categories;
            for (uint32_t i = 0; i < kLv2CategoryCount; ++i)
                categories.append(lv2_category_name(static_cast<Lv2Category>(i)));

            bankJson["categories"] = categories;
        }

        ws->sendTextMessage(QJsonDocument(bankJson).toJson(QJsonDocument::Compact));
    }

    // websocket message received, typically to indicate bank changes
    void messageReceived(const QString& msg) override
    {
        const QJsonObject msgObj = QJsonDocument::fromJson(msg.toUtf8()).object();

        if (msgObj["type"] == "bank")
        {
            QJsonObject bankObj;
            if (bankJson.contains("bank"))
                bankObj = bankJson["bank"].toObject();

            const QJsonObject msgBankObj(msgObj["bank"].toObject());
            copyJsonObjectValue(bankObj, msgBankObj);

            bankJson["bank"] = bankObj;

            if (verboseLogs)
            {
                puts(QJsonDocument(msgBankObj).toJson().constData());
            }

            handleBankChanges(msgBankObj);
            saveBankLater();
        }
    }

    // ----------------------------------------------------------------------------------------------------------------

    void handleBankChanges(const QJsonObject& bankObj)
    {
        const QJsonObject presets(bankObj["presets"].toObject());
        for (const QString& presetid : presets.keys())
        {
            const int presetidi = presetid.toInt() - 1;

            if (presetidi < 0 || presetidi >= NUM_PRESETS_PER_BANK)
                continue;

            const QJsonObject preset(presets[presetid].toObject());
            Preset& presetdata(_presets[presetidi]);

            // if we are changing the current preset, send changes to mod-host
            const bool islive = _current.preset == presetidi;

            const QJsonObject blocks(preset["blocks"].toObject());
            for (const QString& blockid : blocks.keys())
            {
                const int blockidi = blockid.toInt() - 1;
                if (blockidi < 0 || blockidi >= NUM_BLOCKS_PER_PRESET)
                    continue;

                // TODO
                const QJsonObject block(blocks[blockid].toObject());
                Block& blockdata(presetdata.chain[0].blocks[blockidi]);

                printf("DEBUG: now handling block %d\n", blockidi);

                if (block.contains("uri"))
                {
                    const std::string uri = block["uri"].toString().toStdString();
                    blockdata.uri = uri;

                    if (islive)
                        replaceBlock(blockidi, uri.c_str());
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
                        const int parameteridi = parameterid.toInt() - 1;
                        if (parameteridi < 0 || parameteridi >= MAX_PARAMS_PER_BLOCK)
                            continue;

                        const QJsonObject parameter(parameters[parameterid].toObject());
                        Parameter& parameterdata(blockdata.parameters[parameteridi]);

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
                                _host.param_set(blockidi, symbol.c_str(), value);
                            }
                        }
                    }
                }
            }
        }

        // puts(QJsonDocument(blocks).toJson().constData());
    }

    // ----------------------------------------------------------------------------------------------------------------
    // delayed save handling

    bool bankChangedRecently = false;

    void saveBankLater()
    {
        if (bankChangedRecently)
            return;

        bankChangedRecently = true;
        QTimer::singleShot(1000, this, &WebSocketConnector::slot_saveBankNow);
    }

private slots:
    void slot_saveBankNow()
    {
        bankChangedRecently = false;

        QFile bankFile("bank.json");
        if (bankFile.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text))
            bankFile.write(QJsonDocument(bankJson["bank"].toObject()).toJson());
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
