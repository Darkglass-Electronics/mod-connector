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
    QJsonObject stateJson;
    struct {
        int bank = 0;
        struct {
            struct {
                QString uri = "-";
                struct {
                    QString symbol = "-";
                    float value;
                } parameters[6];
            } chain[6];
        } banks[6];
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
                for (uint32_t i=0; i<pcount; ++i)
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

            if (verboseLogs)
            {
                puts(QJsonDocument(stateObj).toJson().constData());
            }

            const QJsonObject msgStateObj(msgObj["state"].toObject());
            copyJsonObjectValue(stateObj, msgStateObj);

            stateJson["state"] = stateObj;

            handleStateChanges(msgStateObj);
            saveStateLater();
        }
    }

    // load state as saved in the `current` struct
    void loadCurrent()
    {
        host.remove(-1);

        const auto& bankdata(current.banks[current.bank]);

        for (int c=0; c<6; ++c)
        {
            const auto& chaindata(bankdata.chain[c]);
            if (chaindata.uri == "-")
                continue;
            host.add(chaindata.uri.toUtf8().constData(), c);

            for (int p=0; p<6; ++p)
            {
                const auto& parameterdata(chaindata.parameters[p]);
                if (parameterdata.symbol == "-")
                    continue;
                host.param_set(c, parameterdata.symbol.toUtf8().constData(), parameterdata.value);
            }
        }

        hostConnectBetweenBlocks();
    }

    // common function to connect all the blocks as needed
    // TODO cleanup duplicated code with function below
    void hostConnectBetweenBlocks()
    {
        const auto& bankdata(current.banks[current.bank]);

        bool loaded[6];
        for (int c = 0; c < 6; ++c)
            loaded[c] = bankdata.chain[c].uri != "-";

        // first plugin
        for (int c = 0; c < 6; ++c)
        {
            if (! loaded[c])
                continue;

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.chain[c].uri.toUtf8().constData()))
            {
                size_t srci = 0;
                for (size_t i = 0; i < plugin->ports.size(); ++i)
                {
                    if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                        continue;

                    ++srci;
                    const QString origin(QString("system:capture_%1").arg(srci));
                    const QString target(QString("effect_%1:%2").arg(c).arg(plugin->ports[i].symbol.c_str()));
                    host.connect(origin.toUtf8().constData(), target.toUtf8().constData());
                }
            }

            break;
        }

        // last plugin
        for (int c = 5; c >= 0; --c)
        {
            if (! loaded[c])
                continue;

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.chain[c].uri.toUtf8().constData()))
            {
                size_t dsti = 0;
                for (size_t i = 0; i < plugin->ports.size(); ++i)
                {
                    if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                        continue;

                    ++dsti;
                    const QString origin(QString("effect_%1:%2").arg(c).arg(plugin->ports[i].symbol.c_str()));
                    const QString target(QString("mod-monitor:in_%1").arg(dsti));
                    host.connect(origin.toUtf8().constData(), target.toUtf8().constData());
                }
            }

            break;
        }

        // between plugins
        for (int c1 = 0; c1 < 5; ++c1)
        {
            if (! loaded[c1])
                continue;

            for (int c2 = c1 + 1; c2 < 6; ++c2)
            {
                if (! loaded[c2])
                    continue;

                const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(bankdata.chain[c1].uri.toUtf8().constData());
                const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(bankdata.chain[c2].uri.toUtf8().constData());

                if (plugin1 != nullptr && plugin2 != nullptr)
                {
                    size_t srci = 0;
                    for (size_t i = 0; i < plugin1->ports.size(); ++i)
                    {
                        if ((plugin1->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                            continue;

                        ++srci;
                        size_t dstj = 0;
                        for (size_t j = 0; j < plugin2->ports.size(); ++j)
                        {
                            if (plugin2->ports[j].flags & Lv2PortIsOutput)
                                continue;
                            if ((plugin2->ports[j].flags & Lv2PortIsAudio) == 0)
                                continue;

                            if (srci != ++dstj)
                                continue;

                            const QString origin(QString("effect_%1:%2").arg(c1).arg(plugin1->ports[i].symbol.c_str()));
                            const QString target(QString("effect_%1:%2").arg(c2).arg(plugin2->ports[j].symbol.c_str()));
                            host.connect(origin.toUtf8().constData(), target.toUtf8().constData());
                        }
                    }
                }

                break;
            }
        }
    }

    // disconnect everything around the new plugin, to prevent double connections
    // TODO cleanup duplicated code with function above
    // FIXME this logic can be made much better, but this is for now just a testing tool anyhow
    void hostDisconnectForNewBlock(const int chainidi)
    {
        const auto& bankdata(current.banks[current.bank]);

        bool loaded[6];
        for (int c = 0; c < 6; ++c)
            loaded[c] = bankdata.chain[c].uri != "-";
        loaded[chainidi] = false;

        // first plugin
        for (int c = 0; c < 6; ++c)
        {
            if (! loaded[c])
                continue;

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.chain[c].uri.toUtf8().constData()))
            {
                size_t srci = 0;
                for (size_t i = 0; i < plugin->ports.size(); ++i)
                {
                    if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                        continue;

                    ++srci;
                    const QString origin(QString("system:capture_%1").arg(srci));
                    const QString target(QString("effect_%1:%2").arg(c).arg(plugin->ports[i].symbol.c_str()));
                    host.disconnect(origin.toUtf8().constData(), target.toUtf8().constData());
                }
            }

            break;
        }

        // last plugin
        for (int c = 5; c >= 0; --c)
        {
            if (! loaded[c])
                continue;

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.chain[c].uri.toUtf8().constData()))
            {
                size_t dsti = 0;
                for (size_t i = 0; i < plugin->ports.size(); ++i)
                {
                    if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                        continue;

                    ++dsti;
                    const QString origin(QString("effect_%1:%2").arg(c).arg(plugin->ports[i].symbol.c_str()));
                    const QString target(QString("mod-monitor:in_%1").arg(dsti));
                    host.disconnect(origin.toUtf8().constData(), target.toUtf8().constData());
                }
            }

            break;
        }

        // between plugins
        for (int c1 = 0; c1 < 5; ++c1)
        {
            if (! loaded[c1])
                continue;

            for (int c2 = c1 + 1; c2 < 6; ++c2)
            {
                if (! loaded[c2])
                    continue;

                const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(bankdata.chain[c1].uri.toUtf8().constData());
                const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(bankdata.chain[c2].uri.toUtf8().constData());

                if (plugin1 != nullptr && plugin2 != nullptr)
                {
                    size_t srci = 0;
                    for (size_t i = 0; i < plugin1->ports.size(); ++i)
                    {
                        if ((plugin1->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                            continue;

                        ++srci;
                        size_t dstj = 0;
                        for (size_t j = 0; j < plugin2->ports.size(); ++j)
                        {
                            if (plugin2->ports[j].flags & Lv2PortIsOutput)
                                continue;
                            if ((plugin2->ports[j].flags & Lv2PortIsAudio) == 0)
                                continue;

                            if (srci != ++dstj)
                                continue;

                            const QString origin(QString("effect_%1:%2").arg(c1).arg(plugin1->ports[i].symbol.c_str()));
                            const QString target(QString("effect_%1:%2").arg(c2).arg(plugin2->ports[j].symbol.c_str()));
                            host.disconnect(origin.toUtf8().constData(), target.toUtf8().constData());
                        }
                    }
                }

                break;
            }
        }
    }

    void handleStateChanges(const QJsonObject& stateObj)
    {
        bool bankchanged = false;
        bool chainchanged = false;

        if (stateObj.contains("bank"))
        {
            const int newbank = stateObj["bank"].toInt() - 1;

            if (current.bank != newbank)
            {
                current.bank = newbank;
                bankchanged = true;
            }
        }

        const QJsonObject banks(stateObj["banks"].toObject());
        for (const QString& bankid : banks.keys())
        {
            const QJsonObject bank(banks[bankid].toObject());
            const int bankidi = bankid.toInt() - 1;
            auto& bankdata(current.banks[bankidi]);

            // if we are changing the current bank, send changes to mod-host
            const bool islive = !bankchanged && current.bank == bankidi;

            const QJsonObject chains(bank["chain"].toObject());
            for (const QString& chainid : chains.keys())
            {
                const QJsonObject chain(chains[chainid].toObject());
                const int chainidi = chainid.toInt() - 1;
                auto& chaindata(bankdata.chain[chainidi]);

                if (chain.contains("uri"))
                {
                    const QString uri = chain["uri"].toString();
                    chaindata.uri = uri;

                    if (islive)
                    {
                        chainchanged = true;
                        host.remove(chainidi);

                        if (uri != "-")
                        {
                            host.add(uri.toUtf8().constData(), chainidi);
                            hostDisconnectForNewBlock(chainidi);
                        }
                    }
                }

                if (chain.contains("parameters"))
                {
                    const QJsonObject parameters(chain["parameters"].toObject());
                    for (const QString& parameterid : parameters.keys())
                    {
                        const QJsonObject parameter(parameters[parameterid].toObject());
                        const int parameteridi = parameterid.toInt() - 1;
                        auto& parameterdata(chaindata.parameters[parameteridi]);

                        if (parameter.contains("symbol"))
                        {
                            const QString symbol = parameter["symbol"].toString();
                            parameterdata.symbol = symbol;
                        }

                        if (parameter.contains("value"))
                        {
                            const float value = parameter["value"].toDouble();
                            parameterdata.value = value;

                            if (islive)
                            {
                                const QString symbol = parameterdata.symbol;
                                host.param_set(chainidi, symbol.toUtf8().constData(), value);
                            }
                        }
                    }
                }
            }
        }

        // puts(QJsonDocument(chains).toJson().constData());

        if (bankchanged)
            loadCurrent();
        else if (chainchanged)
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
        QTimer::singleShot(1000, this, &Connector::slot_saveStateNow);
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
