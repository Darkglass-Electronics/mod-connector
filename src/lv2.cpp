// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "lv2.hpp"

#include <cstring>
#include <cstdint>
#include <unordered_map>

#include <lilv/lilv.h>

#ifdef HAVE_LV2_1_18
#include <lv2/atom/atom.h>
#include <lv2/morph/morph.h>
#include <lv2/port-props/port-props.h>
#else
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/morph/morph.h>
#include <lv2/lv2plug.in/ns/ext/port-props/port-props.h>
#endif

#define MOD__CVPort "http://moddevices.com/ns/mod#CVPort"

// --------------------------------------------------------------------------------------------------------------------

struct Lv2NamespaceDefinitions {
    LilvNode* const lv2core_default;
    LilvNode* const lv2core_designation;
    LilvNode* const lv2core_minimum;
    LilvNode* const lv2core_maximum;
    LilvNode* const lv2core_portProperty;
    LilvNode* const rdf_type;

    Lv2NamespaceDefinitions(LilvWorld* const world)
        : lv2core_default(lilv_new_uri(world, LV2_CORE__default)),
          lv2core_designation(lilv_new_uri(world, LV2_CORE__designation)),
          lv2core_minimum(lilv_new_uri(world, LV2_CORE__minimum)),
          lv2core_maximum(lilv_new_uri(world, LV2_CORE__maximum)),
          lv2core_portProperty(lilv_new_uri(world, LV2_CORE__portProperty)),
          rdf_type(lilv_new_uri(world, LILV_NS_RDF "type"))
    {
    }

    void free()
    {
        lilv_node_free(lv2core_default);
        lilv_node_free(lv2core_designation);
        lilv_node_free(lv2core_minimum);
        lilv_node_free(lv2core_maximum);
        lilv_node_free(lv2core_portProperty);
        lilv_node_free(rdf_type);
    }
};

// --------------------------------------------------------------------------------------------------------------------

struct Lv2World::Impl
{
    Impl(std::string& last_error_)
        : last_error(last_error_),
          world(lilv_world_new()),
          ns(world)
    {
        lilv_world_load_all(world);

        plugins = lilv_world_get_all_plugins(world);

        const uint32_t plugincount = lilv_plugins_size(plugins);
        pluginuris.reserve(plugincount);

        LILV_FOREACH(plugins, it, plugins)
        {
            const LilvPlugin* const p = lilv_plugins_get(plugins, it);

            const std::string uri(lilv_node_as_uri(lilv_plugin_get_uri(p)));
            pluginuris.push_back(uri);
            pluginscache[uri] = nullptr;
        }
    }

    ~Impl()
    {
        for (const auto& pair : pluginscache)
            delete pair.second;

        ns.free();
        lilv_world_free(world);
    }

    // ----------------------------------------------------------------------------------------------------------------

    uint32_t getPluginCount() const noexcept
    {
        return pluginuris.size();
    }

    const Lv2Plugin* getPluginByIndex(const uint32_t index)
    {
        return getPluginByURI(pluginuris[index].c_str());
    }

    const Lv2Plugin* getPluginByURI(const char* const uri)
    {
        if (pluginscache[uri] == nullptr)
        {
            LilvNode* const urinode = lilv_new_uri(world, uri);

            if (urinode == nullptr)
            {
                last_error = "Invalid URI";
                return nullptr;
            }

            const LilvPlugin* const plugin = lilv_plugins_get_by_uri(plugins, urinode);

            lilv_node_free(urinode);

            if (plugin == nullptr)
            {
                last_error = "Invalid Plugin";
                return nullptr;
            }

            Lv2Plugin* const retplugin = new Lv2Plugin;

            retplugin->uri = uri;

            // --------------------------------------------------------------------------------------------------------
            // name

            if (LilvNode* const node = lilv_plugin_get_name(plugin))
            {
                retplugin->name = lilv_node_as_string(node);
                lilv_node_free(node);
            }

            // --------------------------------------------------------------------------------------------------------
            // category

            if (LilvNodes* const nodes = lilv_plugin_get_value(plugin, ns.rdf_type))
            {
                LILV_FOREACH(nodes, it, nodes)
                {
                    const LilvNode* const node2 = lilv_nodes_get(nodes, it);
                    const char* const nodestr = lilv_node_as_string(node2);

                    if (nodestr == nullptr)
                        continue;

                    if (const char* cat = std::strstr(nodestr, LV2_CORE_PREFIX))
                    {
                        cat += 29; // strlen(LV2_CORE_PREFIX)

                        /**/ if (std::strcmp(cat, "Plugin") == 0)
                            continue;
                        else if (std::strcmp(cat, "FilterPlugin") == 0)
                            retplugin->category = kLv2CategoryFilter;
                        else if (std::strcmp(cat, "ReverbPlugin") == 0)
                            retplugin->category = kLv2CategoryReverb;
                        else if (std::strcmp(cat, "UtilityPlugin") == 0)
                            retplugin->category = kLv2CategoryUtility;
                    }
                }

                lilv_nodes_free(nodes);
            }

            // --------------------------------------------------------------------------------------------------------
            // ports

            if (const uint32_t numports = lilv_plugin_get_num_ports(plugin))
            {
                bool supported = true;

                for (uint32_t i = 0; i < numports; ++i)
                {
                    const LilvPort* const port = lilv_plugin_get_port_by_index(plugin, i);

                    bool hasDirection = false;
                    bool isGood = false;

                    if (LilvNodes* const typenodes = lilv_port_get_value(plugin, port, ns.rdf_type))
                    {
                        LILV_FOREACH(nodes, it, typenodes)
                        {
                            const char* const typestr = lilv_node_as_string(lilv_nodes_get(typenodes, it));

                            if (typestr == nullptr)
                                continue;

                            // ignore raw port type
                            if (std::strcmp(typestr, LV2_CORE__Port) == 0)
                                continue;

                            // direction (input or output) is required
                            if (std::strcmp(typestr, LV2_CORE__InputPort) == 0) {
                                hasDirection = true;
                                continue;
                            }
                            if (std::strcmp(typestr, LV2_CORE__OutputPort) == 0) {
                                hasDirection = true;
                                continue;
                            }

                            // ignore morph ports if base type is supported
                            if (std::strcmp(typestr, LV2_MORPH__MorphPort) == 0)
                                continue;

                            // check base type, must be supported
                            if (std::strcmp(typestr, LV2_ATOM__AtomPort) == 0) {
                                isGood = true;
                                continue;
                            }
                            if (std::strcmp(typestr, LV2_CORE__AudioPort) == 0) {
                                isGood = true;
                                continue;
                            }
                            if (std::strcmp(typestr, LV2_CORE__ControlPort) == 0) {
                                isGood = true;
                                continue;
                            }
                            if (std::strcmp(typestr, LV2_CORE__CVPort) == 0) {
                                isGood = true;
                                continue;
                            }
                            if (std::strcmp(typestr, MOD__CVPort) == 0) {
                                isGood = true;
                                continue;
                            }
                        }
                        lilv_nodes_free(typenodes);
                    }

                    if (! (hasDirection && isGood))
                    {
                        supported = false;
                        break;
                    }
                }

                if (! supported)
                {
                    last_error = "Plugin uses non-supported port types";
                    return nullptr;
                }

                retplugin->ports.resize(numports);
 
                for (uint32_t i = 0; i < numports; ++i)
                {
                    const LilvPort* const port = lilv_plugin_get_port_by_index(plugin, i);
                    Lv2Port& retport(retplugin->ports[i]);

                    if (LilvNodes* const nodes = lilv_port_get_value(plugin, port, ns.rdf_type))
                    {
                        LILV_FOREACH(nodes, it, nodes)
                        {
                            const LilvNode* const node2 = lilv_nodes_get(nodes, it);
                            const char* const nodestr = lilv_node_as_string(node2);

                            /**/ if (std::strcmp(nodestr, LV2_CORE__OutputPort) == 0)
                                retport.flags |= Lv2PortIsOutput;
                            else if (std::strcmp(nodestr, LV2_CORE__AudioPort) == 0)
                                retport.flags |= Lv2PortIsAudio;
                            else if (std::strcmp(nodestr, LV2_CORE__ControlPort) == 0)
                                retport.flags |= Lv2PortIsControl;
                        }
                        lilv_nodes_free(nodes);
                    }

                    if (const LilvNode* const symbolnode = lilv_port_get_symbol(plugin, port))
                        retport.symbol = lilv_node_as_string(symbolnode);

                    if (LilvNode* const node = lilv_port_get_name(plugin, port))
                    {
                        retport.name = lilv_node_as_string(node);
                        lilv_node_free(node);
                    }

                    if (retport.flags & Lv2PortIsControl)
                    {
                        if (LilvNodes* const nodes = lilv_port_get_value(plugin, port, ns.lv2core_portProperty))
                        {
                            LILV_FOREACH(nodes, itprop, nodes)
                            {
                                const char* const propuri = lilv_node_as_string(lilv_nodes_get(nodes, itprop));

                                /**/ if (std::strcmp(propuri, LV2_CORE__toggled) == 0)
                                    retport.flags |= Lv2ParameterToggled;
                                else if (std::strcmp(propuri, LV2_CORE__integer) == 0)
                                    retport.flags |= Lv2ParameterInteger;
                                else if (std::strcmp(propuri, LV2_PORT_PROPS__notOnGUI) == 0)
                                    retport.flags |= Lv2ParameterHidden;
                            }

                            lilv_nodes_free(nodes);
                        }

                        if (LilvNodes* const xdesignation = lilv_port_get_value(plugin, port, ns.lv2core_designation))
                        {
                            const char* const designation = lilv_node_as_string(lilv_nodes_get_first(xdesignation));

                            if (std::strcmp(designation, LV2_CORE__enabled) == 0)
                                retport.designation = kLv2DesignationEnabled;

                            // TODO define quick pot URI and spec

                            lilv_nodes_free(xdesignation);
                        }

                        LilvNodes* const xminimum = lilv_port_get_value(plugin, port, ns.lv2core_minimum);
                        LilvNodes* const xmaximum = lilv_port_get_value(plugin, port, ns.lv2core_maximum);
                        LilvNodes* const xdefault = lilv_port_get_value(plugin, port, ns.lv2core_default);

                        if (xminimum != nullptr && xmaximum != nullptr)
                        {
                            retport.min = lilv_node_as_float(lilv_nodes_get_first(xminimum));
                            retport.max = lilv_node_as_float(lilv_nodes_get_first(xmaximum));

                            if (retport.min >= retport.max)
                                retport.max = retport.min + 1.f;

                            if (xdefault != nullptr)
                            {
                                retport.def = lilv_node_as_float(lilv_nodes_get_first(xdefault));

                                if (retport.def < retport.min)
                                    retport.def = retport.min;
                                else if (retport.def > retport.max)
                                    retport.def = retport.max;
                            }
                            else
                            {
                                retport.def = retport.min;
                            }
                        }

                        lilv_nodes_free(xminimum);
                        lilv_nodes_free(xmaximum);
                        lilv_nodes_free(xdefault);
                    }
                }
            }

            pluginscache[uri] = retplugin;
            return retplugin;
        }

        return pluginscache[uri];
    }

private:
    std::string& last_error;

    LilvWorld* const world = nullptr;
    const LilvPlugins* plugins = nullptr;

    Lv2NamespaceDefinitions ns;
    std::vector<std::string> pluginuris;
    std::unordered_map<std::string, const Lv2Plugin*> pluginscache;
};

// --------------------------------------------------------------------------------------------------------------------

Lv2World::Lv2World() : impl(new Impl(last_error)) {}
Lv2World::~Lv2World() { delete impl; }

// --------------------------------------------------------------------------------------------------------------------

uint32_t Lv2World::get_plugin_count() const noexcept
{
    return impl->getPluginCount();
}

const Lv2Plugin* Lv2World::get_plugin_by_index(const uint32_t index) const
{
    return impl->getPluginByIndex(index);
}

const Lv2Plugin* Lv2World::get_plugin_by_uri(const char* const uri) const
{
    return impl->getPluginByURI(uri);
}

// --------------------------------------------------------------------------------------------------------------------
