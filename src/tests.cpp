// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

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

        // test pass-through connections
        assert_return(testPassthrough(), false);

        mod_log_info("All tests finished successfully!");

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

    // test loading single plugin
    bool testPluginLoad()
    {
        // load plugin
        assert_return(connector.replaceBlock(0, 0, "urn:mod-connector:test2in2out"), false);

        // ensure our single plugin is connected properly
        std::string port_name;

        port_name = connector.getBlockId(0, 0) + ":in1";
        assert_return(checkOnlyConnection(port_name, JACK_CAPTURE_PORT_1), false);

        port_name = connector.getBlockId(0, 0) + ":in2";
        assert_return(checkOnlyConnection(port_name, JACK_CAPTURE_PORT_2), false);

        port_name = connector.getBlockId(0, 0) + ":out1";
        assert_return(checkOnlyConnection(port_name, JACK_PLAYBACK_PORT_1), false);

        port_name = connector.getBlockId(0, 0) + ":out2";
        assert_return(checkOnlyConnection(port_name, JACK_PLAYBACK_PORT_2), false);

        // remove plugin
        assert_return(connector.replaceBlock(0, 0, nullptr), false);

        return true;
    }

    bool checkOnlyConnection(std::string &port_to_check, const char* const only_port_connected_to)
    {
        QStringList connections;
        connections = q_jack_port_get_all_connections(client, port_to_check);
        return connections == QStringList({ only_port_connected_to });
    }

    bool checkOnlyConnection(std::string &port_to_check, std::string only_port_connected_to)
    {
        return checkOnlyConnection(port_to_check, only_port_connected_to.c_str());
    }

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
