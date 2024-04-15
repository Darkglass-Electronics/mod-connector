// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "host.hpp"

#include <QtCore/QIODevice>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

// --------------------------------------------------------------------------------------------------------------------

enum HostError {
    SUCCESS = 0,
    ERR_INSTANCE_INVALID = -1,
    ERR_INSTANCE_ALREADY_EXISTS = -2,
    ERR_INSTANCE_NON_EXISTS = -3,
    ERR_INSTANCE_UNLICENSED = -4,

    ERR_LV2_INVALID_URI = -101,
    ERR_LV2_INSTANTIATION = -102,
    ERR_LV2_INVALID_PARAM_SYMBOL = -103,
    ERR_LV2_INVALID_PRESET_URI = -104,
    ERR_LV2_CANT_LOAD_STATE = -105,

    ERR_JACK_CLIENT_CREATION = -201,
    ERR_JACK_CLIENT_ACTIVATION = -202,
    ERR_JACK_CLIENT_DEACTIVATION = -203,
    ERR_JACK_PORT_REGISTER = -204,
    ERR_JACK_PORT_CONNECTION = -205,
    ERR_JACK_PORT_DISCONNECTION = -206,
    ERR_JACK_VALUE_OUT_OF_RANGE = -207,

    ERR_ASSIGNMENT_ALREADY_EXISTS = -301,
    ERR_ASSIGNMENT_INVALID_OP = -302,
    ERR_ASSIGNMENT_LIST_FULL = -303,
    ERR_ASSIGNMENT_FAILED = -304,
    ERR_ASSIGNMENT_UNUSED = -305,

    ERR_CONTROL_CHAIN_UNAVAILABLE = -401,
    ERR_ABLETON_LINK_UNAVAILABLE = -402,
    ERR_HMI_UNAVAILABLE = -403,
    ERR_EXTERNAL_UI_UNAVAILABLE = -404,

    ERR_MEMORY_ALLOCATION = -901,
    ERR_INVALID_OPERATION = -902
};

enum HostResponseType {
    kHostResponseNone,
    kHostResponseFloat,
    kHostResponseString,
};

struct HostResponse {
    int code;
    union {
        float f;
        char* s;
    } data;
};

static const char* host_error_code_to_string(const int code)
{
    switch (static_cast<HostError>(code))
    {
    case SUCCESS:
        return "success";
    case ERR_INSTANCE_INVALID:
        return "invalid instance";
    case ERR_INSTANCE_ALREADY_EXISTS:
        return "instance already exists";
    case ERR_INSTANCE_NON_EXISTS:
        return "instance does not exist";
    case ERR_INSTANCE_UNLICENSED:
        return "instance is unlicensed";
    case ERR_LV2_INVALID_URI:
        return "invalid URI";
    case ERR_LV2_INSTANTIATION:
        return "instantiation failure";
    case ERR_LV2_INVALID_PARAM_SYMBOL:
        return "invalid parameter symbol";
    case ERR_LV2_INVALID_PRESET_URI:
        return "invalid preset uri";
    case ERR_LV2_CANT_LOAD_STATE:
        return "failed to load state";
    case ERR_JACK_CLIENT_CREATION:
        return "failed to create jack client";
    case ERR_JACK_CLIENT_ACTIVATION:
        return "failed to activate jack client";
    case ERR_JACK_CLIENT_DEACTIVATION:
        return "failed to deactivate jack client";
    case ERR_JACK_PORT_REGISTER:
        return "failed to register jack port";
    case ERR_JACK_PORT_CONNECTION:
        return "failed to connect jack ports";
    case ERR_JACK_PORT_DISCONNECTION:
        return "failed to disconnect jack ports";
    case ERR_JACK_VALUE_OUT_OF_RANGE:
        return "value out of range";
    case ERR_ASSIGNMENT_ALREADY_EXISTS:
        return "assignment already exists";
    case ERR_ASSIGNMENT_INVALID_OP:
        return "invalid assignment operation";
    case ERR_ASSIGNMENT_LIST_FULL:
        return "assignment list is full";
    case ERR_ASSIGNMENT_FAILED:
        return "assignment failed";
    case ERR_ASSIGNMENT_UNUSED:
        return "assignment is unused";
    case ERR_CONTROL_CHAIN_UNAVAILABLE:
        return "control chain is unavailable";
    case ERR_ABLETON_LINK_UNAVAILABLE:
        return "ableton link is unavailable";
    case ERR_HMI_UNAVAILABLE:
        return "HMI is unavailable";
    case ERR_EXTERNAL_UI_UNAVAILABLE:
        return "external UI is unavailable";
    case ERR_MEMORY_ALLOCATION:
        return "failed to allocate memory";
    case ERR_INVALID_OPERATION:
        return "invalid operation";
    }

    return "unknown error";
}

// --------------------------------------------------------------------------------------------------------------------

struct Host::Impl : QObject
{
    Impl(std::string& last_error)
        : last_error(last_error)
    {
        if (const char* const dev = std::getenv("MOD_DEV_HOST"))
        {
            if (std::atoi(dev) != 0)
            {
                dummyDevMode = true;
                return;
            }
        }

        int port;
        if (const char* const portEnv = std::getenv("MOD_DEVICE_HOST_PORT"))
        {
            port = std::atoi(portEnv);

            if (port == 0)
            {
                last_error = "No valid port specified, try setting `MOD_DEVICE_HOST_PORT` env var";
                return;
            }
        }
        else
        {
            port = 5555;
        }

// TODO QAbstractSocket::LowDelayOption

        QObject::connect(&sockets.out, &QAbstractSocket::connected, this, &Host::Impl::slot_connected);
        QObject::connect(&sockets.feedback, &QAbstractSocket::readyRead, this, &Host::Impl::slot_readyRead);

        sockets.out.connectToHost(QHostAddress::LocalHost, port);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        sockets.feedback.connectToHost(QHostAddress::LocalHost, port + 1, QIODeviceBase::ReadOnly);
#else
        sockets.feedback.connectToHost(QHostAddress::LocalHost, port + 1, QIODevice::ReadOnly);
#endif

        if (! sockets.out.waitForConnected())
        {
            last_error = sockets.out.errorString().toStdString();
            close();
            return;
        }

        if (! sockets.feedback.waitForConnected())
        {
            last_error = sockets.feedback.errorString().toStdString();
            close();
            return;
        }
    }

    ~Impl()
    {
        close();
    }

    void close()
    {
        sockets.out.close();
        sockets.feedback.close();
    }

    // ----------------------------------------------------------------------------------------------------------------
    // message handling

    bool writeMessageAndWait(const QString& message,
                             const HostResponseType respType = kHostResponseNone,
                             HostResponse* const resp = nullptr)
    {
        if (dummyDevMode)
        {
            if (resp != nullptr)
            {
                *resp = {};
                resp->code = SUCCESS;
                switch (respType)
                {
                case kHostResponseNone:
                    break;
                case kHostResponseString:
                    resp->data.s = strdup("");
                    break;
                case kHostResponseFloat:
                    resp->data.f = 0.f;
                    break;
                }
            }
            return true;
        }

        if (sockets.out.state() != QAbstractSocket::ConnectedState)
        {
            last_error = "mod-host socket is not connected";
            return false;
        }

        // write message to buffer
        sockets.out.write(message.toUtf8());

        // wait until message is written
        if (! sockets.out.waitForBytesWritten(1000))
        {
            last_error = sockets.out.errorString().toStdString();
            return false;
        }

        // wait for message response
        if (! sockets.out.waitForReadyRead(1000))
        {
            last_error = sockets.out.errorString().toStdString();
            return false;
        }

        // special handling for string replies, read all incoming data
        if (respType == kHostResponseString)
        {
            const QByteArray data(sockets.out.readAll());

            if (resp != nullptr)
            {
                resp->code = SUCCESS;
                resp->data.s = strdup(data.constData());
            }

            return true;
        }

        // read and validate regular response
        QString respdata(sockets.out.readLine());
        if (respdata.isEmpty())
        {
            last_error = "mod-host reply is empty";
            return false;
        }
        if (respdata.length() < 6) // "resp x"
        {
            last_error = "mod-host reply is incomplete (less than 6 characters)";
            return false;
        }
        if (! respdata.startsWith("resp "))
        {
            last_error = "mod-host reply is malformed (missing resp prefix)";
            return false;
        }

        // skip first 5 bytes "resp "
        respdata.remove(0, 5);

        // parse response error code
        bool ok = false;
        const int respcode = respdata.left(respdata.indexOf(' ', 1)).toInt(&ok);

        if (! ok)
        {
            last_error = "failed to parse mod-host response error code";
            return false;
        }
        if (respcode != SUCCESS)
        {
            last_error = host_error_code_to_string(respcode);
            return false;
        }

        // stop here if not wanting response data
        if (resp == nullptr)
            return true;

        // need response data
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QStringList respdatasplit(respdata.split(' ', Qt::KeepEmptyParts));
#else
        const QStringList respdatasplit(respdata.split(' ', QString::KeepEmptyParts));
#endif
        fprintf(stdout, "test2 '%s'\n", respdatasplit[0].toUtf8().constData());

        *resp = {};
        resp->code = respcode;

        switch (respType)
        {
        case kHostResponseNone:
        case kHostResponseString:
            break;
        case kHostResponseFloat:
        //     // resp.data.f = respdata2[1].toFloat();
        //     // resp.data.f = QLocale::c().toDouble(respdata2[1]);
            resp->data.f = std::atof(respdatasplit[1].toUtf8().constData());
            break;
        }

        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // feedback port handling

private:
    bool willReceiveMoreFeedbackMessages = false;

    void reportFeedbackReady()
    {
        if (willReceiveMoreFeedbackMessages)
            return;

        willReceiveMoreFeedbackMessages = true;
        QTimer::singleShot(100, this, &Host::Impl::slot_reportFeedbackReady);
    }

private slots:
    void slot_readyRead()
    {
        for (;;)
        {
            const QString data(sockets.feedback.readLine(0));

            if (data.isEmpty())
                break;

            fprintf(stdout, "got feedback: '%s'\n", data.toUtf8().constData());
        }

        reportFeedbackReady();
    }

    void slot_reportFeedbackReady()
    {
        willReceiveMoreFeedbackMessages = false;
        writeMessageAndWait("output_data_ready");
    }

    // ----------------------------------------------------------------------------------------------------------------

private slots:
    void slot_connected()
    {
        reportFeedbackReady();
    }

private:
    std::string& last_error;
    bool dummyDevMode = false;

    struct {
        QTcpSocket out;
        QTcpSocket feedback;
    } sockets;
};

// --------------------------------------------------------------------------------------------------------------------

Host::Host() : impl(new Impl(last_error)) {}
Host::~Host() { delete impl; }

// --------------------------------------------------------------------------------------------------------------------

// TODO escape-quote strings

bool Host::add(const char* const uri, const int16_t instance_number)
{
    const QString message(QString::fromUtf8("add \"%1\" %2").arg(uri).arg(instance_number));
    return impl->writeMessageAndWait(message);
}

bool Host::remove(const int16_t instance_number)
{
    const QString message(QString::fromUtf8("remove %1").arg(instance_number));
    return impl->writeMessageAndWait(message);
}

bool Host::preset_load(const int16_t instance_number, const char* const preset_uri)
{
    const QString message(QString::fromUtf8("preset_load %1 \"%2\"").arg(instance_number).arg(preset_uri));
    return impl->writeMessageAndWait(message);
}

bool Host::preset_save(const int16_t instance_number, const char* const preset_name, const char* const dir, const char* const file_name)
{
    const QString message(QString::fromUtf8("preset_save %1 \"%2\" \"%3\" \"%4\"").arg(instance_number).arg(preset_name).arg(dir).arg(file_name));
    return impl->writeMessageAndWait(message);
}

std::string Host::preset_show(const char* const preset_uri)
{
    const QString message(QString::fromUtf8("preset_show \"%1\"").arg(preset_uri));

    HostResponse resp;
    if (! impl->writeMessageAndWait(message, kHostResponseString, &resp))
        return {};

    const std::string ret(resp.data.s);
    std::free(resp.data.s);
    return ret;
}

float Host::cpu_load()
{
    const QString message(QString::fromUtf8("cpu_load"));
    HostResponse resp;
    return impl->writeMessageAndWait(message, kHostResponseFloat, &resp) ? resp.data.f : 0.f;
}

// --------------------------------------------------------------------------------------------------------------------
