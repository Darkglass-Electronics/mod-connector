// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"

int main()
{
    HostConnector c;
    // test for sord related issues, see https://github.com/drobilla/sord/issues/10
    const void* _;
    _ = c.lv2world.getPluginByURI("urn:darkglass-anagram:cabinet-bass");
    _ = c.lv2world.getPluginByURI("urn:darkglass:Sublemon");
    return 0;
}
