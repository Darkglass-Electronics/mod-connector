// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#define MOD_LOG_GROUP "host"

#include "host.hpp"
#include "config.h"
#include "utils.hpp"

#include <cassert>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
    kHostResponseInteger,
    kHostResponseFloat,
    kHostResponseString,
};

struct HostResponse {
    int code;
    union {
        int i;
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
       #ifdef __EMSCRIPTEN__
        dummyDevMode = true;
        return;
       #endif

        if (const char* const dev = std::getenv("MOD_DEV_HOST"))
        {
            if (std::atoi(dev) != 0)
            {
                dummyDevMode = true;
                return;
            }
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

        wsaInitialized = true;
       #endif

        reconnect();
    }

    ~Impl()
    {
        close();

       #ifdef _WIN32
        if (wsaInitialized)
            WSACleanup();
       #endif
    }

    bool reconnect()
    {
        if (dummyDevMode)
            return true;

       #ifdef _WIN32
        if (! wsaInitialized)
            return false;
       #endif

        if (sockets.out != INVALID_SOCKET)
        {
            last_error.clear();
            return true;
        }

        int port;
        if (const char* const portEnv = std::getenv("MOD_DEVICE_HOST_PORT"))
        {
            port = std::atoi(portEnv);

            if (port == 0)
            {
                last_error = "No valid port specified, try setting `MOD_DEVICE_HOST_PORT` env var";
                return false;
            }
        }
        else
        {
            port = 5555;
        }

        if ((sockets.out = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        {
            last_error = "output socket error";
            return false;
        }

        if ((sockets.feedback = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        {
            last_error = "feedback socket error";
            ::closesocket(sockets.out);
            sockets.out = INVALID_SOCKET;
            return false;
        }

       #ifndef _WIN32
        /* increase socket size */
        int value = 131071;
        setsockopt(sockets.out, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
        setsockopt(sockets.feedback, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));

        /* set TCP_NODELAY */
        value = 1;
        setsockopt(sockets.out, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
        setsockopt(sockets.feedback, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
       #endif

        /* Startup the socket struct */
        struct sockaddr_in serv_addr = {};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        /* Try assign the server address */
        serv_addr.sin_port = htons(port);
        if (::connect(sockets.out, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            close();
            last_error = "output socket connect error";
            return false;
        }

        serv_addr.sin_port = htons(port + 1);
        if (::connect(sockets.feedback, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            close();
            last_error = "feedback socket connect error";
            return false;
        }

        /* set non-blocking mode on feedback port, so we can poke to see if there are any messages */
       #ifdef _WIN32
        const unsigned long nonblocking = 1;
        ::ioctlsocket(sockets.feedback, FIONBIO, &nonblocking);
       #else
        const int socketflags = ::fcntl(sockets.feedback, F_GETFL);
        ::fcntl(sockets.feedback, F_SETFL, socketflags | O_NONBLOCK);
       #endif

        return true;
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
                case kHostResponseFloat:
                    resp->data.f = 0.f;
                    break;
                case kHostResponseInteger:
                    resp->data.i = 0;
                    break;
                case kHostResponseString:
                    resp->data.s = strdup("");
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

       #ifndef NDEBUG
        const bool canDebug = message != "output_data_ready";
        if (canDebug) {
            mod_log_debug("%s: sending '%s'", __func__, message.c_str());
        }
       #endif

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

            mod_log_debug3("%s: non-block send, numNonBlockingOps: %u", __func__, numNonBlockingOps);
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
                mod_log_warn("error: %s", last_error.c_str());

                if (stackbuffer != buffer)
                    std::free(buffer);

                return false;
            }

           #ifndef NDEBUG
            if (canDebug) {
                mod_log_debug("%s: received response: '%s'", __func__, buffer);
            }
           #endif

            // special handling for string replies, read all incoming data
            if (respType == kHostResponseString)
            {
                if (resp != nullptr)
                {
                    resp->code = SUCCESS;

                    if (stackbuffer != buffer)
                        resp->data.s = buffer;
                    else
                        resp->data.s = strdup(buffer);
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
            case kHostResponseInteger:
                resp->data.i = respdata != nullptr
                             ? std::atoi(respdata)
                             : 0;
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

        mod_log_debug("%s: begin, numNonBlockingOps: %u", __func__, numNonBlockingOps);

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
                    mod_log_debug3("%s: next, numNonBlockingOps: %u", __func__, numNonBlockingOps);
                }
            }
            /* Error */
            else if (r < 0)
            {
                last_error = "read error";
                mod_log_warn("error: %s", last_error.c_str());
                return false;
            }
            /* Client disconnected */
            else
            {
                last_error = "disconnected";
                mod_log_warn("error: %s", last_error.c_str());
                return false;
            }
        }

        mod_log_debug("%s: end, numNonBlockingOps: %u", __func__, numNonBlockingOps);
        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // feedback port handling

    bool poll(FeedbackCallback* const callback) const
    {
        std::string error;

        while (!dummyDevMode && _poll(callback, error)) {}

        return error.empty();
    }

    bool _poll(FeedbackCallback* const callback, std::string& error) const
    {
        // read first byte
        char firstbyte = '\0';
        int r = recv(sockets.feedback, &firstbyte, 1, 0);

        // nothing to read, quit
        if (r == 0)
            return false;

        if (r < 0)
        {
            error = "read error";
            return false;
        }

        // set blocking mode, so we block-wait until message is fully delivered
       #ifdef _WIN32
        const unsigned long blocking = 0;
        ::ioctlsocket(sockets.feedback, FIONBIO, &blocking);
       #else
        const int socketflags = ::fcntl(sockets.feedback, F_GETFL);
        ::fcntl(sockets.feedback, F_SETFL, socketflags & ~O_NONBLOCK);
       #endif

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
                error = "read error";
                break;
            }
            /* Client disconnected */
            else
            {
                error = "disconnected";
                break;
            }
        }

        // set non-blocking mode again
       #ifdef _WIN32
        const unsigned long nonblocking = 1;
        ::ioctlsocket(sockets.feedback, FIONBIO, &nonblocking);
       #else
        ::fcntl(sockets.feedback, F_SETFL, socketflags | O_NONBLOCK);
       #endif

        if (std::strncmp(buffer, "audio_monitor ", 14) == 0)
        {
            assert(read > 16);
            HostFeedbackData d = { HostFeedbackData::kFeedbackAudioMonitor, {} };

            char* msgbuffer;
            char* sep = buffer + 14;

            // 1st arg: int index
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.audioMonitor.index = std::atoi(msgbuffer);

            // 2nd arg: float value
            msgbuffer = sep;
            d.audioMonitor.value = std::atof(msgbuffer);

            callback->hostFeedbackCallback(d);
        }
        else if (std::strncmp(buffer, "cpu_load ", 9) == 0)
        {
            assert(read > 11);
            HostFeedbackData d = { HostFeedbackData::kFeedbackCpuLoad, {} };

            char* msgbuffer;
            char* sep = buffer + 9;

            // 1st arg: float avg
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.cpuLoad.avg = std::atof(msgbuffer);

            // 2nd arg: float max
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.cpuLoad.max = std::atof(msgbuffer);

            // 3rd arg: int xruns
            msgbuffer = sep;
            d.cpuLoad.xruns = std::atoi(msgbuffer);

            callback->hostFeedbackCallback(d);
        }
        else if (std::strncmp(buffer, "param_set ", 10) == 0)
        {
            assert(read > 12);
            HostFeedbackData d = { HostFeedbackData::kFeedbackParameterSet, {} };

            char* msgbuffer;
            char* sep = buffer + 10;

            // 1st arg: int effect_id
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.paramSet.effect_id = std::atoi(msgbuffer);

            // 2nd arg: char* symbol
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.paramSet.symbol = msgbuffer;

            // 3rd arg: float value
            msgbuffer = sep;
            d.paramSet.value = std::atof(msgbuffer);

            callback->hostFeedbackCallback(d);
        }
        else if (std::strncmp(buffer, "patch_set ", 10) == 0)
        {
            assert(read > 12);
            HostFeedbackData d = { HostFeedbackData::kFeedbackPatchSet, {} };

            char* msgbuffer;
            char* sep = buffer + 10;
            void* ptr2free = nullptr;

            // 1st arg: int effect_id
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.patchSet.effect_id = std::atoi(msgbuffer);

            // 2nd arg: char* key
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.patchSet.key = msgbuffer;

            // 3rd arg: char type
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.patchSet.type = msgbuffer[0];

            // 4th arg: data
            msgbuffer = sep;

            switch (d.patchSet.type)
            {
            case 'b':
            case 'i':
                d.patchSet.data.i = std::atoi(msgbuffer);
                break;
            case 'l':
                d.patchSet.data.l = std::atoll(msgbuffer);
                break;
            case 'f':
                d.patchSet.data.f = std::atof(msgbuffer);
                break;
            case 'g':
                d.patchSet.data.g = std::atof(msgbuffer);
                break;
            case 's':
            case 'p':
            case 'u':
                d.patchSet.data.s = msgbuffer;
                break;
            case 'v':
                // vector 1st arg: int num
                sep = std::strchr(sep, '-');
                *sep++ = '\0';
                d.patchSet.data.v.num = std::atoi(msgbuffer);

                // vector 2nd arg: char type
                msgbuffer = sep;
                sep = std::strchr(sep, '-');
                *sep++ = '\0';
                d.patchSet.data.v.type = msgbuffer[0];

                // vector 3rd arg: data
                msgbuffer = sep;

                switch (d.patchSet.data.v.type)
                {
                case 'b':
                case 'i':
                    if (int32_t* const data = static_cast<int32_t*>(std::calloc(d.patchSet.data.v.num, sizeof(int32_t))))
                    {
                        for (uint32_t i = 0; i < d.patchSet.data.v.num && *msgbuffer != '\0'; ++i)
                        {
                            if ((sep = std::strchr(sep, ':')) != nullptr)
                                *sep++ = '\0';

                            data[i] = std::atoi(msgbuffer);
                            msgbuffer = sep;
                        }

                        ptr2free = data;
                        d.patchSet.data.v.data.i = data;
                    }
                    else
                    {
                        d.patchSet.data.v.data.i = nullptr;
                    }
                    break;
                case 'l':
                    if (int64_t* const data = static_cast<int64_t*>(std::calloc(d.patchSet.data.v.num, sizeof(int64_t))))
                    {
                        for (uint32_t i = 0; i < d.patchSet.data.v.num && *msgbuffer != '\0'; ++i)
                        {
                            if ((sep = std::strchr(sep, ':')) != nullptr)
                                *sep++ = '\0';

                            data[i] = std::atof(msgbuffer);
                            msgbuffer = sep;
                        }

                        ptr2free = data;
                        d.patchSet.data.v.data.l = data;
                    }
                    else
                    {
                        d.patchSet.data.v.data.l = nullptr;
                    }
                    break;
                case 'f':
                    if (float* const data = static_cast<float*>(std::calloc(d.patchSet.data.v.num, sizeof(float))))
                    {
                        for (uint32_t i = 0; i < d.patchSet.data.v.num && *msgbuffer != '\0'; ++i)
                        {
                            if ((sep = std::strchr(sep, ':')) != nullptr)
                                *sep++ = '\0';

                            data[i] = std::atof(msgbuffer);
                            msgbuffer = sep;
                        }

                        ptr2free = data;
                        d.patchSet.data.v.data.f = data;
                    }
                    else
                    {
                        d.patchSet.data.v.data.f = nullptr;
                    }
                    break;
                case 'g':
                    if (double* const data = static_cast<double*>(std::calloc(d.patchSet.data.v.num, sizeof(double))))
                    {
                        for (uint32_t i = 0; i < d.patchSet.data.v.num && *msgbuffer != '\0'; ++i)
                        {
                            if ((sep = std::strchr(sep, ':')) != nullptr)
                                *sep++ = '\0';

                            data[i] = std::atof(msgbuffer);
                            msgbuffer = sep;
                        }

                        ptr2free = data;
                        d.patchSet.data.v.data.g = data;
                    }
                    else
                    {
                        d.patchSet.data.v.data.g = nullptr;
                    }
                    break;
                default:
                    std::memset(&d.patchSet.data.v.data, 0, sizeof(d.patchSet.data.v.data));
                    break;
                }
                break;
            default:
                std::memset(&d.patchSet.data, 0, sizeof(d.patchSet.data));
                break;
            }

            callback->hostFeedbackCallback(d);
            std::free(ptr2free);
        }
        else if (std::strncmp(buffer, "output_set ", 11) == 0)
        {
            assert(read > 13);
            HostFeedbackData d = { HostFeedbackData::kFeedbackOutputMonitor, {} };

            char* msgbuffer;
            char* sep = buffer + 11;

            // 1st arg: int effect_id
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.outputMonitor.effect_id = std::atoi(msgbuffer);

            // 2nd arg: char* symbol
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.outputMonitor.symbol = msgbuffer;

            // 3rd arg: float value
            msgbuffer = sep;
            d.outputMonitor.value = std::atof(msgbuffer);

            callback->hostFeedbackCallback(d);
        }
        else if (std::strncmp(buffer, "midi_program_change ", 20) == 0)
        {
            assert(read > 22);
            HostFeedbackData d = { HostFeedbackData::kFeedbackMidiProgramChange, {} };

            char* msgbuffer;
            char* sep = buffer + 20;

            // 1st arg: int program
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.midiProgramChange.program = std::atoi(msgbuffer);

            // 2nd arg: int channel
            msgbuffer = sep;
            d.midiProgramChange.channel = std::atoi(msgbuffer);

            callback->hostFeedbackCallback(d);
        }
        else if (std::strncmp(buffer, "midi_mapped ", 12) == 0)
        {
            assert(read > 14);
            HostFeedbackData d = { HostFeedbackData::kFeedbackMidiMapped, {} };

            char* msgbuffer;
            char* sep = buffer + 12;

            // 1st arg: int effect_id
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.midiMapped.effect_id = std::atoi(msgbuffer);

            // 2nd arg: char* symbol
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.midiMapped.symbol = msgbuffer;

            // 3rd arg: int channel
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.midiMapped.channel = std::atoi(msgbuffer);

            // 4th arg: int controller
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.midiMapped.controller = std::atoi(msgbuffer);

            // 5th arg: float value
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.midiMapped.value = std::atof(msgbuffer);

            // 6th arg: float value
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.midiMapped.minimum = std::atof(msgbuffer);

            // 7th arg: float value
            msgbuffer = sep;
            d.midiMapped.maximum = std::atof(msgbuffer);

            callback->hostFeedbackCallback(d);
        }
        else if (std::strncmp(buffer, "transport ", 10) == 0)
        {
            assert(read > 12);
            HostFeedbackData d = { HostFeedbackData::kFeedbackTransport, {} };

            char* msgbuffer;
            char* sep = buffer + 10;

            // 1st arg: bool rolling
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.transport.rolling = msgbuffer[0] != '0';

            // 2nd arg: float bpm
            msgbuffer = sep;
            sep = std::strchr(sep, ' ');
            *sep++ = '\0';
            d.transport.bpm = std::atof(msgbuffer);

            // 3rd arg: float bpb
            msgbuffer = sep;
            d.transport.bpb = std::atof(msgbuffer);

            callback->hostFeedbackCallback(d);
        }
        else if (std::strncmp(buffer, "log ", 4) == 0)
        {
            assert(read > 6);
            HostFeedbackData d = { HostFeedbackData::kFeedbackLog, {} };

            switch (buffer[4])
            {
            case '3': d.log.type = 'e'; break;
            case '2': d.log.type = 'w'; break;
            case '0': d.log.type = 'd'; break;
            default: d.log.type = 'n'; break;
            }

            d.log.msg = buffer + 6;
            callback->hostFeedbackCallback(d);
        }
        else if (std::strcmp(buffer, "data_finish") == 0)
        {
            HostFeedbackData d = { HostFeedbackData::kFeedbackFinished, {} };
            callback->hostFeedbackCallback(d);
        }
        else
        {
            mod_log_warn("unknown feedback messge '%s'\n", buffer);
        }

        if (stackbuffer != buffer)
            std::free(buffer);

        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------

    std::string& last_error;

private:
    bool dummyDevMode = false;
    bool nonBlockingMode = false;
    uint16_t numNonBlockingOps = 0;

    struct {
        SOCKET out = INVALID_SOCKET;
        SOCKET feedback = INVALID_SOCKET;
    } sockets;

   #ifdef _WIN32
    bool wsaInitialized = false;
   #endif

    friend class NonBlockingScope;
    friend class NonBlockingScopeWithAudioFades;
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
    assert(host.impl->nonBlockingMode);

    host.impl->nonBlockingMode = false;
    host.impl->wait();
}

// --------------------------------------------------------------------------------------------------------------------

Host::NonBlockingScopeWithAudioFades::NonBlockingScopeWithAudioFades(Host& host_)
    : host(host_)
{
    assert(! host.impl->nonBlockingMode);

    host.impl->nonBlockingMode = true;
    host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOffWithFadeOut);
}

Host::NonBlockingScopeWithAudioFades::~NonBlockingScopeWithAudioFades()
{
    assert(host.impl->nonBlockingMode);

    host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOnWithFadeIn);
    host.impl->nonBlockingMode = false;
    host.impl->wait();
}

// --------------------------------------------------------------------------------------------------------------------
// input validation for debug builds

#ifdef NDEBUG
#define VALIDATE_INSTANCE_NUMBER(n)
#define VALIDATE_INSTANCE_REMOVE_NUMBER(n)
#define VALIDATE_JACK_PORT(p)
#define VALIDATE_MIDI_CHANNEL(c)
#define VALIDATE_SYMBOL(s)
#define VALIDATE_URI(u)
#else
static bool valid_jack_port(const char* const port)
{
    // must contain at least 3 characters
    if (std::strlen(port) < 3)
        return false;
    // must contain at least 1 client/port name separator
    if (std::strchr(port, ':') == nullptr)
        return false;
    return true;
}
static bool valid_symbol(const char* const symbol)
{
    // must only contain very specific characters
    const size_t len = std::strlen(symbol);
    for (size_t i = 0; i < len; ++i)
    {
        switch (symbol[i])
        {
        case '_':
        case 'a' ... 'z':
        case 'A' ... 'Z':
            break;
        case '0' ... '9':
            // not allowed as first character
            if (i == 0)
                return false;
            break;
        }
    }
    return true;
}
static bool valid_uri(const char* const uri)
{
    // must contain at least 1 colon
    if (std::strchr(uri, ':') == nullptr)
        return false;
    // must not contain spaces or quotes
    if (std::strchr(uri, ' ') != nullptr)
        return false;
    if (std::strchr(uri, '"') != nullptr)
        return false;
    return true;
}
#define VALIDATE(expr) \
    if (__builtin_expect(!(expr),0)) { _assert_print(#expr, __FILE__, __LINE__); abort(); return {}; }
#define VALIDATE_INSTANCE_NUMBER(n) VALIDATE(n >= 0 && n < MAX_MOD_HOST_INSTANCES)
#define VALIDATE_INSTANCE_REMOVE_NUMBER(n) VALIDATE(n >= -1 && n < MAX_MOD_HOST_INSTANCES)
#define VALIDATE_BPB(b) VALIDATE(b >= 1 && b < 16)
#define VALIDATE_BPM(b) VALIDATE(b >= 20 && b < 280)
#define VALIDATE_JACK_PORT(p) VALIDATE(valid_jack_port(p))
#define VALIDATE_MIDI_CHANNEL(c) VALIDATE(c >= 0 && c < 16)
#define VALIDATE_SYMBOL(s) VALIDATE(valid_symbol(s))
#define VALIDATE_URI(uri) VALIDATE(valid_uri(uri))
#endif

// --------------------------------------------------------------------------------------------------------------------
// utilities

static std::string escape(const char* const str)
{
    // TODO properly escape-quote strings

    if (const char* const space = std::strchr(str, ' '))
    {
        std::string ret;
        ret += "\"";
        ret += str;
        ret += "\"";
        return ret;
    }

    return str;
}

bool Host::add(const char* const uri, const int16_t instance_number)
{
    VALIDATE_INSTANCE_NUMBER(instance_number);
    VALIDATE_URI(uri);

    return impl->writeMessageAndWait(format("add %s %d", uri, instance_number));
}

bool Host::remove(const int16_t instance_number)
{
    VALIDATE_INSTANCE_REMOVE_NUMBER(instance_number);

    return impl->writeMessageAndWait(format("remove %d", instance_number));
}

bool Host::activate(const int16_t instance_number, const bool activate_value)
{
    VALIDATE_INSTANCE_NUMBER(instance_number);

    return impl->writeMessageAndWait(format("activate %d %d",
                                            instance_number, activate_value ? 1 : 0));
}

bool Host::preload(const char* const uri, const int16_t instance_number)
{
    VALIDATE_INSTANCE_NUMBER(instance_number);
    VALIDATE_URI(uri);

    return impl->writeMessageAndWait(format("preload %s %d", uri, instance_number));
}

bool Host::preset_load(const int16_t instance_number, const char* const preset_uri)
{
    VALIDATE_INSTANCE_NUMBER(instance_number);
    VALIDATE_URI(preset_uri);

    return impl->writeMessageAndWait(format("preset_load %d %s", instance_number, preset_uri));
}

bool Host::preset_save(const int16_t instance_number,
                       const char* const preset_name,
                       const char* const dir,
                       const char* const file_name)
{
    VALIDATE_INSTANCE_NUMBER(instance_number);

    return impl->writeMessageAndWait(format("preset_save %d %s %s %s",
                                            instance_number,
                                            escape(preset_name).c_str(),
                                            escape(dir).c_str(),
                                            escape(file_name).c_str()));
}

std::string Host::preset_show(const char* const preset_uri)
{
    VALIDATE_URI(preset_uri);

    HostResponse resp = {};
    if (impl->writeMessageAndWait(format("preset_show %s", preset_uri), kHostResponseString, &resp))
    {
        std::string ret(resp.data.s);
        std::free(resp.data.s);
        return ret;
    }

    return {};
}

bool Host::connect(const char* const origin_port, const char* const destination_port)
{
    VALIDATE_JACK_PORT(origin_port);
    VALIDATE_JACK_PORT(destination_port);

    return impl->writeMessageAndWait(format("connect %s %s",
                                            escape(origin_port).c_str(), escape(destination_port).c_str()));
}

bool Host::disconnect(const char* const origin_port, const char* const destination_port)
{
    VALIDATE_JACK_PORT(origin_port);
    VALIDATE_JACK_PORT(destination_port);

    return impl->writeMessageAndWait(format("disconnect %s %s",
                                            escape(origin_port).c_str(), escape(destination_port).c_str()));
}

bool Host::disconnect_all(const char* const origin_port)
{
    VALIDATE_JACK_PORT(origin_port);

    return impl->writeMessageAndWait(format("disconnect_all %s", escape(origin_port).c_str()));
}

bool Host::bypass(const int16_t instance_number, const bool bypass_value)
{
    VALIDATE_INSTANCE_NUMBER(instance_number);

    return impl->writeMessageAndWait(format("bypass %d %d", instance_number, bypass_value ? 1 : 0));
}

bool Host::param_set(const int16_t instance_number, const char* const param_symbol, const float param_value)
{
    VALIDATE_INSTANCE_NUMBER(instance_number);
    VALIDATE_SYMBOL(param_symbol);

    return impl->writeMessageAndWait(format("param_set %d %s %f", instance_number, param_symbol, param_value));
}

float Host::param_get(const int16_t instance_number, const char* const param_symbol)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    HostResponse resp = {};
    return impl->writeMessageAndWait(format("param_get %d %s", instance_number, param_symbol),
                                     kHostResponseFloat,
                                     &resp) ? resp.data.f : 0.f;
}

bool Host::param_monitor(const int16_t instance_number,
                         const char* const param_symbol,
                         const char* const cond_op,
                         const float value)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("param_monitor %d %s %s %f",
                                            instance_number, param_symbol, cond_op, value));
}

bool Host::params_flush(const int16_t instance_number,
                        const uint8_t reset_value,
                        const unsigned int param_count,
                        const flushed_param* const params)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)

    std::string msg = format("params_flush %d %u %u", instance_number, reset_value, param_count);

    for (unsigned int i = 0; i < param_count; ++i)
    {
        VALIDATE_SYMBOL(params[i].symbol)
        msg += format(" %s %f", params[i].symbol, params[i].value);
    }

    return impl->writeMessageAndWait(msg);
}

bool Host::patch_set(const int16_t instance_number, const char* const property_uri, const char* const value)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_URI(property_uri)

    return impl->writeMessageAndWait(format("patch_set %d %s %s",
                                            instance_number, property_uri, escape(value).c_str()));
}

bool Host::patch_get(const int16_t instance_number, const char* const property_uri)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_URI(property_uri)

    return impl->writeMessageAndWait(format("patch_get %d %s", instance_number, property_uri));
}

std::string Host::licensee(const int16_t instance_number)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)

    HostResponse resp = {};
    if (impl->writeMessageAndWait(format("licensee %d", instance_number), kHostResponseString, &resp))
    {
        std::string ret(resp.data.s);
        std::free(resp.data.s);
        return ret;
    }

    return {};
}

bool Host::monitor(const char* const addr, const int port, const bool status)
{
    return impl->writeMessageAndWait(format("monitor %s %d %d", addr, port, status ? 1 : 0));
}

bool Host::monitor_output(const int16_t instance_number, const char* const param_symbol, const bool enable)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("monitor_output%s %d %s",
                                            enable ? "" : "_off", instance_number, param_symbol));
}

bool Host::midi_learn(const int16_t instance_number,
                      const char* const param_symbol,
                      const float minimum,
                      const float maximum)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("midi_learn %d %s %f %f",
                                            instance_number, param_symbol, minimum, maximum));
}

bool Host::midi_map(const int16_t instance_number,
                    const char* const param_symbol,
                    const uint8_t midi_channel,
                    const uint8_t midi_cc,
                    const float minimum,
                    const float maximum)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_MIDI_CHANNEL(midi_channel)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("midi_map %d %s %d %d %f %f",
                                            instance_number, param_symbol, midi_channel, midi_cc, minimum, maximum));
}

bool Host::midi_unmap(const int16_t instance_number, const char* const param_symbol)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("midi_unmap %d %s", instance_number, param_symbol));
}

bool Host::monitor_audio_levels(const char* const source_port_name, bool enable)
{
    VALIDATE_JACK_PORT(source_port_name)

    return impl->writeMessageAndWait(format("monitor_audio_levels %s %d", source_port_name, enable ? 1 : 0));
}

bool Host::monitor_midi_program(const uint8_t midi_channel, const bool enable)
{
    VALIDATE_MIDI_CHANNEL(midi_channel)

    return impl->writeMessageAndWait(format("monitor_midi_program %d %d", midi_channel, enable ? 1 : 0));
}

bool Host::cc_map(const int16_t instance_number,
                  const char* const param_symbol,
                  const int device_id,
                  const int actuator_id,
                  const char* const label,
                  const float value,
                  const float minimum,
                  const float maximum,
                  const int steps,
                  const int extraflags,
                  const char* const unit,
                  const unsigned int scalepoints_count,
                  const cc_scalepoint* const scalepoints)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    std::string msg = format("cc_map %d %s %d %d %s %f %f %f %d %d %s %d",
                             instance_number,
                             param_symbol,
                             device_id,
                             actuator_id,
                             escape(label).c_str(),
                             value,
                             minimum,
                             maximum,
                             steps,
                             extraflags,
                             escape(unit).c_str(),
                             scalepoints_count);

    for (unsigned int i = 0; i < scalepoints_count; ++i)
        msg += format(" %s %f", escape(scalepoints[i].label).c_str(), scalepoints[i].value);

    return impl->writeMessageAndWait(msg);
}

bool Host::cc_unmap(const int16_t instance_number, const char* const param_symbol)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("cc_unmap %d %s", instance_number, param_symbol));
}

bool Host::cc_value_set(const int16_t instance_number, const char* const param_symbol, const float value)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("cc_value_set %d %s %f", instance_number, param_symbol, value));
}

bool Host::cv_map(const int16_t instance_number,
                  const char* const param_symbol,
                  const char* const source_port_name,
                  const float minimum,
                  const float maximum,
                  const char operational_mode)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_JACK_PORT(source_port_name)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("cv_map %d %s %s %f %f %c",
                                            instance_number,
                                            param_symbol,
                                            source_port_name,
                                            minimum,
                                            maximum,
                                            operational_mode));
}

bool Host::cv_unmap(const int16_t instance_number, const char* const param_symbol)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("cv_unmap %d %s", instance_number, param_symbol));
}

bool Host::hmi_map(const int16_t instance_number,
                   const char* const param_symbol,
                   const int hw_id,
                   const int page,
                   const int subpage,
                   const int caps,
                   const int flags,
                   const char* const label,
                   const float minimum,
                   const float maximum,
                   const int steps)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("hmi_map %d %s %d %d %d %d %d %s %f %f %d",
                                            instance_number,
                                            param_symbol,
                                            hw_id,
                                            page,
                                            subpage,
                                            caps,
                                            flags,
                                            escape(label).c_str(),
                                            minimum,
                                            maximum,
                                            steps));
}

bool Host::hmi_unmap(const int16_t instance_number, const char* const param_symbol)
{
    VALIDATE_INSTANCE_NUMBER(instance_number)
    VALIDATE_SYMBOL(param_symbol)

    return impl->writeMessageAndWait(format("hmi_unmap %d %s", instance_number, param_symbol));
}

float Host::cpu_load()
{
    HostResponse resp = {};
    return impl->writeMessageAndWait("cpu_load", kHostResponseFloat, &resp) ? resp.data.f : 0.f;
}

float Host::max_cpu_load()
{
    HostResponse resp = {};
    return impl->writeMessageAndWait("max_cpu_load", kHostResponseFloat, &resp) ? resp.data.f : 0.f;
}

bool Host::load(const char* const file_name)
{
    return impl->writeMessageAndWait(format("load %s", escape(file_name).c_str()));
}

bool Host::save(const char* const file_name)
{
    return impl->writeMessageAndWait(format("save %s", escape(file_name).c_str()));
}

bool Host::bundle_add(const char* const bundle_path)
{
    return impl->writeMessageAndWait(format("bundle_add %s", escape(bundle_path).c_str()));
}

bool Host::bundle_remove(const char* const bundle_path, const char* const resource)
{
    if (resource != nullptr && resource[0] != '\0')
    {
        VALIDATE_URI(resource);
        return impl->writeMessageAndWait(format("bundle_remove %s %s", escape(bundle_path).c_str(), resource));
    }

    return impl->writeMessageAndWait(format("bundle_remove %s \"\"", escape(bundle_path).c_str()));
}

bool Host::state_load(const char* const dir)
{
    return impl->writeMessageAndWait(format("state_load %s", escape(dir).c_str()));
}

bool Host::state_save(const char* const dir)
{
    return impl->writeMessageAndWait(format("state_save %s", escape(dir).c_str()));
}

bool Host::state_tmpdir(const char* const dir)
{
    return impl->writeMessageAndWait(format("state_tmpdir %s", escape(dir).c_str()));
}

bool Host::feature_enable(const Feature feature, const int value)
{
    switch (feature)
    {
    case kFeatureAggregatedMidi:
        return impl->writeMessageAndWait(format("feature_enable aggregated-midi %d", value != 0 ? 1 : 0));

    case kFeatureCpuLoad:
        return impl->writeMessageAndWait(format("feature_enable cpu-load %d", value != 0 ? 1 : 0));

    case kFeatureFreeWheeling:
        return impl->writeMessageAndWait(format("feature_enable freewheeling %d", value != 0 ? 1 : 0));

    case kFeatureProcessing:
        switch (static_cast<ProcessingType>(value))
        {
        case kProcessingOff:
        case kProcessingOn:
        case kProcessingOnWithDataReady:
        case kProcessingOffWithFadeOut:
        case kProcessingOffWithoutFadeOut:
        case kProcessingOnWithFadeIn:
            return impl->writeMessageAndWait(format("feature_enable processing %d", value));
        }

        impl->last_error = "Invalid value argument";
        return false;
    }

    impl->last_error = "Invalid feature argument";
    return false;
}

bool Host::set_bpb(const double beats_per_bar)
{
    VALIDATE_BPB(beats_per_bar)

    return impl->writeMessageAndWait(format("set_bpb %f", beats_per_bar));
}

bool Host::set_bpm(const double beats_per_minute)
{
    VALIDATE_BPM(beats_per_minute)

    return impl->writeMessageAndWait(format("set_bpm %f", beats_per_minute));
}

bool Host::transport(const bool rolling, const double beats_per_bar, const double beats_per_minute)
{
    VALIDATE_BPB(beats_per_bar)
    VALIDATE_BPM(beats_per_minute)

    return impl->writeMessageAndWait(format("transport %d %f %f", rolling ? 1 : 0, beats_per_bar, beats_per_minute));
}

bool Host::transport_sync(const TransportSync sync)
{
    switch (sync)
    {
    case kTransportSyncNone:
        return impl->writeMessageAndWait(format("transport_sync none"));
    case kTransportSyncAbletonLink:
        return impl->writeMessageAndWait(format("transport_sync link"));
    case kTransportSyncMIDI:
        return impl->writeMessageAndWait(format("transport_sync midi"));
    }

    impl->last_error = "Invalid sync argument";
    return false;
}

bool Host::output_data_ready()
{
    return impl->writeMessageAndWait("output_data_ready");
}

bool Host::poll_feedback(FeedbackCallback* const callback) const
{
    return impl->poll(callback);
}

bool Host::reconnect()
{
    return impl->reconnect();
}

// --------------------------------------------------------------------------------------------------------------------
