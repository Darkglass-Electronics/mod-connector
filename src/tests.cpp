// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "config.h"
#include <cstdint>
#include <string>
#define MOD_LOG_GROUP "tests"

#define MONOBLOCK "urn:mod-connector:test1in1out"
#define STEREOBLOCK "urn:mod-connector:test2in2out"
#define SIDEOUTBLOCK "urn:mod-connector:testsideout"
#define SIDEINBLOCK "urn:mod-connector:testsidein"

#include "connector.hpp"
#include "utils.hpp"

#include <jack/jack.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QTimer>

// --------------------------------------------------------------------------------------------------------------------

constexpr const char* getProcessErrorAsString(QProcess::ProcessError error)
{
    switch (error)
    {
    case QProcess::FailedToStart:
        return "Process failed to start";
    case QProcess::Crashed:
        return "Process crashed";
    case QProcess::Timedout:
        return "Process timed out";
    case QProcess::WriteError:
        return "Process write error";
    default:
        return "Unkown error";
    }
}
class HostProcess : public QProcess
{
    bool closing = false;

public:
    HostProcess() : QProcess()
    {
        setProcessChannelMode(QProcess::ForwardedChannels);
        setProgram(QDir::homePath() + "/mod-host/mod-host");
        setArguments({"-p", "5555", "-f", "5556", "-v"});

        QObject::connect(this, &QProcess::errorOccurred, [this](const QProcess::ProcessError error) {
            if (closing)
                return;

            mod_log_warn("Could not start MOD Host: %s", getProcessErrorAsString(error));
            QCoreApplication::exit(1);
        });
    }

    void terminate()
    {
        closing = true;
        QProcess::terminate();

        if (! waitForFinished(500))
            kill();
    }

public slots:
    void startSlot()
    {
        start();
    }
};

// --------------------------------------------------------------------------------------------------------------------

static QStringList q_jack_get_ports(jack_client_t* const client,
                                                  const char* const port_name_pattern = nullptr,
                                                  const char* const type_name_pattern = nullptr,
                                                  const unsigned long flags = 0)
{
    if (const char** const ports = jack_get_ports(client, port_name_pattern, type_name_pattern, flags))
    {
        QStringList ret;

        for (int i = 0; ports[i] != nullptr; ++i)
            ret.append(ports[i]);

        jack_free(ports);

        return ret;
    }

    return {};
}

static QStringList q_jack_port_get_all_connections(jack_client_t* const client, const std::string& port_name)
{
    if (jack_port_t* const port = jack_port_by_name(client, port_name.c_str()))
    {
        if (const char** const ports = jack_port_get_all_connections(client, port))
        {
            QStringList ret;

            for (int i = 0; ports[i] != nullptr; ++i)
                ret.append(ports[i]);

            jack_free(ports);

            return ret;
        }
    }

    return {};
}

class HostConnectorTests : public QObject
{
    jack_client_t* const client;
    HostProcess& hostProcess;
    HostConnector connector;
    uint retryAttempt = 0;

    // return true if all tests pass
    bool hostReady()
    {
        mod_log_info("hostReady started, begginning tests...");

        // NOTE do any custom setup here as necessary (tools, virtualparams, connections)

        // report to connector we are ready to roll!
        connector.hostReady();

        // ensure our required ports exist
        {
            const QStringList allports = q_jack_get_ports(client);
            assert_return(allports.contains(JACK_CAPTURE_PORT_1), false);
            assert_return(allports.contains(JACK_CAPTURE_PORT_2), false);
            assert_return(allports.contains(JACK_PLAYBACK_PORT_1), false);
            assert_return(allports.contains(JACK_PLAYBACK_PORT_2), false);
            assert_return(allports.contains(JACK_PLAYBACK_MONITOR_PORT_1), false);
            assert_return(allports.contains(JACK_PLAYBACK_MONITOR_PORT_2), false);
        }

        // ensure our required plugins exist
        assert_return(connector.lv2world.get_plugin_by_uri(MONOBLOCK) != nullptr, false);
        assert_return(connector.lv2world.get_plugin_by_uri(STEREOBLOCK) != nullptr, false);
        assert_return(connector.lv2world.get_plugin_by_uri(SIDEOUTBLOCK) != nullptr, false);
        assert_return(connector.lv2world.get_plugin_by_uri(SIDEINBLOCK) != nullptr, false);

        // initial empty bank load
        {
            const std::array<std::string, 3> filenames = {
                "1.json",
                "2.json",
                "3.json",
            };
            connector.loadBankFromPresetFiles(filenames);
        }

        // test pass-through connections
        assert_return(testPassthrough(), false);

        // test loading single plugin
        assert_return(testPluginLoad(), false);
        // check return to pass-through connections
        assert_return(testPassthrough(), false);

        // test mono chain actions
        assert_return(testSingleMonoChain(), false);
        // check return to pass-through state
        assert_return(testPassthrough(), false);

        // test stereo chain actions
        assert_return(testSingleStereoChain(), false);
        // check return to pass-through state
        assert_return(testPassthrough(), false);

        // test side chain management
        assert_return(testSideChain(), false);
        // check return to pass-through state
        assert_return(testPassthrough(), false);

        mod_log_info("SUCCESS: All tests finished successfully!");

        return true;
    }

    // ensure pass-through connections
    bool testPassthrough()
    {
        QStringList connections;

        // NOTE capture ports will be monitored, so they are connected to more than 1 port
        connections = q_jack_port_get_all_connections(client, JACK_CAPTURE_PORT_1);
        for (auto& conn : connections) {
            mod_log_debug(JACK_CAPTURE_PORT_1 " connections: %s", conn.toUtf8().constData());
        }
        assert_return(connections.contains(JACK_PLAYBACK_PORT_1), false);

        connections = q_jack_port_get_all_connections(client, JACK_CAPTURE_PORT_2);
        for (auto& conn : connections) {
            mod_log_debug(JACK_CAPTURE_PORT_2 " connections: %s", conn.toUtf8().constData());
        }
        assert_return(connections.contains(JACK_PLAYBACK_PORT_2), false);

        connections = q_jack_port_get_all_connections(client, JACK_PLAYBACK_PORT_1);
        for (auto& conn : connections) {
            mod_log_debug(JACK_PLAYBACK_PORT_1 " connections: %s", conn.toUtf8().constData());
        }
        assert_return(connections == QStringList({ JACK_CAPTURE_PORT_1 }), false);

        connections = q_jack_port_get_all_connections(client, JACK_PLAYBACK_PORT_2);
        for (auto& conn : connections) {
            mod_log_debug(JACK_PLAYBACK_PORT_2 " connections: %s", conn.toUtf8().constData());
        }
        assert_return(connections == QStringList({ JACK_CAPTURE_PORT_2 }), false);

        return true;
    }

    // ensure no system input to system output connections
    bool testNoPassthrough()
    {
        QStringList connections;

        connections = q_jack_port_get_all_connections(client, JACK_CAPTURE_PORT_1);
        assert_return(!connections.contains(JACK_PLAYBACK_PORT_1), false);
        assert_return(!connections.contains(JACK_PLAYBACK_PORT_2), false);

        connections = q_jack_port_get_all_connections(client, JACK_CAPTURE_PORT_2);
        assert_return(!connections.contains(JACK_PLAYBACK_PORT_1), false);
        assert_return(!connections.contains(JACK_PLAYBACK_PORT_2), false);

        connections = q_jack_port_get_all_connections(client, JACK_PLAYBACK_PORT_1);
        assert_return(!connections.contains(JACK_CAPTURE_PORT_1), false);
        assert_return(!connections.contains(JACK_CAPTURE_PORT_2), false);

        connections = q_jack_port_get_all_connections(client, JACK_PLAYBACK_PORT_2);
        assert_return(!connections.contains(JACK_CAPTURE_PORT_1), false);
        assert_return(!connections.contains(JACK_CAPTURE_PORT_2), false);

        return true;
    }

    // test loading each individual test block
    bool testPluginLoad()
    {
        // MONOBLOCK
        // load plugin
        assert_return(connector.replaceBlock(0, 0, MONOBLOCK), false);
        // ensure our single plugin is connected properly
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // remove plugin
        assert_return(connector.replaceBlock(0, 0, nullptr), false);
    
        // STEREOBLOCK
        // load plugin
        assert_return(connector.replaceBlock(0, 0, STEREOBLOCK), false);
        // ensure our single plugin is connected properly
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 0), JACK_CAPTURE_PORT_2), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 0), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 0), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // remove plugin
        assert_return(connector.replaceBlock(0, 0, nullptr), false);

        // SIDEOUTBLOCK
        // load plugin
        assert_return(connector.replaceBlock(0, 0, SIDEOUTBLOCK), false);
        // ensure our single plugin is connected properly
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // SIDEINBLOCK AFTER SIDEOUTBLOCK -> create sidechain
        // load plugin
        assert_return(connector.replaceBlock(0, 1, SIDEINBLOCK), false);
        // ensure our single plugin is connected properly
        // row 1 inter-connection
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 0), blockPortIn1(0, 1)), false);
        // row 2 inter-connection
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 0), blockPortIn2(0, 1)), false);
        // output
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // remove plugins (sidechain input first)
        assert_return(connector.replaceBlock(0, 1, nullptr), false);
        assert_return(connector.replaceBlock(0, 0, nullptr), false);

        return true;
    }

    // test adding, reordering and removing on a single-row mono chain
    bool testSingleMonoChain() 
    {
        // add block to slot 1
        assert_return(connector.replaceBlock(0, 1, MONOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        
        // add another block to slot 2
        assert_return(connector.replaceBlock(0, 2, MONOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 2), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // add another block to slot 4
        assert_return(connector.replaceBlock(0, 4, MONOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // add another block to slot 5
        assert_return(connector.replaceBlock(0, 5, MONOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 4), blockPortIn1(0, 5)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // move block 4 to empty slot 3
        assert_return(connector.reorderBlock(0, 4, 3), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 3)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 5)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // move first block last
        assert_return(connector.reorderBlock(0, 1, 5), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 4), blockPortIn1(0, 5)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove plugin from end
        assert_return(connector.replaceBlock(0, 5, nullptr), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove plugin from middle
        assert_return(connector.replaceBlock(0, 2, nullptr), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove plugin from start
        assert_return(connector.replaceBlock(0, 1, nullptr), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 4), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove last remaining plugin
        assert_return(connector.replaceBlock(0, 4, nullptr), false);

        return true;
    }

    // test adding, reordering and removing on a single-row mixed mono/stereo/dualmono chain
    bool testSingleStereoChain() 
    {
        // stereo block to slot 0
        assert_return(connector.replaceBlock(0, 0, STEREOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 0), JACK_CAPTURE_PORT_2), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 0), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 0), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // basic stereo block system IO OK

        // replace stereo block with mono block (slot 0)
        assert_return(connector.replaceBlock(0, 0, MONOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // replacing stereo with mono block OK

        // add stereo block to slot 1
        assert_return(connector.replaceBlock(0, 1, STEREOBLOCK), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->1
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), blockPortIn1(0, 1), blockPortIn2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), blockPortOut1(0, 0)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 1), blockPortOut1(0, 0)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 1), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // basic mono to stereo OK

        // add stereo block to slot 2
        assert_return(connector.replaceBlock(0, 2, STEREOBLOCK), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->1
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), blockPortIn1(0, 1), blockPortIn2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), blockPortOut1(0, 0)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 1), blockPortOut1(0, 0)), false);
        // connections 1<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 2)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 2), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 2), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // basic stereo to stereo OK

        // add a (dual) mono block to slot 3
        assert_return(connector.replaceBlock(0, 3, MONOBLOCK), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->1
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), blockPortIn1(0, 1), blockPortIn2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), blockPortOut1(0, 0)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 1), blockPortOut1(0, 0)), false);
        // connections 1<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 3)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 3)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 3), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 3), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // basic stereo to dual mono OK

        // add another (dual) mono block to slot 4
        assert_return(connector.replaceBlock(0, 4, MONOBLOCK), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->1
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), blockPortIn1(0, 1), blockPortIn2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), blockPortOut1(0, 0)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 1), blockPortOut1(0, 0)), false);
        // connections 1<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 3)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 3)), false);
        // connections 3<->4
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 3), blockPairPortIn1(0, 4)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // basic dual mono to dual mono OK

        // add a stereo block to slot 5
        assert_return(connector.replaceBlock(0, 5, STEREOBLOCK), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->1
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), blockPortIn1(0, 1), blockPortIn2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), blockPortOut1(0, 0)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 1), blockPortOut1(0, 0)), false);
        // connections 1<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 3)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 3)), false);
        // connections 3<->4
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 3), blockPairPortIn1(0, 4)), false);
        // connections 4<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 4), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 4), blockPortIn2(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // basic dual mono to stereo OK

        // move mono block from beginning to end
        // chain becomes: stereo - stereo - dual mono - dual mono - stereo - dual mono
        assert_return(connector.reorderBlock(0, 0, 5), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 0), JACK_CAPTURE_PORT_2), false);
        // connections 0<->1
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 0), blockPortIn1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 0), blockPortIn2(0, 1)), false);
        // connections 1<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPairPortIn1(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 3)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 2), blockPairPortIn1(0, 3)), false);
        // connections 3<->4
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 3), blockPortIn2(0, 4)), false);
        // connections 4<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 4), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 4), blockPairPortIn1(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // making a mono block dual mono by reorder OK
        // moving stereo block first by reorder OK

        // change places of stereo and mono block (1 and 2)
        // chain becomes: stereo - dual mono - stereo- dual mono - stereo - dual mono
        assert_return(connector.reorderBlock(0, 1, 2), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 0), JACK_CAPTURE_PORT_2), false);
        // connections 0<->1
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 0), blockPortIn1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 0), blockPairPortIn1(0, 1)), false);
        // connections 1<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 1), blockPortIn2(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 3)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 3)), false);
        // connections 3<->4
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 3), blockPortIn2(0, 4)), false);
        // connections 4<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 4), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 4), blockPairPortIn1(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // move mono block 3 first
        // chain becomes: mono - stereo - dual mono - stereo - stereo - dual mono
        assert_return(connector.reorderBlock(0, 3, 0), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->1
        assert_return(checkOnly2Connections(blockPortOut1(0, 0), blockPortIn1(0, 1), blockPortIn2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), blockPortOut1(0, 0)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 1), blockPortOut1(0, 0)), false);
        // connections 1<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPairPortIn1(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 3)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 2), blockPortIn2(0, 3)), false);
        // connections 3<->4
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 3), blockPortIn2(0, 4)), false);
        // connections 4<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 4), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 4), blockPairPortIn1(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove stereo block 1 to make block 2 mono
        // chain becomes: mono - empty - mono - stereo - stereo - dual mono
        assert_return(connector.replaceBlock(0, 1, nullptr), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 0), blockPortIn1(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnly2Connections(blockPortOut1(0, 2), blockPortIn1(0, 3), blockPortIn2(0, 3)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 3), blockPortOut1(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 3), blockPortOut1(0, 2)), false);
        // connections 3<->4
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 3), blockPortIn2(0, 4)), false);
        // connections 4<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 4), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 4), blockPairPortIn1(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove stereo block 4
        // chain becomes: mono - empty - mono - stereo - empty - dual mono
        assert_return(connector.replaceBlock(0, 4, nullptr), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        // connections 0<->2
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 0), blockPortIn1(0, 2)), false);
        // connections 2<->3
        assert_return(checkOnly2Connections(blockPortOut1(0, 2), blockPortIn1(0, 3), blockPortIn2(0, 3)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 3), blockPortOut1(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 3), blockPortOut1(0, 2)), false);
        // connections 3<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 3), blockPairPortIn1(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove mono blocks 0 and 2 from beginning
        // chain becomes: empty - empty - empty - stereo - empty - dual mono
        assert_return(connector.replaceBlock(0, 0, nullptr), false);
        assert_return(connector.replaceBlock(0, 2, nullptr), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 3), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 3), JACK_CAPTURE_PORT_2), false);
        // connections 3<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 3), blockPairPortIn1(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // reorder with empty slots 
        // chain becomes: empty - empty - stereo - empty - empty - dual mono
        assert_return(connector.reorderBlock(0, 1, 3), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), JACK_CAPTURE_PORT_2), false);
        // connections 2<->5
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 5)), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove dual mono from end 
        // chain becomes: empty - empty - stereo - empty - empty - empty
        assert_return(connector.replaceBlock(0, 5, nullptr), false);
        // system in
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), JACK_CAPTURE_PORT_2), false);
        // system out
        assert_return(checkOnlyConnection(blockPortOut1(0, 2), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 2), JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);

        // remove remaining block
        assert_return(connector.replaceBlock(0, 2, nullptr), false);

        return true;
    }

    // test building 2-row (sidechain) setup from left to right (and dismantling right to left)
    bool testSideChainBuiltInOrder() 
    {
        // branch to sidechain
        assert_return(connector.replaceBlock(0, 1, SIDEOUTBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(checkNoConnections(blockPortOut2(0, 1)), false); // no sidechain
        assert_return(testNoPassthrough(), false);

        // add a block on non-finished sidechain
        assert_return(connector.replaceBlock(1, 2, MONOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn1(1, 2)), false);
        assert_return(checkNoConnections(blockPortOut1(1, 2)), false);
        assert_return(testNoPassthrough(), false);

        // complete sidechain
        assert_return(connector.replaceBlock(0, 4, SIDEINBLOCK), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn1(1, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // remove sidein block (undo sidechain ending)
        assert_return(connector.replaceBlock(0, 4, nullptr), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn1(1, 2)), false);
        assert_return(checkNoConnections(blockPortOut1(1, 2)), false);
        assert_return(testNoPassthrough(), false);

        // replace sidechain block with a stereo one
        assert_return(connector.replaceBlock(1, 2, STEREOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkNoConnections(blockPortOut1(1, 2)), false);
        assert_return(checkNoConnections(blockPortOut2(1, 2)), false);
        assert_return(testNoPassthrough(), false);

        // complete sidechain
        assert_return(connector.replaceBlock(0, 4, SIDEINBLOCK), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 4), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(1, 2), blockPairPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // remove sidein block (undo sidechain ending)
        assert_return(connector.replaceBlock(0, 4, nullptr), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkNoConnections(blockPortOut1(1, 2)), false);
        assert_return(checkNoConnections(blockPortOut2(1, 2)), false);
        assert_return(testNoPassthrough(), false);

        // remove stereo block from sidechain
        assert_return(connector.replaceBlock(1, 2, nullptr), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(checkNoConnections(blockPortOut2(0, 1)), false); // no sidechain
        assert_return(testNoPassthrough(), false);

        // add stereo block on row 0
        assert_return(connector.replaceBlock(0, 2, STEREOBLOCK), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 2), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 2), JACK_PLAYBACK_PORT_2), false);
        // row 1
        assert_return(checkNoConnections(blockPortOut2(0, 1)), false); // no sidechain
        assert_return(testNoPassthrough(), false);

        // complete sidechain
        assert_return(connector.replaceBlock(0, 4, SIDEINBLOCK), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn2(0, 4), blockPairPortIn2(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 4), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn2(0, 4), blockPortOut2(0, 1)), false);
        assert_return(testNoPassthrough(), false);

        // remove sidein block (undo sidechain ending)
        assert_return(connector.replaceBlock(0, 4, nullptr), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 2), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 2), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkNoConnections(blockPortOut2(0, 1)), false); // no sidechain
        assert_return(testNoPassthrough(), false);
        
        // remove remaining stereo block
        assert_return(connector.replaceBlock(0, 2, nullptr), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(checkNoConnections(blockPortOut2(0, 1)), false); // no sidechain
        assert_return(testNoPassthrough(), false);

        // add back on both rows
        assert_return(connector.replaceBlock(0, 2, STEREOBLOCK), false);
        assert_return(connector.replaceBlock(1, 2, STEREOBLOCK), false);
        // row 0
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 2), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 2), JACK_PLAYBACK_PORT_2), false);
        // row 1
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkNoConnections(blockPortOut1(1, 2)), false);
        assert_return(checkNoConnections(blockPortOut2(1, 2)), false);
        assert_return(testNoPassthrough(), false);

        // complete sidechain
        assert_return(connector.replaceBlock(0, 4, SIDEINBLOCK), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(1, 2), blockPairPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // remove sidechain completion
        assert_return(connector.replaceBlock(0, 4, nullptr), false);
        // row 0
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 2), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 2), JACK_PLAYBACK_PORT_2), false);
        // row 1
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkNoConnections(blockPortOut1(1, 2)), false);
        assert_return(checkNoConnections(blockPortOut2(1, 2)), false);
        assert_return(testNoPassthrough(), false);

        // remove remaining blocks
        assert_return(connector.replaceBlock(1, 2, nullptr), false);
        assert_return(connector.replaceBlock(0, 2, nullptr), false);
        assert_return(connector.replaceBlock(0, 1, nullptr), false);

        return true;
    }

    // test adding stereo blocks to an existing 2-row (sidechaining) setup
    bool testSideChainAddStereoInBetween() 
    {
        // create sidechain (these actions tested in testPluginLoad())
        assert_return(connector.replaceBlock(0, 1, SIDEOUTBLOCK), false);
        assert_return(connector.replaceBlock(0, 4, SIDEINBLOCK), false);

        // add stereo block in between the blocks on row 0
        assert_return(connector.replaceBlock(0, 2, STEREOBLOCK), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn2(0, 4), blockPairPortIn2(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 4), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn2(0, 4), blockPortOut2(0, 1)), false);
        assert_return(testNoPassthrough(), false);

        // remove the added block
        assert_return(connector.replaceBlock(0, 2, nullptr), false);
        // row 0
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // row 1 
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 4)), false);

        // add stereo block in between the blocks on row 1
        assert_return(connector.replaceBlock(1, 2, STEREOBLOCK), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 4), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(1, 2), blockPairPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // remove the added block
        assert_return(connector.replaceBlock(1, 2, nullptr), false);
        // row 0
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1, JACK_PLAYBACK_PORT_2), false);
        assert_return(testNoPassthrough(), false);
        // row 1 
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 4)), false);

        // add stereo block in both rows
        assert_return(connector.replaceBlock(0, 2, STEREOBLOCK), false);
        assert_return(connector.replaceBlock(1, 2, STEREOBLOCK), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(1, 2), blockPairPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // remove stereo block from row 0
        assert_return(connector.replaceBlock(0, 2, nullptr), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 4), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(1, 2), blockPairPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // add previous back and remove the one from row 1
        assert_return(connector.replaceBlock(0, 2, STEREOBLOCK), false);
        assert_return(connector.replaceBlock(1, 2, nullptr), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn2(0, 4), blockPairPortIn2(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 4), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn2(0, 4), blockPortOut2(0, 1)), false);
        assert_return(testNoPassthrough(), false);
        
        // remove remaining blocks
        assert_return(connector.replaceBlock(0, 2, nullptr), false);
        assert_return(connector.replaceBlock(0, 4, nullptr), false);
        assert_return(connector.replaceBlock(0, 1, nullptr), false);

        return true;
    }

    // test moving a stereo block around and within sidechaining setup
    // (only on row 0, meaning using only replaceBlock and reorderBlock)
    bool testSideChainMoveStereoOnFirstRow() 
    {
        // create sidechain (these actions tested in testPluginLoad())
        assert_return(connector.replaceBlock(0, 1, SIDEOUTBLOCK), false);
        assert_return(connector.replaceBlock(0, 4, SIDEINBLOCK), false);

        // add stereo block to end
        assert_return(connector.replaceBlock(0, 5, STEREOBLOCK), false);
        // row 0 
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), blockPortIn1(0, 5), blockPortIn2(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 5), blockPortOut1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 5), blockPortOut1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 5), JACK_PLAYBACK_PORT_2), false);
        // row 1 
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // move stereo block in between sidechaining blocks on row 0
        // SIDEINBLOCK 4 becomes block 5
        assert_return(connector.reorderBlock(0, 5, 2), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 2), blockPortIn2(0, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 2), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPairPortIn1(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn2(0, 5), blockPairPortIn2(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 5), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn2(0, 5), blockPortOut2(0, 1)), false);
        assert_return(testNoPassthrough(), false);

        // move stereo block before sidechaining blocks on row 0
        // SIDEOUTBLOCK 1 becomes block 2
        assert_return(connector.reorderBlock(0, 2, 0), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 0), JACK_CAPTURE_PORT_2), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 0), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 0), blockPairPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 2), blockPairPortIn1(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPortIn2(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut2(0, 2), blockPairPortIn2(0, 5)), false);
        assert_return(testNoPassthrough(), false);

        // move stereo block back inside sidechaining blocks on row 0
        // SIDEOUTBLOCK 2 becomes block 1
        assert_return(connector.reorderBlock(0, 0, 3), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 3), blockPortIn2(0, 3)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 3), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 3), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 3), blockPairPortIn1(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn2(0, 5), blockPairPortIn2(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 5), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn2(0, 5), blockPortOut2(0, 1)), false);
        assert_return(testNoPassthrough(), false);

        // move stereo block to end
        assert_return(connector.reorderBlock(0, 3, 5), false);
        // row 0 
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 1), blockPortIn1(0, 4)), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 4), blockPortIn1(0, 5), blockPortIn2(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 5), blockPortOut1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 5), blockPortOut1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortOut2(0, 5), JACK_PLAYBACK_PORT_2), false);
        // row 1 
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 1), blockPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // remove all blocks
        assert_return(connector.replaceBlock(0, 5, nullptr), false);
        assert_return(connector.replaceBlock(0, 4, nullptr), false);
        assert_return(connector.replaceBlock(0, 1, nullptr), false);

        return true;
    }

    // test moving a stereo block around on both rows of a 2-row setup
    bool testSideChainSwapBlockRows()
    {
        // NOTE: swapBlockRow is called here for blocks at the end of the two rows,
        // meaning that block is first moved to the end, row swapped and then moved to destination index

        // create sidechain (these actions tested in testPluginLoad())
        assert_return(connector.replaceBlock(0, 1, SIDEOUTBLOCK), false);
        assert_return(connector.replaceBlock(0, 4, SIDEINBLOCK), false);
        // add stereo block to end
        assert_return(connector.replaceBlock(0, 5, STEREOBLOCK), false);
        // connections of this starting point checked in testSideChainMoveStereoOnFirstRow()

        // move stereo block in between sidechaining blocks on row 1
        assert_return(connector.swapBlockRow(0, 5, 1, 5), false);
        assert_return(connector.reorderBlock(1, 5, 2), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 4), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(1, 2), blockPairPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // move stereo block to beginning on row 0
        assert_return(connector.reorderBlock(1, 2, 5), false);
        assert_return(connector.swapBlockRow(1, 5, 0, 5), false);
        assert_return(connector.reorderBlock(0, 5, 0), false);
        // SIDEOUTBLOCK 1 becomes block 2
        // SIDEINBLOCK 4 becomes block 5
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 0), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 0), JACK_CAPTURE_PORT_2), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 0), blockPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 0), blockPairPortIn1(0, 2)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 2), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut1(0, 2), blockPairPortIn1(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 2), blockPortIn2(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPairPortOut2(0, 2), blockPairPortIn2(0, 5)), false);
        assert_return(testNoPassthrough(), false);

        // move stereo block back to where it was on row 1
        assert_return(connector.reorderBlock(0, 0, 5), false);
        assert_return(connector.swapBlockRow(0, 5, 1, 5), false);
        assert_return(connector.reorderBlock(1, 5, 2), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 4), blockPairPortIn1(0, 4)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn1(0, 4), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 4), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 4), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn1(1, 2), blockPortIn2(1, 2)), false);
        assert_return(checkOnlyConnection(blockPortIn1(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(1, 2), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(1, 2), blockPortIn2(0, 4)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(1, 2), blockPairPortIn2(0, 4)), false);
        assert_return(testNoPassthrough(), false);

        // move stereo block to row 0 between the sidechaining blocks
        assert_return(connector.reorderBlock(1, 2, 5), false);
        assert_return(connector.swapBlockRow(1, 5, 0, 5), false);
        assert_return(connector.reorderBlock(0, 5, 3), false);
        // row 0 connections
        assert_return(checkOnlyConnection(blockPortIn1(0, 1), JACK_CAPTURE_PORT_1), false);
        assert_return(checkOnly2Connections(blockPortOut1(0, 1), blockPortIn1(0, 3), blockPortIn2(0, 3)), false);
        assert_return(checkOnlyConnection(blockPortIn1(0, 3), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 3), blockPortOut1(0, 1)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut1(0, 3), blockPortIn1(0, 5)), false);
        assert_return(checkOnlyConnectionBothWays(blockPortOut2(0, 3), blockPairPortIn1(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortOut1(0, 5), JACK_PLAYBACK_PORT_1), false);
        assert_return(checkOnlyConnection(blockPairPortOut1(0, 5), JACK_PLAYBACK_PORT_2), false);
        // row 1 connections
        assert_return(checkOnly2Connections(blockPortOut2(0, 1), blockPortIn2(0, 5), blockPairPortIn2(0, 5)), false);
        assert_return(checkOnlyConnection(blockPortIn2(0, 5), blockPortOut2(0, 1)), false);
        assert_return(checkOnlyConnection(blockPairPortIn2(0, 5), blockPortOut2(0, 1)), false);
        assert_return(testNoPassthrough(), false);

        // remove all blocks
        assert_return(connector.replaceBlock(0, 5, nullptr), false);
        assert_return(connector.replaceBlock(0, 3, nullptr), false);
        assert_return(connector.replaceBlock(0, 1, nullptr), false);

        return true;
    }

    bool testSideChain() 
    {
        // NOTE: even though this test set is extensive, it doesn't include every possible scenario
        // focus is on issues that have appeared earlier in development

        // NOTE: many of the connection checks in between actions are the same within one test
        // and between test functions but the steps to get there are different

        assert_return(testSideChainBuiltInOrder(), false);
        assert_return(testPassthrough(), false);

        assert_return(testSideChainAddStereoInBetween(), false);
        assert_return(testPassthrough(), false);

        assert_return(testSideChainMoveStereoOnFirstRow(), false);
        assert_return(testPassthrough(), false);
        
        assert_return(testSideChainSwapBlockRows(), false);
        assert_return(testPassthrough(), false);

        return true;
    }


    // HELPERS

    bool checkNoConnections(std::string port_to_check)
    {
        QStringList connections;
        connections = q_jack_port_get_all_connections(client, port_to_check);
        return connections.size() == 0;
    }

    bool checkOnlyConnection(std::string port_to_check, const char* const only_port_connected_to)
    {
        QStringList connections;
        connections = q_jack_port_get_all_connections(client, port_to_check);
        return connections == QStringList({ only_port_connected_to });
    }

    bool checkOnlyConnection(std::string port_to_check, std::string only_port_connected_to)
    {
        return checkOnlyConnection(port_to_check, only_port_connected_to.c_str());
    }

    bool checkOnlyConnectionBothWays(std::string port_1, std::string port_2)
    {
        return checkOnlyConnection(port_1, port_2.c_str()) &&
               checkOnlyConnection(port_2, port_1.c_str());
    }

    bool checkOnly2Connections(std::string port_to_check, const char* const port_1_connected_to, const char* const port_2_connected_to)
    {
        QStringList connections;
        connections = q_jack_port_get_all_connections(client, port_to_check);
        return connections.contains(port_1_connected_to) &&
               connections.contains(port_2_connected_to) &&
               connections.size() == 2;
    }

    bool checkOnly2Connections(std::string port_to_check, std::string port_1_connected_to, std::string port_2_connected_to)
    {
        return checkOnly2Connections(port_to_check, port_1_connected_to.c_str(), port_2_connected_to.c_str());
    }

    std::string blockPortIn1(uint8_t row, uint8_t block) { return connector.getBlockIdNoPair(row, block) + ":in1"; }

    std::string blockPortIn2(uint8_t row, uint8_t block) { return connector.getBlockIdNoPair(row, block) + ":in2"; }

    std::string blockPortOut1(uint8_t row, uint8_t block) { return connector.getBlockIdNoPair(row, block) + ":out1"; }

    std::string blockPortOut2(uint8_t row, uint8_t block) { return connector.getBlockIdNoPair(row, block) + ":out2"; }

    std::string blockPairPortIn1(uint8_t row, uint8_t block) { return connector.getBlockIdPairOnly(row, block) + ":in1"; }

    std::string blockPairPortIn2(uint8_t row, uint8_t block) { return connector.getBlockIdPairOnly(row, block) + ":in2"; }

    std::string blockPairPortOut1(uint8_t row, uint8_t block) { return connector.getBlockIdPairOnly(row, block) + ":out1"; }

    std::string blockPairPortOut2(uint8_t row, uint8_t block) { return connector.getBlockIdPairOnly(row, block) + ":out2"; }

private slots:
    void reconnect()
    {
        if (connector.reconnect())
        {
            const bool ok = hostReady();
            hostProcess.terminate();
            QCoreApplication::exit(ok ? 0 : 1);
            return;
        }

        if (++retryAttempt > 10)
        {
            mod_log_warn("Could not connect to host after %d attempts, bailing out", retryAttempt - 1);
            QCoreApplication::exit(1);
            return;
        }

        mod_log_info("Could not connect to host, attempt #%d. Trying again in 500ms...", retryAttempt);
        QTimer::singleShot(500, this, &HostConnectorTests::reconnect);
    }

public:
    HostConnectorTests(jack_client_t* const c, HostProcess& hostProc)
        : client(c),
          hostProcess(hostProc)
    {
        mod_log_info("Connecting to host...");
        QTimer::singleShot(100, this, &HostConnectorTests::reconnect);
    }
};

// --------------------------------------------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // activate logging by default
    setenv("MOD_LOG", "2", 1);

    QCoreApplication app(argc, argv);
    app.setApplicationName("mod-connector");
    app.setApplicationVersion("0.0.1");
    app.setOrganizationName("Darkglass");

    // open and activate jack client
    jack_client_t* const client = jack_client_open("tests", JackNullOption, nullptr);
    assert_return(client != nullptr, 1);
    assert_return(jack_activate(client) == 0, 1);

    // start mod-host
    HostProcess hostProcess;
    QTimer::singleShot(0, &hostProcess, &HostProcess::startSlot);

    // start connector and tests
    HostConnectorTests tests(client, hostProcess);

    // run event loop
    return app.exec();
}

// --------------------------------------------------------------------------------------------------------------------
