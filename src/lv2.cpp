// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "lv2.hpp"

#include <cassert>
#include <cstring>
#include <cstdint>
#include <map>
#include <unordered_map>

#include <lilv/lilv.h>

#if defined(HAVE_LV2_1_18) || (defined(__has_include) && __has_include(<lv2/atom/atom.h>))
#include <lv2/atom/atom.h>
#include <lv2/morph/morph.h>
#include <lv2/patch/patch.h>
#include <lv2/port-props/port-props.h>
#include <lv2/units/units.h>
#include <lv2/state/state.h>
#else
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/morph/morph.h>
#include <lv2/lv2plug.in/ns/ext/patch/patch.h>
#include <lv2/lv2plug.in/ns/ext/port-props/port-props.h>
#include <lv2/lv2plug.in/ns/ext/state/state.h>
#include <lv2/lv2plug.in/ns/ext/units/units.h>
#endif

#ifndef LV2_CORE__Parameter
#define LV2_CORE__Parameter LV2_CORE_PREFIX "Parameter"
#endif

#ifndef LV2_CORE__enabled
#define LV2_CORE__enabled LV2_CORE_PREFIX "enabled"
#endif

#ifndef LV2_CORE__isSideChain
#define LV2_CORE__isSideChain LV2_CORE_PREFIX "isSideChain"
#endif

#ifndef LV2_CORE__shortName
#define LV2_CORE__shortName LV2_CORE_PREFIX "shortName"
#endif

#define LILV_NS_DARKGLASS "http://www.darkglass.com/lv2/ns"
#define DARKGLASS__abbreviation LILV_NS_DARKGLASS "#abbreviation"

#define LILV_NS_KXSTUDIO "http://kxstudio.sf.net/ns/lv2ext/props"
#define KXSTUDIO__Reset LILV_NS_KXSTUDIO "#Reset"

#define LILV_NS_MOD "http://moddevices.com/ns/mod#"
#define MOD__CVPort LILV_NS_MOD "CVPort"

#define LILV_NS_MODGUI "http://moddevices.com/ns/modgui#"

// --------------------------------------------------------------------------------------------------------------------

struct Lv2NamespaceDefinitions {
    LilvNode* const dargkglass_abbreviation;
    LilvNode* const lv2core_default;
    LilvNode* const lv2core_designation;
    LilvNode* const lv2core_minimum;
    LilvNode* const lv2core_maximum;
    LilvNode* const lv2core_portProperty;
    LilvNode* const lv2core_shortName;
    LilvNode* const modgui_gui;
    LilvNode* const modgui_resourcesDirectory;
    LilvNode* const modgui_screenshot;
    LilvNode* const patch_readable;
    LilvNode* const patch_writable;
    LilvNode* const rdf_type;
    LilvNode* const state_state;
    LilvNode* const units_unit;

    Lv2NamespaceDefinitions(LilvWorld* const world)
        : dargkglass_abbreviation(lilv_new_uri(world, DARKGLASS__abbreviation)),
          lv2core_default(lilv_new_uri(world, LV2_CORE__default)),
          lv2core_designation(lilv_new_uri(world, LV2_CORE__designation)),
          lv2core_minimum(lilv_new_uri(world, LV2_CORE__minimum)),
          lv2core_maximum(lilv_new_uri(world, LV2_CORE__maximum)),
          lv2core_portProperty(lilv_new_uri(world, LV2_CORE__portProperty)),
          lv2core_shortName(lilv_new_uri(world, LV2_CORE__shortName)),
          modgui_gui(lilv_new_uri(world, LILV_NS_MODGUI "gui")),
          modgui_resourcesDirectory(lilv_new_uri(world, LILV_NS_MODGUI "resourcesDirectory")),
          modgui_screenshot(lilv_new_uri(world, LILV_NS_MODGUI "screenshot")),
          patch_readable(lilv_new_uri(world, LV2_PATCH__readable)),
          patch_writable(lilv_new_uri(world, LV2_PATCH__writable)),
          rdf_type(lilv_new_uri(world, LILV_NS_RDF "type")),
          state_state(lilv_new_uri(world, LV2_STATE__state)),
          units_unit(lilv_new_uri(world, LV2_UNITS__unit))
    {
    }

    void free() const
    {
        lilv_node_free(dargkglass_abbreviation);
        lilv_node_free(lv2core_default);
        lilv_node_free(lv2core_designation);
        lilv_node_free(lv2core_minimum);
        lilv_node_free(lv2core_maximum);
        lilv_node_free(lv2core_portProperty);
        lilv_node_free(lv2core_shortName);
        lilv_node_free(modgui_gui);
        lilv_node_free(modgui_resourcesDirectory);
        lilv_node_free(modgui_screenshot);
        lilv_node_free(patch_readable);
        lilv_node_free(patch_writable);
        lilv_node_free(rdf_type);
        lilv_node_free(state_state);
        lilv_node_free(units_unit);
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
        assert(uri != nullptr);
        assert(*uri != '\0');

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
            // abbreviation

            if (LilvNodes* const nodes = lilv_plugin_get_value(plugin, ns.dargkglass_abbreviation))
            {
                retplugin->abbreviation = lilv_node_as_string(lilv_nodes_get_first(nodes));
                lilv_nodes_free(nodes);
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
                        else if (std::strcmp(cat, "DelayPlugin") == 0)
                            retplugin->category = kLv2CategoryDelay;
                        else if (std::strcmp(cat, "DistortionPlugin") == 0)
                            retplugin->category = kLv2CategoryDistortion;
                        else if (std::strcmp(cat, "WaveshaperPlugin") == 0)
                            retplugin->category = kLv2CategoryDistortionWaveshaper;
                        else if (std::strcmp(cat, "DynamicsPlugin") == 0)
                            retplugin->category = kLv2CategoryDynamics;
                        else if (std::strcmp(cat, "AmplifierPlugin") == 0)
                            retplugin->category = kLv2CategoryDynamicsAmplifier;
                        else if (std::strcmp(cat, "CompressorPlugin") == 0)
                            retplugin->category = kLv2CategoryDynamicsCompressor;
                        else if (std::strcmp(cat, "ExpanderPlugin") == 0)
                            retplugin->category = kLv2CategoryDynamicsExpander;
                        else if (std::strcmp(cat, "GatePlugin") == 0)
                            retplugin->category = kLv2CategoryDynamicsGate;
                        else if (std::strcmp(cat, "LimiterPlugin") == 0)
                            retplugin->category = kLv2CategoryDynamicsLimiter;
                        else if (std::strcmp(cat, "FilterPlugin") == 0)
                            retplugin->category = kLv2CategoryFilter;
                        else if (std::strcmp(cat, "AllpassPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterAllpass;
                        else if (std::strcmp(cat, "BandpassPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterBandpass;
                        else if (std::strcmp(cat, "CombPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterComb;
                        else if (std::strcmp(cat, "EQPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterEqualiser;
                        else if (std::strcmp(cat, "MultiEQPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterEqualiserMultiband;
                        else if (std::strcmp(cat, "ParaEQPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterEqualiserParametric;
                        else if (std::strcmp(cat, "HighpassPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterHighpass;
                        else if (std::strcmp(cat, "LowpassPlugin") == 0)
                            retplugin->category = kLv2CategoryFilterLowpass;
                        else if (std::strcmp(cat, "GeneratorPlugin") == 0)
                            retplugin->category = kLv2CategoryGenerator;
                        else if (std::strcmp(cat, "ConstantPlugin") == 0)
                            retplugin->category = kLv2CategoryGeneratorConstant;
                        else if (std::strcmp(cat, "InstrumentPlugin") == 0)
                            retplugin->category = kLv2CategoryGeneratorInstrument;
                        else if (std::strcmp(cat, "OscillatorPlugin") == 0)
                            retplugin->category = kLv2CategoryGeneratorOscillator;
                        else if (std::strcmp(cat, "ModulatorPlugin") == 0)
                            retplugin->category = kLv2CategoryModulator;
                        else if (std::strcmp(cat, "ChorusPlugin") == 0)
                            retplugin->category = kLv2CategoryModulatorChorus;
                        else if (std::strcmp(cat, "FlangerPlugin") == 0)
                            retplugin->category = kLv2CategoryModulatorFlanger;
                        else if (std::strcmp(cat, "PhaserPlugin") == 0)
                            retplugin->category = kLv2CategoryModulatorPhaser;
                        else if (std::strcmp(cat, "ReverbPlugin") == 0)
                            retplugin->category = kLv2CategoryReverb;
                        else if (std::strcmp(cat, "SimulatorPlugin") == 0)
                            retplugin->category = kLv2CategorySimulator;
                        else if (std::strcmp(cat, "SpatialPlugin") == 0)
                            retplugin->category = kLv2CategorySpatial;
                        else if (std::strcmp(cat, "SpectralPlugin") == 0)
                            retplugin->category = kLv2CategorySpectral;
                        else if (std::strcmp(cat, "PitchPlugin") == 0)
                            retplugin->category = kLv2CategorySpectralPitchShifter;
                        else if (std::strcmp(cat, "UtilityPlugin") == 0)
                            retplugin->category = kLv2CategoryUtility;
                        else if (std::strcmp(cat, "AnalyserPlugin") == 0)
                            retplugin->category = kLv2CategoryUtilityAnalyser;
                        else if (std::strcmp(cat, "ConverterPlugin") == 0)
                            retplugin->category = kLv2CategoryUtilityConverter;
                        else if (std::strcmp(cat, "FunctionPlugin") == 0)
                            retplugin->category = kLv2CategoryUtilityFunction;
                        else if (std::strcmp(cat, "MixerPlugin") == 0)
                            retplugin->category = kLv2CategoryUtilityMixer;
                        else if (std::strcmp(cat, "MIDIPlugin") == 0)
                            retplugin->category = kLv2CategoryMIDI;
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

                    if (LilvNodes* const nodes = lilv_port_get_value(plugin, port, ns.lv2core_shortName))
                    {
                        retport.shortname = lilv_node_as_string(lilv_nodes_get_first(nodes));
                        lilv_nodes_free(nodes);
                    }

                    /**/ if ((retport.flags & Lv2PortIsAudio) != 0)
                    {
                        if (LilvNodes* const nodes = lilv_port_get_value(plugin, port, ns.lv2core_portProperty))
                        {
                            LILV_FOREACH(nodes, itprop, nodes)
                            {
                                const char* const propuri = lilv_node_as_string(lilv_nodes_get(nodes, itprop));

                                if (std::strcmp(propuri, LV2_CORE__isSideChain) == 0)
                                    retport.flags |= Lv2PortIsSidechain;
                            }

                            lilv_nodes_free(nodes);
                        }
                    }
                    else if ((retport.flags & Lv2PortIsControl) != 0)
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
                                else if (std::strcmp(propuri, LV2_CORE__enumeration) == 0)
                                    retport.flags |= Lv2ParameterEnumerated;
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
                            else if (std::strcmp(designation, KXSTUDIO__Reset) == 0)
                                retport.designation = kLv2DesignationReset;

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

                        if (LilvScalePoints* const scalepoints = lilv_port_get_scale_points(plugin, port))
                        {
                            if (const unsigned int scalepointcount = lilv_scale_points_size(scalepoints))
                            {
                                // get all scalepoints and sort them by value
                                std::map<double, Lv2ScalePoint> sortedpoints;

                                LILV_FOREACH(scale_points, itscl, scalepoints)
                                {
                                    const LilvScalePoint* const scalepoint = lilv_scale_points_get(scalepoints, itscl);
                                    const LilvNode* const xlabel = lilv_scale_point_get_label(scalepoint);
                                    const LilvNode* const xvalue = lilv_scale_point_get_value(scalepoint);

                                    if (xlabel == nullptr || xvalue == nullptr)
                                        continue;

                                    const double valueid = lilv_node_as_float(xvalue);
                                    sortedpoints[valueid] = {
                                        .label = lilv_node_as_string(xlabel),
                                        .value = lilv_node_as_float(xvalue),
                                    };
                                }

                                // now store them sorted
                                for (auto& scalepoint : sortedpoints)
                                    retport.scalePoints.push_back(scalepoint.second);
                            }

                            lilv_scale_points_free(scalepoints);
                        }

                        if (LilvNodes* const uunits = lilv_port_get_value(plugin, port, ns.units_unit))
                        {
                            const char* uuri = lilv_node_as_uri(lilv_nodes_get_first(uunits));

                            // using pre-existing lv2 unit
                            if (uuri != nullptr && std::strncmp(uuri, LV2_UNITS_PREFIX, 38) == 0)
                            {
                                uuri += 38; // strlen(LV2_UNITS_PREFIX)

                                /**/ if (strcmp(uuri, "s") == 0)
                                    retport.unit = "s";
                                else if (strcmp(uuri, "ms") == 0)
                                    retport.unit = "ms";
                                else if (strcmp(uuri, "db") == 0)
                                    retport.unit = "dB";
                                else if (strcmp(uuri, "pc") == 0)
                                    retport.unit = "%";
                                else if (strcmp(uuri, "hz") == 0)
                                    retport.unit = "Hz";
                                else if (strcmp(uuri, "khz") == 0)
                                    retport.unit = "kHz";
                                else if (strcmp(uuri, "mhz") == 0)
                                    retport.unit = "MHz";
                                else if (strcmp(uuri, "cent") == 0)
                                    retport.unit = "ct";
                                else if (strcmp(uuri, "semitone12TET") == 0)
                                    retport.unit = "semi";
                            }

                            lilv_nodes_free(uunits);
                        }
                    }
                }
            }

            // --------------------------------------------------------------------------------------------------------
            // properties

            {
                std::map<std::string, Lv2Property> properties;

                const auto get_properties = [=](const bool writable)
                {
                    if (LilvNodes* const patches = lilv_plugin_get_value(plugin, writable ? ns.patch_writable
                                                                                          : ns.patch_readable))
                    {
                        // used to fetch default values
                        LilvNode* const statenode = lilv_world_get(world,
                                                                   lilv_plugin_get_uri(plugin),
                                                                   ns.state_state,
                                                                   nullptr);

                        LILV_FOREACH(nodes, itpatches, patches)
                        {
                            // TODO
                        }

                        lilv_node_free(statenode);
                        lilv_nodes_free(patches);
                    }
                };

                get_properties(true);
                get_properties(false);

                if (const size_t count = properties.size())
                {
                    retplugin->properties.reserve(count);

                    for (auto& prop : properties)
                        retplugin->properties.push_back(prop.second);
                }
            }

            // --------------------------------------------------------------------------------------------------------
            // screenshot

            if (LilvNodes* const nodes = lilv_plugin_get_value(plugin, ns.modgui_gui))
            {
                LILV_FOREACH(nodes, it, nodes)
                {
                    const LilvNode* const modgui = lilv_nodes_get(nodes, it);
                    lilv_world_load_resource(world, modgui);

                    LilvNode* const resdirn = lilv_world_get(world, modgui, ns.modgui_resourcesDirectory, nullptr);
                    if (resdirn == nullptr)
                        continue;

                    char* const resdir = lilv_file_uri_parse(lilv_node_as_string(resdirn), nullptr);
                    lilv_node_free(resdirn);

                    if (resdir == nullptr)
                        continue;

                    if (LilvNode* const modgui_scrn = lilv_world_get(world, modgui, ns.modgui_screenshot, nullptr))
                    {
                        if (char* const path = lilv_file_uri_parse(lilv_node_as_string(modgui_scrn), nullptr))
                        {
                            retplugin->screenshot = path;
                            lilv_free(path);
                        }
                        lilv_node_free(modgui_scrn);
                    }

                    lilv_free(resdir);
                    break;
                }

                lilv_nodes_free(nodes);
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
