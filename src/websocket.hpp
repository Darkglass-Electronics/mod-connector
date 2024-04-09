// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>

// --------------------------------------------------------------------------------------------------------------------

struct WebSocket
{
   /**
    * string describing the last error, in case any operation fails.
    * will also be set during initialization in case of mod-host connection failure.
    */
    std::string last_error;

    bool listen(uint16_t port);

    WebSocket();
    ~WebSocket();

private:
    struct Impl;
    Impl* const impl;
};

// --------------------------------------------------------------------------------------------------------------------
