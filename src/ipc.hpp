// SPDX-FileCopyrightText: 2024-2026 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <cstdint>
#include <string>

struct IPC
{
    /**
     * type of message response expected to be received.
     */
    enum ResponseType {
        kResponseNone,
        kResponseInteger,
        kResponseFloat,
        kResponseString,
    };

    /**
     * type of write error.
     */
    enum WriteError {
        kWriteSuccess,
        kWriteErrorBadReply,
        kWriteErrorDisconnected,
    };

    /**
     * message response with error code and optional data.
     * there is no generic handling, response is specific to the message being sent.
     */
    struct Response {
        int code;
        union {
            int i;
            float f;
            char* s;
        } data;
    };

    /**
     * function type used for callback-based IPC.
     */
    typedef bool (*SendCallback)(void* ptr, const char* msg);

    /**
     * function type used for callback-based IPC.
     */
    typedef const char* (*RecvCallback)(void* ptr, uint32_t* size);

    /**
     * create IPC using a serial port, specifying path to serial port and baudrate.
     */
    static IPC* createSerialPortIPC(const char* serial, int baudrate);

    /**
     * create IPC using a TCP socket, specifying TCP port and if server or client.
     * @note creation of a server is a blocking operation until a client is connected.
     */
    static IPC* createSingleSocketIPC(int tcpPort, bool isServer);

    /**
     * create IPC using dual TCP sockets (one out-going, one receiving), specifying TCP port.
     */
    static IPC* createDualSocketIPC(int tcpPort);

    /**
     * create IPC using dual callbacks (one out-going, one receiving).
     */
    static IPC* createDualCallbackIPC(SendCallback send, RecvCallback reply, RecvCallback feedback, void* userPtr);

    /**
     * destructor.
     */
    ~IPC();

    /**
     * string describing the last error, in case any operation fails.
     * will also be set during initialization in case of IPC connection failure.
     */
    std::string last_error;

    /**
     * read a message.
     * returned value must not be freed, but it can be safely modified.
     */
    char* readMessage(uint32_t* bytesRead);

    /**
     * change writing blocking mode.
     * will wait for all responses if writing becomes blocking.
     */
    void setWriteBlockingAndWait(bool blocking);

    /**
     * write a message and potentially fetch remote response.
     */
    WriteError writeMessage(const std::string& message,
                            ResponseType respType = kResponseNone,
                            Response* resp = nullptr);

    /**
     * write a message without a reply, typically used for replies themselves.
     */
    WriteError writeMessageWithoutReply(const std::string& message);

protected:
    IPC();

private:
    struct Impl;
    Impl* const impl;
};
