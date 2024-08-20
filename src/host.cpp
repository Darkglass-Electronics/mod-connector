// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "host.hpp"
#include "utils.hpp"

#include <cassert>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#define INVALID_SOCKET -1
typedef int SOCKET;
#endif

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

struct Host::Impl
{
    Impl(std::string& last_error_)
        : last_error(last_error_)
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

       #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            last_error = "WSAStartup failed";
            return;
        }

        if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
        {
            last_error = "WSAStartup version mismatch";
            WSACleanup();
            return;
        }
       #endif

        if ((sockets.out = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        {
            last_error = "output socket error";
            return;
        }

        if ((sockets.feedback = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        {
            last_error = "feedback socket error";
            ::closesocket(sockets.out);
            sockets.out = INVALID_SOCKET;
            return;
        }

       #ifndef _WIN32
        /* increase socket size */
        const int size = 131071;
        setsockopt(sockets.out, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        setsockopt(sockets.feedback, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
       #endif

        /* Startup the socket struct */
        struct sockaddr_in serv_addr = {};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        /* Try assign the server address */
        serv_addr.sin_port = htons(port);
        if (::connect(sockets.out, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            last_error = "output socket connect error";
            return;
        }

        serv_addr.sin_port = htons(port + 1);
        if (::connect(sockets.feedback, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            last_error = "feedback socket connect error";
            return;
        }
    }

    ~Impl()
    {
        close();
    }

    void close()
    {
        if (sockets.out == INVALID_SOCKET)
            return;

        // make local copies so that we can invalidate these vars first
        const SOCKET outsock = sockets.out;
        const SOCKET fbsock = sockets.feedback;
        sockets.out = sockets.feedback = INVALID_SOCKET;

        ::closesocket(outsock);
        ::closesocket(fbsock);

       #ifdef _WIN32
        WSACleanup();
       #endif
    }

    // ----------------------------------------------------------------------------------------------------------------
    // message handling

    bool writeMessageAndWait(const std::string& message,
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

        if (sockets.out == INVALID_SOCKET)
        {
            last_error = "mod-host socket is not connected";
            return false;
        }

        // write message to socket
        {
            const char* buffer = message.c_str();
            size_t msgsize = message.size() + 1;
            int ret;

            while (msgsize > 0)
            {
                ret = ::send(sockets.out, buffer, msgsize, 0);
                if (ret < 0)
                {
                    last_error = "send error";
                    return false;
                }

                msgsize -= ret;
                buffer += ret;
            }
        }

        // retrieve response
        if (nonBlockingMode)
        {
            assert(resp == nullptr);

            ++numNonBlockingOps;

            // fprintf(stderr, "Host::writeMessageAndWait() '%s' | %d\n", message.c_str(), numNonBlockingOps);
        }
        else
        {
            assert(numNonBlockingOps == 0);

            // use stack buffer first, in case of small messages
            char stackbuffer[128];
            char* buffer = stackbuffer;
            size_t buffersize = sizeof(stackbuffer) / sizeof(stackbuffer[0]);
            size_t written = 0;
            last_error.clear();

            for (int r;;)
            {
                r = recv(sockets.out, buffer + written, 1, 0);

                /* Data received */
                if (r == 1)
                {
                    // null terminator, stop
                    if (buffer[written] == '\0')
                        break;

                    // increase buffer by 2x for longer messages
                    if (++written == buffersize)
                    {
                        buffersize *= 2;

                        if (stackbuffer == buffer)
                        {
                            buffer = static_cast<char*>(std::malloc(buffersize));
                            std::memcpy(buffer, stackbuffer, sizeof(stackbuffer));
                        }
                        else
                        {
                            buffer = static_cast<char*>(std::realloc(buffer, buffersize));
                        }
                    }
                }
                /* Error */
                else if (r < 0)
                {
                    last_error = "read error";
                    break;
                }
                /* Client disconnected */
                else
                {
                    last_error = "disconnected";
                    break;
                }
            }

            if (! last_error.empty())
            {   
                if (stackbuffer != buffer)
                    std::free(buffer);

                return false;
            }

            // special handling for string replies, read all incoming data
            if (respType == kHostResponseString)
            {
                if (resp != nullptr)
                {
                    resp->code = SUCCESS;
                    resp->data.s = buffer;
                }
                else
                {
                    if (stackbuffer != buffer)
                        std::free(buffer);
                }

                return true;
            }

            if (buffer[0] == '\0')
            {
                last_error = "mod-host reply is empty";
                return false;
            }
            if (std::strncmp(buffer, "resp ", 5) != 0)
            {
                last_error = "mod-host reply is malformed (missing resp prefix)";
                return false;
            }
            if (buffer[5] == '\0')
            {
                last_error = "mod-host reply is incomplete (less than 6 characters)";
                return false;
            }

            char* const respbuffer = buffer + 5;
            const char* respdata;
            if (char* respargs = std::strchr(respbuffer, ' '))
            {
                *respargs = '\0';
                respdata = respargs + 1;
            }
            else
            {
                respdata = nullptr;
            }

            // parse response error code
            // bool ok = false;
            const int respcode = std::atoi(respbuffer);

            // printf("wrote '%s', got resp %d '%s' '%s'\n", message.c_str(), respcode, respbuffer, respdata);

            /*
            if (! ok)
            {
                last_error = "failed to parse mod-host response error code";
                return false;
            }
            */
            if (respcode < 0)
            {
                last_error = host_error_code_to_string(respcode);
                return false;
            }

            // stop here if not wanting response data
            if (resp == nullptr)
                return true;

            *resp = {};
            resp->code = respcode;

            switch (respType)
            {
            case kHostResponseNone:
            case kHostResponseString:
                break;
            case kHostResponseFloat:
                resp->data.f = respdata != nullptr
                             ? std::atof(respdata)
                             : 0.f;
                break;
            }
        }

        return true;
    }

    bool wait()
    {
        char c;
        int r;
        last_error.clear();

        // fprintf(stderr, "Host::wait() begin %d ------------------------------\n", numNonBlockingOps);

        while (numNonBlockingOps != 0)
        {
            r = recv(sockets.out, &c, 1, 0);

            /* Data received */
            if (r == 1)
            {
                // fprintf(stderr, "%c", c);

                if (c == '\0')
                {
                    --numNonBlockingOps;
                    // fprintf(stderr, "\nHost::wait() next %d ------------------------------\n", numNonBlockingOps);
                }
            }
            /* Error */
            else if (r < 0)
            {
                last_error = "read error";
                return false;
            }
            /* Client disconnected */
            else
            {
                last_error = "disconnected";
                return false;
            }
        }

        // fprintf(stderr, "Host::wait() end %d ------------------------------\n", numNonBlockingOps);
        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // feedback port handling

    bool poll()
    {
        last_error.clear();

        while (_poll()) {}

        return last_error.empty();
    }

    bool _poll()
    {
        // set non-blocking mode, so we can poke to see if there are any messages
       #ifdef _WIN32
        const unsigned long nonblocking = 1;
        ::ioctlsocket(sockets.feedback, FIONBIO, &nonblocking);
       #else
        const int socketflags = ::fcntl(sockets.feedback, F_GETFL);
        ::fcntl(sockets.feedback, F_SETFL, socketflags | O_NONBLOCK);
       #endif

        // read first byte
        char firstbyte = '\0';
        int r = recv(sockets.feedback, &firstbyte, 1, 0);

        // set blocking mode again, so we block-wait until message is fully delivered
       #ifdef _WIN32
        const unsigned long blocking = 0;
        ::ioctlsocket(sockets.feedback, FIONBIO, &blocking);
       #else
        ::fcntl(sockets.feedback, F_SETFL, socketflags & ~O_NONBLOCK);
       #endif

        // nothing to read, quit
        if (r == 0)
            return false;

        if (r < 0)
        {
            last_error = "read error";
            return false;
        }

        // use stack buffer first, in case of small messages
        char stackbuffer[128] = { firstbyte, };
        char* buffer = stackbuffer;
        size_t buffersize = sizeof(stackbuffer) / sizeof(stackbuffer[0]);
        size_t read = 1;

        for (;;)
        {
            r = recv(sockets.feedback, buffer + read, 1, 0);

            /* Data received */
            if (r == 1)
            {
                // null terminator, stop
                if (buffer[read] == '\0')
                    break;

                // increase buffer by 2x for longer messages
                if (++read == buffersize)
                {
                    buffersize *= 2;

                    if (stackbuffer == buffer)
                    {
                        buffer = static_cast<char*>(std::malloc(buffersize));
                        std::memcpy(buffer, stackbuffer, sizeof(stackbuffer));
                    }
                    else
                    {
                        buffer = static_cast<char*>(std::realloc(buffer, buffersize));
                    }
                }
            }
            /* Error */
            else if (r < 0)
            {
                last_error = "read error";
                break;
            }
            /* Client disconnected */
            else
            {
                last_error = "disconnected";
                break;
            }
        }

        printf("got feedback '%s'\n", buffer);

        if (stackbuffer != buffer)
            std::free(buffer);

        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------

private:
    std::string& last_error;
    bool dummyDevMode = false;
    bool nonBlockingMode = false;
    uint16_t numNonBlockingOps = 0;

    struct {
        SOCKET out = INVALID_SOCKET;
        SOCKET feedback = INVALID_SOCKET;
    } sockets;

    friend class NonBlockingScope;
};

// --------------------------------------------------------------------------------------------------------------------

Host::Host() : impl(new Impl(last_error)) {}
Host::~Host() { delete impl; }

// --------------------------------------------------------------------------------------------------------------------

Host::NonBlockingScope::NonBlockingScope(Host& host_)
    : host(host_)
{
    assert(! host.impl->nonBlockingMode);

    host.impl->nonBlockingMode = true;
}

Host::NonBlockingScope::~NonBlockingScope()
{
    host.impl->nonBlockingMode = false;
    host.impl->wait();
}

// --------------------------------------------------------------------------------------------------------------------

// TODO escape-quote strings

bool Host::add(const char* const uri, const int16_t instance_number)
{
    return impl->writeMessageAndWait(format("add %s %d", uri, instance_number));
}

bool Host::remove(const int16_t instance_number)
{
    return impl->writeMessageAndWait(format("remove %d", instance_number));
}

bool Host::activate(int16_t instance_number, int16_t instance_number_end, bool activate_value)
{
    return impl->writeMessageAndWait(format("activate %d %d %d", instance_number, instance_number_end, activate_value ? 1 : 0));
}

bool Host::preload(const char* const uri, int16_t instance_number)
{
    return impl->writeMessageAndWait(format("preload %s %d", uri, instance_number));
}

bool Host::preset_load(const int16_t instance_number, const char* const preset_uri)
{
    return impl->writeMessageAndWait(format("preset_load %d %s", instance_number, preset_uri));
}

bool Host::preset_save(const int16_t instance_number, const char* const preset_name, const char* const dir, const char* const file_name)
{
    return impl->writeMessageAndWait(format("preset_save %d \"%s\" \"%s\" \"%s\"", instance_number, preset_name, dir, file_name));
}

std::string Host::preset_show(const char* const preset_uri)
{
    HostResponse resp;
    if (! impl->writeMessageAndWait(format("preset_show %s", preset_uri),
                                    kHostResponseString, &resp))
        return {};

    const std::string ret(resp.data.s);
    std::free(resp.data.s);
    return ret;
}

bool Host::connect(const char* origin_port, const char* destination_port)
{
    return impl->writeMessageAndWait(format("connect \"%s\" \"%s\"", origin_port, destination_port));
}

bool Host::disconnect(const char* origin_port, const char* destination_port)
{
    return impl->writeMessageAndWait(format("disconnect \"%s\" \"%s\"", origin_port, destination_port));
}

bool Host::bypass(int16_t instance_number, bool bypass_value)
{
    return impl->writeMessageAndWait(format("bypass %d %d", instance_number, bypass_value ? 1 : 0));
}

bool Host::param_set(int16_t instance_number, const char* param_symbol, float param_value)
{
    return impl->writeMessageAndWait(format("param_set %d %s %f", instance_number, param_symbol, param_value));
}

bool Host::param_get(int16_t instance_number, const char* param_symbol)
{
    return impl->writeMessageAndWait(format("param_get %d %s", instance_number, param_symbol));
}

bool Host::patch_set(int16_t instance_number, const char* property_uri, const char* value)
{
    return impl->writeMessageAndWait(format("patch_set %d %s \"%s\"", instance_number, property_uri, value));
}

float Host::cpu_load()
{
    HostResponse resp;
    return impl->writeMessageAndWait("cpu_load",
                                     kHostResponseFloat, &resp) ? resp.data.f : 0.f;
}

bool Host::feature_enable(const char* const feature, const bool enable)
{
    return impl->writeMessageAndWait(format("feature_enable %s %d", feature, enable ? 1 : 0));
}

bool Host::transport(const bool rolling, const double beats_per_bar, const double beats_per_minute)
{
    return impl->writeMessageAndWait(format("transport %d %f %f", rolling, beats_per_bar, beats_per_minute));
}

bool Host::transport_sync(const char* const mode)
{
    return impl->writeMessageAndWait(format("transport_sync \"%s\"", mode));
}

bool Host::output_data_ready()
{
    return impl->writeMessageAndWait("output_data_ready");
}

bool Host::poll_feedback()
{
    return impl->poll();
}

// --------------------------------------------------------------------------------------------------------------------
