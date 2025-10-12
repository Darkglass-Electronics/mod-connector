// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "ipc.hpp"
#include "utils.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>

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

struct IPC::Impl
{
    std::string& last_error;

    Impl(std::string& last_error_)
        : last_error(last_error_)
    {
       #ifdef __EMSCRIPTEN__
        dummyDevMode = true;
        return;
       #endif

        bufferSize = 128;
        buffer = static_cast<char*>(std::malloc(bufferSize));
        assert(buffer != nullptr);
    }

    ~Impl()
    {
        close();

        std::free(buffer);
    }

    void openSerial(const char* const path, const int baudrate)
    {
        if (dummyDevMode)
            return;

        iface = std::make_unique<Serial>(last_error, path, baudrate);
    }

    void openTCP(const int port)
    {
        if (const char* const dev = std::getenv("MOD_DEV_HOST"); std::atoi(dev) != 0)
            dummyDevMode = true;

        if (dummyDevMode)
            return;

        iface = std::make_unique<TCP>(last_error, port);
    }

    void close()
    {
        iface.reset();
    }

    char* readMessage(uint32_t* const bytesRead)
    {
        // read first byte
        char firstbyte = '\0';
        int r = iface->readMessageByte(&firstbyte);

        // nothing to read, quit
        if (r == 0)
        {
            last_error.clear();
            return nullptr;
        }

        if (r < 0)
        {
            last_error = "read error";
            return nullptr;
        }

        // set blocking mode, so we block-wait until message is fully delivered
        const int flags = iface->readBlocking();

        // read full message now
        buffer[0] = firstbyte;

        for (size_t read = 1;;)
        {
            r = iface->readMessageByte(buffer + read);

            /* Data received */
            if (r == 1)
            {
                // null terminator, stop
                if (buffer[read] == '\0')
                {
                    *bytesRead = read;
                    break;
                }

                // increase buffer by 2x for longer messages
                if (++read == bufferSize)
                {
                    bufferSize *= 2;
                    buffer = static_cast<char*>(std::realloc(buffer, bufferSize));
                }
            }
            /* Error */
            else if (r < 0)
            {
                last_error = "read error";
                return nullptr;
            }
            /* Client disconnected */
            else
            {
                last_error = "disconnected";
                return nullptr;
            }
        }

        // set non-blocking mode again
        iface->readNonBlocking(flags);

        return buffer;
    }

    void setWriteBlocking(bool blocking)
    {
        if (blocking)
        {
            assert(nonBlockingMode);
            nonBlockingMode = false;
            waitResponses();
        }
        else
        {
            assert(! nonBlockingMode);
            nonBlockingMode = true;
        }
    }

    bool writeMessage(const std::string& message, const ResponseType respType, Response* const resp)
    {
       #ifndef NDEBUG
        if (resp != nullptr)
        {
            assert(resp->code == 0);
        }
       #endif

        if (dummyDevMode)
        {
            if (resp != nullptr)
            {
                *resp = {};
                resp->code = SUCCESS;
                switch (respType)
                {
                case kResponseNone:
                    break;
                case kResponseFloat:
                    resp->data.f = 0.f;
                    break;
                case kResponseInteger:
                    resp->data.i = 0;
                    break;
                case kResponseString:
                    resp->data.s = strdup("");
                    break;
                }
            }
            return true;
        }

        if (! iface->writeMessage(message))
            return false;

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
                r = iface->readResponseByte(buffer + written);

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

           #if 0 // ndef NDEBUG
            if (canDebug) {
                mod_log_debug("%s: received response: '%s'", __func__, buffer);
            }
           #endif

            // special handling for string replies, read all incoming data
            if (respType == kResponseString)
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
                last_error = "reply is empty";
                return false;
            }

            if (std::strncmp(buffer, "r ", 2) == 0)
            {
                if (buffer[2] == '\0')
                {
                    last_error = "mod-host reply is incomplete (less than 3 characters)";
                    return false;
                }
            }
            else if (std::strncmp(buffer, "resp ", 5) == 0)
            {
                if (buffer[5] == '\0')
                {
                    last_error = "mod-host reply is incomplete (less than 6 characters)";
                    return false;
                }
            }
            else
            {
                last_error = "mod-host reply is malformed (missing 'r' or 'resp' prefix)";
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

            if (respcode < 0)
                return false;

            // stop here if not wanting response data
            if (resp == nullptr)
                return true;

            *resp = {};
            resp->code = respcode;

            switch (respType)
            {
            case kResponseNone:
            case kResponseString:
                break;
            case kResponseInteger:
                resp->data.i = respdata != nullptr
                             ? std::atoi(respdata)
                             : 0;
                break;
            case kResponseFloat:
                resp->data.f = respdata != nullptr
                             ? std::atof(respdata)
                             : 0.f;
                break;
            }
        }

        return true;
    }

private:
    bool waitResponses()
    {
        char c;
        int r;
        last_error.clear();

       #ifndef NDEBUG
        std::string cmd;
        uint64_t* times;

        if (_mod_log_level() >= 1)
        {
            cmd.reserve(8);
            times = new uint64_t[numNonBlockingOps + 1];
            times[numNonBlockingOps] = getTimeNS();
        }
        else
        {
            times = nullptr;
        }
       #endif

        mod_log_debug("%s: begin, numNonBlockingOps: %u", __func__, numNonBlockingOps);

        while (numNonBlockingOps != 0)
        {
            r = iface->readResponseByte(&c);

            /* Data received */
            if (r == 1)
            {
                // fprintf(stderr, "%c", c);
               #ifndef NDEBUG
                if (times != nullptr)
                    cmd += c;
               #endif

                if (c == '\0')
                {
                    --numNonBlockingOps;
                    mod_log_debug3("%s: next, numNonBlockingOps: %u", __func__, numNonBlockingOps);

                   #ifndef NDEBUG
                    if (times != nullptr)
                    {
                        times[numNonBlockingOps] = getTimeNS();
                        fprintf(stderr,
                                "wait %03u %12lu %s\n",
                                numNonBlockingOps,
                                times[numNonBlockingOps] - times[numNonBlockingOps + 1],
                                cmd.c_str());
                        cmd.clear();
                    }
                   #endif
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

       #ifndef NDEBUG
        delete[] times;
       #endif

        mod_log_debug("%s: end, numNonBlockingOps: %u", __func__, numNonBlockingOps);
        return true;
    }

    bool dummyDevMode = false;
    bool nonBlockingMode = false;
    uint16_t numNonBlockingOps = 0;

    char* buffer = nullptr;
    uint32_t bufferSize = 0;

    struct Interface {
        std::string& last_error;
        Interface(std::string& last_error_) : last_error(last_error_) {};
        virtual ~Interface() = default;
        [[nodiscard]] virtual int readBlocking() = 0;
        virtual void readNonBlocking(int flags) = 0;
        [[nodiscard]] virtual int readMessageByte(char* c) = 0;
        [[nodiscard]] virtual int readResponseByte(char* c) = 0;
        [[nodiscard]] virtual bool writeMessage(const std::string& message) = 0;
    };

    struct Serial : Interface {
        Serial(std::string& last_error, const char* path, int baudrate);
        ~Serial() override;
        [[nodiscard]] int readBlocking() final;
        void readNonBlocking(int flags) final;
        [[nodiscard]] int readMessageByte(char* c) final;
        [[nodiscard]] int readResponseByte(char* c) final;
        [[nodiscard]] bool writeMessage(const std::string& message) final;
    };

    struct TCP : Interface {
        TCP(std::string& last_error, int port);
        ~TCP() override;
        [[nodiscard]] int readBlocking() final;
        void readNonBlocking(int flags) final;
        [[nodiscard]] int readMessageByte(char* c) final;
        [[nodiscard]] int readResponseByte(char* c) final;
        [[nodiscard]] bool writeMessage(const std::string& message) final;

        bool wsaInitialized = false;

        struct {
            SOCKET out = INVALID_SOCKET;
            SOCKET feedback = INVALID_SOCKET;
        } sockets;
    };

    std::unique_ptr<Interface> iface;
};

// --------------------------------------------------------------------------------------------------------------------

IPC::Impl::TCP::TCP(std::string& last_error, const int port)
    : Interface(last_error)
{
    last_error.clear();

   #ifdef _WIN32
    if (! wsaInitialized)
    {
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
    }
   #endif

    SOCKET outsock, fbsock;

    if (outsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); outsock == INVALID_SOCKET)
    {
        last_error = "output socket error";
        return;
    }

    if (fbsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); fbsock == INVALID_SOCKET)
    {
        last_error = "feedback socket error";
        ::closesocket(outsock);
        return;
    }

   #ifndef _WIN32
    int value;

    /* increase socket size */
    constexpr const int socketsize = 131071;
    value = socketsize;
    setsockopt(outsock, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
    setsockopt(fbsock, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));

    /* set TCP_NODELAY */
    value = 1;
    setsockopt(outsock, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
    setsockopt(fbsock, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
   #endif

    /* Startup the socket struct */
    struct sockaddr_in serv_addr = {};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* Try assign the server address */
    serv_addr.sin_port = htons(port);
    if (::connect(outsock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ::closesocket(outsock);
        ::closesocket(fbsock);
        last_error = "output socket connect error";
        return;
    }

    serv_addr.sin_port = htons(port + 1);
    if (::connect(fbsock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ::closesocket(outsock);
        ::closesocket(fbsock);
        last_error = "feedback socket connect error";
        return;
    }

    /* set non-blocking mode on feedback port, so we can poke to see if there are any messages */
   #ifdef _WIN32
    unsigned long nonblocking = 1;
    ::ioctlsocket(fbsock, FIONBIO, &nonblocking);
   #else
    const int socketflags = ::fcntl(fbsock, F_GETFL);
    ::fcntl(fbsock, F_SETFL, socketflags | O_NONBLOCK);
   #endif

    sockets.out = outsock;
    sockets.feedback = fbsock;
}

IPC::Impl::TCP::~TCP()
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
    wsaInitialized = false;
   #endif
}

int IPC::Impl::TCP::readBlocking()
{
   #ifdef _WIN32
    unsigned long nonblocking = 0;
    ::ioctlsocket(sockets.feedback, FIONBIO, &nonblocking);
    return 0;
   #else
    const int socketflags = ::fcntl(sockets.feedback, F_GETFL);
    ::fcntl(sockets.feedback, F_SETFL, socketflags & ~O_NONBLOCK);
    return socketflags;
   #endif
}

void IPC::Impl::TCP::readNonBlocking(const int flags [[maybe_unused]])
{
   #ifdef _WIN32
    unsigned long nonblocking = 1;
    ::ioctlsocket(sockets.feedback, FIONBIO, &nonblocking);
   #else
    ::fcntl(sockets.feedback, F_SETFL, flags | O_NONBLOCK);
   #endif
}

int IPC::Impl::TCP::readMessageByte(char* const c)
{
    return recv(sockets.feedback, c, 1, 0);
}

int IPC::Impl::TCP::readResponseByte(char* const c)
{
    return recv(sockets.out, c, 1, 0);
}

bool IPC::Impl::TCP::writeMessage(const std::string& message)
{
    if (sockets.out == INVALID_SOCKET)
    {
        last_error = "socket is not connected";
        return false;
    }

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

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

IPC::IPC(const char* const serial, const int baudrate)
    : impl(new Impl(last_error))
{
    impl->openSerial(serial, baudrate);
}

IPC::IPC(const int tcpPort)
    : impl(new Impl(last_error))
{
    impl->openTCP(tcpPort);
}

IPC::~IPC()
{
    delete impl;
}

char* IPC::readMessage(uint32_t* const bytesRead)
{
    return impl->readMessage(bytesRead);
}

void IPC::setWriteBlocking(const bool blocking)
{
    impl->setWriteBlocking(blocking);
}

bool IPC::writeMessage(const std::string& message, const ResponseType respType, Response* const resp)
{
    return impl->writeMessage(message, respType, resp);
}

// --------------------------------------------------------------------------------------------------------------------
