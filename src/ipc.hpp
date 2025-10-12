// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <string>

struct IPC
{
    /**
     * string describing the last error, in case any operation fails.
     * will also be set during initialization in case of IPC connection failure.
     */
    enum ResponseType {
        kResponseNone,
        kResponseInteger,
        kResponseFloat,
        kResponseString,
    };

    /**
     * string describing the last error, in case any operation fails.
     * will also be set during initialization in case of IPC connection failure.
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
     * constructor for IPC using serial port, specifying path to serial port and baudrate.
     */
    IPC(const char* serial, int baudrate);

    /**
     * constructor for TCP socket, specifying TCP port.
     */
    IPC(int tcpPort);

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
    void setWriteBlocking(bool blocking);

    /**
     * write a message and potentially fetch remote response.
     */
    bool writeMessage(const std::string& message, ResponseType respType = kResponseNone, Response* resp = nullptr);

private:
    struct Impl;
    Impl* const impl;
};
