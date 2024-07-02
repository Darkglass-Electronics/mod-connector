// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "connector.hpp"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>

// --------------------------------------------------------------------------------------------------------------------

Connector::Connector()
{
    if (! host.last_error.empty())
    {
        fprintf(stderr, "Failed to initialize host connection: %s\n", host.last_error.c_str());
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

// --------------------------------------------------------------------------------------------------------------------
// load state as saved in the `current` struct

void Connector::loadCurrent()
{
    host.remove(-1);

    const auto& bankdata(current.banks[current.bank]);

    for (int c = 0; c < NUM_BLOCKS_IN_BANK; ++c)
    {
        const auto& blockdata(bankdata.blocks[c]);
        if (blockdata.uri == "-")
            continue;
        host.add(blockdata.uri.toUtf8().constData(), c);

        for (int p = 0; p < NUM_PARAMS_PER_BLOCK; ++p)
        {
            const auto& parameterdata(blockdata.parameters[p]);
            if (parameterdata.symbol == "-")
                continue;
            host.param_set(c, parameterdata.symbol.toUtf8().constData(), parameterdata.value);
        }
    }

    hostConnectBetweenBlocks();
}

// --------------------------------------------------------------------------------------------------------------------
// common function to connect all the blocks as needed
// TODO cleanup duplicated code with function below

void Connector::hostConnectBetweenBlocks()
{
    const auto& bankdata(current.banks[current.bank]);

    bool loaded[NUM_BLOCKS_IN_BANK];
    for (int b = 0; b < NUM_BLOCKS_IN_BANK; ++b)
        loaded[b] = bankdata.blocks[b].uri != "-";

    // first plugin
    for (int b = 0; b < NUM_BLOCKS_IN_BANK; ++b)
    {
        if (! loaded[b])
            continue;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.blocks[b].uri.toUtf8().constData()))
        {
            size_t srci = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                    continue;

                ++srci;
                const QString origin(QString("system:capture_%1").arg(srci));
                const QString target(QString("effect_%1:%2").arg(b).arg(plugin->ports[i].symbol.c_str()));
                host.connect(origin.toUtf8().constData(), target.toUtf8().constData());
            }
        }

        break;
    }

    // last plugin
    for (int b = NUM_BLOCKS_IN_BANK - 1; b >= 0; --b)
    {
        if (! loaded[b])
            continue;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.blocks[b].uri.toUtf8().constData()))
        {
            size_t dsti = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                    continue;

                ++dsti;
                const QString origin(QString("effect_%1:%2").arg(b).arg(plugin->ports[i].symbol.c_str()));
                const QString target(QString("mod-monitor:in_%1").arg(dsti));
                host.connect(origin.toUtf8().constData(), target.toUtf8().constData());
            }
        }

        break;
    }

    // between plugins
    for (int b1 = 0; b1 < NUM_BLOCKS_IN_BANK - 1; ++b1)
    {
        if (! loaded[b1])
            continue;

        for (int b2 = b1 + 1; b2 < NUM_BLOCKS_IN_BANK; ++b2)
        {
            if (! loaded[b2])
                continue;

            const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(bankdata.blocks[b1].uri.toUtf8().constData());
            const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(bankdata.blocks[b2].uri.toUtf8().constData());

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

                        const QString origin(QString("effect_%1:%2").arg(b1).arg(plugin1->ports[i].symbol.c_str()));
                        const QString target(QString("effect_%1:%2").arg(b2).arg(plugin2->ports[j].symbol.c_str()));
                        host.connect(origin.toUtf8().constData(), target.toUtf8().constData());
                    }
                }
            }

            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------
// disconnect everything around the new plugin, to prevent double connections
// TODO cleanup duplicated code with function above
// FIXME this logic can be made much better, but this is for now just a testing tool anyhow

void Connector::hostDisconnectForNewBlock(const int blockidi)
{
    const auto& bankdata(current.banks[current.bank]);

    bool loaded[NUM_BLOCKS_IN_BANK];
    for (int b = 0; b < NUM_BLOCKS_IN_BANK; ++b)
        loaded[b] = bankdata.blocks[b].uri != "-";
    loaded[blockidi] = false;

    // first plugin
    for (int b = 0; b < NUM_BLOCKS_IN_BANK; ++b)
    {
        if (! loaded[b])
            continue;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.blocks[b].uri.toUtf8().constData()))
        {
            size_t srci = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                    continue;

                ++srci;
                const QString origin(QString("system:capture_%1").arg(srci));
                const QString target(QString("effect_%1:%2").arg(b).arg(plugin->ports[i].symbol.c_str()));
                host.disconnect(origin.toUtf8().constData(), target.toUtf8().constData());
            }
        }

        break;
    }

    // last plugin
    for (int b = NUM_BLOCKS_IN_BANK - 1; b >= 0; --b)
    {
        if (! loaded[b])
            continue;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(bankdata.blocks[b].uri.toUtf8().constData()))
        {
            size_t dsti = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                    continue;

                ++dsti;
                const QString origin(QString("effect_%1:%2").arg(b).arg(plugin->ports[i].symbol.c_str()));
                const QString target(QString("mod-monitor:in_%1").arg(dsti));
                host.disconnect(origin.toUtf8().constData(), target.toUtf8().constData());
            }
        }

        break;
    }

    // between plugins
    for (int b1 = 0; b1 < NUM_BLOCKS_IN_BANK - 1; ++b1)
    {
        if (! loaded[b1])
            continue;

        for (int b2 = b1 + 1; b2 < NUM_BLOCKS_IN_BANK; ++b2)
        {
            if (! loaded[b2])
                continue;

            const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(bankdata.blocks[b1].uri.toUtf8().constData());
            const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(bankdata.blocks[b2].uri.toUtf8().constData());

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

                        const QString origin(QString("effect_%1:%2").arg(b1).arg(plugin1->ports[i].symbol.c_str()));
                        const QString target(QString("effect_%1:%2").arg(b2).arg(plugin2->ports[j].symbol.c_str()));
                        host.disconnect(origin.toUtf8().constData(), target.toUtf8().constData());
                    }
                }
            }

            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void Connector::handleStateChanges(const QJsonObject& stateObj)
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

        // if we are changing the current bank, send changes to mod-host
        const bool islive = !bankchanged && current.bank == bankidi;

        printf("DEBUG: now handling bank %d, live %d\n", bankidi, islive);

        const QJsonObject blocks(bank["blocks"].toObject());
        for (const QString& blockid : blocks.keys())
        {
            const QJsonObject block(blocks[blockid].toObject());
            const int blockidi = blockid.toInt() - 1;
            auto& blockdata(bankdata.blocks[blockidi]);

            printf("DEBUG: now handling block %d\n", blockidi);

            if (block.contains("uri"))
            {
                const QString uri = block["uri"].toString();
                blockdata.uri = uri;

                if (islive)
                {
                    blockschanged = true;
                    host.remove(blockidi);

                    if (uri != "-")
                    {
                        if (host.add(uri.toUtf8().constData(), blockidi))
                            printf("DEBUG: block %d loaded plugin %s\n", blockidi, uri.toUtf8().constData());
                        else
                            printf("DEBUG: block %d failed loaded plugin %s: %s\n",
                                    blockidi, uri.toUtf8().constData(), host.last_error.c_str());

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
                            host.param_set(blockidi, symbol.toUtf8().constData(), value);
                        }
                    }
                }
            }
        }
    }

    // puts(QJsonDocument(blocks).toJson().constData());

    if (bankchanged)
        loadCurrent();
    else if (blockschanged)
        hostConnectBetweenBlocks();
}

// --------------------------------------------------------------------------------------------------------------------
