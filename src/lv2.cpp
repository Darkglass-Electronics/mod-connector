// SPDX-FileCopyrightText: 2024-2026 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#define MOD_LOG_GROUP "lv2"

#include "lv2.hpp"
#include "utils.hpp"
#include "sha1/sha1.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
#include <list>
#include <map>

#include <lilv/lilv.h>

#ifdef _WIN32
#include <io.h>
#endif

#if defined(HAVE_LV2_1_18) || (defined(__has_include) && __has_include(<lv2/atom/atom.h>))
#include <lv2/atom/atom.h>
#include <lv2/morph/morph.h>
#include <lv2/patch/patch.h>
#include <lv2/port-props/port-props.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/units/units.h>
#else
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/morph/morph.h>
#include <lv2/lv2plug.in/ns/ext/patch/patch.h>
#include <lv2/lv2plug.in/ns/ext/port-props/port-props.h>
#include <lv2/lv2plug.in/ns/ext/state/state.h>
#include <lv2/lv2plug.in/ns/ext/time/time.h>
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

#define LV2_DARKGLASS_PROPERTIES_URI    "http://www.darkglass.com/lv2/ns"
#define LV2_DARKGLASS_PROPERTIES_PREFIX LV2_DARKGLASS_PROPERTIES_URI "#"

#define LV2_DARKGLASS_PROPERTIES__abbreviation          LV2_DARKGLASS_PROPERTIES_PREFIX "abbreviation"
#define LV2_DARKGLASS_PROPERTIES__blockImageOff         LV2_DARKGLASS_PROPERTIES_PREFIX "blockImageOff"
#define LV2_DARKGLASS_PROPERTIES__blockImageOn          LV2_DARKGLASS_PROPERTIES_PREFIX "blockImageOn"
#define LV2_DARKGLASS_PROPERTIES__mayUpdateBlockedState LV2_DARKGLASS_PROPERTIES_PREFIX "mayUpdateBlockedState"
#define LV2_DARKGLASS_PROPERTIES__oneDecimalPoint       LV2_DARKGLASS_PROPERTIES_PREFIX "oneDecimalPoint"
#define LV2_DARKGLASS_PROPERTIES__quickPot              LV2_DARKGLASS_PROPERTIES_PREFIX "quickPot"
#define LV2_DARKGLASS_PROPERTIES__savedToPreset         LV2_DARKGLASS_PROPERTIES_PREFIX "savedToPreset"

#define LV2_DARKGLASS_BLOCK_STYLING_URI    "http://www.darkglass.com/lv2/ns/lv2ext/block-styling"
#define LV2_DARKGLASS_BLOCK_STYLING_PREFIX LV2_DARKGLASS_BLOCK_STYLING_URI "#"

#define LV2_DARKGLASS_BLOCK_STYLING__alignBottomLeft  LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignBottomLeft"
#define LV2_DARKGLASS_BLOCK_STYLING__alignBottomMid   LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignBottomMid"
#define LV2_DARKGLASS_BLOCK_STYLING__alignBottomRight LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignBottomRight"
#define LV2_DARKGLASS_BLOCK_STYLING__alignCenter      LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignCenter"
#define LV2_DARKGLASS_BLOCK_STYLING__alignLeftMid     LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignLeftMid"
#define LV2_DARKGLASS_BLOCK_STYLING__alignRightMid    LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignRightMid"
#define LV2_DARKGLASS_BLOCK_STYLING__alignTopLeft     LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignTopLeft"
#define LV2_DARKGLASS_BLOCK_STYLING__alignTopMid      LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignTopMid"
#define LV2_DARKGLASS_BLOCK_STYLING__alignTopRight    LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignTopRight"
#define LV2_DARKGLASS_BLOCK_STYLING__alignment        LV2_DARKGLASS_BLOCK_STYLING_PREFIX "alignment"
#define LV2_DARKGLASS_BLOCK_STYLING__background       LV2_DARKGLASS_BLOCK_STYLING_PREFIX "background"
#define LV2_DARKGLASS_BLOCK_STYLING__backgroundScenes LV2_DARKGLASS_BLOCK_STYLING_PREFIX "backgroundScenes"
#define LV2_DARKGLASS_BLOCK_STYLING__blockName        LV2_DARKGLASS_BLOCK_STYLING_PREFIX "blockName"
#define LV2_DARKGLASS_BLOCK_STYLING__blockStyling     LV2_DARKGLASS_BLOCK_STYLING_PREFIX "blockStyling"
#define LV2_DARKGLASS_BLOCK_STYLING__bypass           LV2_DARKGLASS_BLOCK_STYLING_PREFIX "bypass"
#define LV2_DARKGLASS_BLOCK_STYLING__circle           LV2_DARKGLASS_BLOCK_STYLING_PREFIX "circle"
#define LV2_DARKGLASS_BLOCK_STYLING__fader            LV2_DARKGLASS_BLOCK_STYLING_PREFIX "fader"
#define LV2_DARKGLASS_BLOCK_STYLING__list             LV2_DARKGLASS_BLOCK_STYLING_PREFIX "list"
#define LV2_DARKGLASS_BLOCK_STYLING__paginationDots   LV2_DARKGLASS_BLOCK_STYLING_PREFIX "paginationDots"
#define LV2_DARKGLASS_BLOCK_STYLING__parameterStartPadding LV2_DARKGLASS_BLOCK_STYLING_PREFIX "parameterStartPadding"
#define LV2_DARKGLASS_BLOCK_STYLING__path             LV2_DARKGLASS_BLOCK_STYLING_PREFIX "path"
#define LV2_DARKGLASS_BLOCK_STYLING__size             LV2_DARKGLASS_BLOCK_STYLING_PREFIX "size"
#define LV2_DARKGLASS_BLOCK_STYLING__toggle           LV2_DARKGLASS_BLOCK_STYLING_PREFIX "toggle"
#define LV2_DARKGLASS_BLOCK_STYLING__widget           LV2_DARKGLASS_BLOCK_STYLING_PREFIX "widget"
#define LV2_DARKGLASS_BLOCK_STYLING__widgets          LV2_DARKGLASS_BLOCK_STYLING_PREFIX "widgets"

#define LILV_NS_KXSTUDIO "http://kxstudio.sf.net/ns/lv2ext/props"
#define KXSTUDIO__Reset LILV_NS_KXSTUDIO "#Reset"

#define LILV_NS_MOD "http://moddevices.com/ns/mod#"
#define MOD__CVPort LILV_NS_MOD "CVPort"

// --------------------------------------------------------------------------------------------------------------------
// compatibility functions

#ifdef _WIN32
static char* realpath(const char* const name, char* const resolved)
{
    if (name == nullptr)
        return nullptr;

    if (_access(name, 4) != 0)
        return nullptr;

    char* retname = nullptr;

    if ((retname = resolved) == nullptr)
        retname = static_cast<char*>(malloc(PATH_MAX + 2));

    if (retname == nullptr)
        return nullptr;

    return _fullpath(retname, name, PATH_MAX);
}
#endif

// --------------------------------------------------------------------------------------------------------------------
// get license keys directory
// NOTE: returned value always has path separator as the last character

static std::string _keysdir()
{
    if (const char* const keysdir = std::getenv("MOD_KEYS_PATH"))
    {
        assert(*keysdir != '\0');
        assert(keysdir[std::strlen(keysdir) - 1] == PATH_SEP_CHAR);
        return keysdir;
    }

    return homedir() + PATH_SEP_STR "keys" PATH_SEP_STR;
}

// --------------------------------------------------------------------------------------------------------------------
// proper lilv_file_uri_parse function that returns absolute paths

static char* _lilv_file_abspath(const LilvNode* const node)
{
    assert(lilv_node_is_uri(node));

    if (char* const lilvpath = lilv_file_uri_parse(lilv_node_as_uri(node), nullptr))
    {
        char* const ret = realpath(lilvpath, nullptr);
        lilv_free(lilvpath);
        return ret;
    }

    return nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// adjust bundle safely to lilv, as it wants the last character as the separator
// this also ensures paths are always written the same way
// NOTE: returned value must not be freed or cached

static const char* _realpath_with_terminator(const char* const bundle)
{
    static char _tmppath[PATH_MAX + 2];
    char* const bundlepath = realpath(bundle, _tmppath);

    if (bundlepath == nullptr)
        return nullptr;

    const size_t bundlepathsize = std::strlen(bundlepath);

    if (bundlepathsize <= 1)
        return nullptr;

    if (bundlepath[bundlepathsize] != PATH_SEP_CHAR)
    {
        bundlepath[bundlepathsize] = PATH_SEP_CHAR;
        bundlepath[bundlepathsize + 1] = '\0';
    }

    return bundlepath;
}

// --------------------------------------------------------------------------------------------------------------------
// hash the contents of a string

static std::string _sha1(const char* const cstring)
{
    sha1nfo s;
    sha1_init(&s);
    sha1_write(&s, cstring, strlen(cstring));

    char hashdec[HASH_LENGTH * 2 + 1];

    uint8_t* const hashenc = sha1_result(&s);
    for (int i = 0; i < HASH_LENGTH; i++)
        snprintf(hashdec + (i * 2), 3, "%02x", hashenc[i]);
    hashdec[HASH_LENGTH * 2] = '\0';

    return hashdec;
}

// --------------------------------------------------------------------------------------------------------------------

struct Lv2NamespaceDefinitions {
    LilvNode* const darkglass_abbreviation;
    LilvNode* const darkglass_blockImageOff;
    LilvNode* const darkglass_blockImageOn;
    LilvNode* const dgbs_alignment;
    LilvNode* const dgbs_background;
    LilvNode* const dgbs_backgroundScenes;
    LilvNode* const dgbs_blockName;
    LilvNode* const dgbs_blockStyling;
    LilvNode* const dgbs_bypass;
    LilvNode* const dgbs_circle;
    LilvNode* const dgbs_fader;
    LilvNode* const dgbs_list;
    LilvNode* const dgbs_paginationDots;
    LilvNode* const dgbs_parameterStartPadding;
    LilvNode* const dgbs_path;
    LilvNode* const dgbs_size;
    LilvNode* const dgbs_toggle;
    LilvNode* const dgbs_widget;
    LilvNode* const dgbs_widgets;
    LilvNode* const lv2core_default;
    LilvNode* const lv2core_designation;
    LilvNode* const lv2core_minimum;
    LilvNode* const lv2core_maximum;
    LilvNode* const lv2core_portProperty;
    LilvNode* const lv2core_shortName;
    LilvNode* const modlicense_interface;
    LilvNode* const patch_readable;
    LilvNode* const patch_writable;
    LilvNode* const rdf_type;
    LilvNode* const rdfs_label;
    LilvNode* const rdfs_range;
    LilvNode* const state_state;
    LilvNode* const units_unit;

    Lv2NamespaceDefinitions(LilvWorld* const world)
        : darkglass_abbreviation(lilv_new_uri(world, LV2_DARKGLASS_PROPERTIES__abbreviation)),
          darkglass_blockImageOff(lilv_new_uri(world, LV2_DARKGLASS_PROPERTIES__blockImageOff)),
          darkglass_blockImageOn(lilv_new_uri(world, LV2_DARKGLASS_PROPERTIES__blockImageOn)),
          dgbs_alignment(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__alignment)),
          dgbs_background(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__background)),
          dgbs_backgroundScenes(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__backgroundScenes)),
          dgbs_blockName(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__blockName)),
          dgbs_blockStyling(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__blockStyling)),
          dgbs_bypass(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__bypass)),
          dgbs_circle(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__circle)),
          dgbs_fader(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__fader)),
          dgbs_list(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__list)),
          dgbs_paginationDots(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__paginationDots)),
          dgbs_parameterStartPadding(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__parameterStartPadding)),
          dgbs_path(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__path)),
          dgbs_size(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__size)),
          dgbs_toggle(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__toggle)),
          dgbs_widget(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__widget)),
          dgbs_widgets(lilv_new_uri(world, LV2_DARKGLASS_BLOCK_STYLING__widgets)),
          lv2core_default(lilv_new_uri(world, LV2_CORE__default)),
          lv2core_designation(lilv_new_uri(world, LV2_CORE__designation)),
          lv2core_minimum(lilv_new_uri(world, LV2_CORE__minimum)),
          lv2core_maximum(lilv_new_uri(world, LV2_CORE__maximum)),
          lv2core_portProperty(lilv_new_uri(world, LV2_CORE__portProperty)),
          lv2core_shortName(lilv_new_uri(world, LV2_CORE__shortName)),
          modlicense_interface(lilv_new_uri(world, "http://moddevices.com/ns/ext/license#interface")),
          patch_readable(lilv_new_uri(world, LV2_PATCH__readable)),
          patch_writable(lilv_new_uri(world, LV2_PATCH__writable)),
          rdf_type(lilv_new_uri(world, LILV_NS_RDF "type")),
          rdfs_label(lilv_new_uri(world, LILV_NS_RDFS "label")),
          rdfs_range(lilv_new_uri(world, LILV_NS_RDFS "range")),
          state_state(lilv_new_uri(world, LV2_STATE__state)),
          units_unit(lilv_new_uri(world, LV2_UNITS__unit))
    {
    }

    void free() const
    {
        lilv_node_free(darkglass_abbreviation);
        lilv_node_free(darkglass_blockImageOff);
        lilv_node_free(darkglass_blockImageOn);
        lilv_node_free(dgbs_alignment);
        lilv_node_free(dgbs_background);
        lilv_node_free(dgbs_backgroundScenes);
        lilv_node_free(dgbs_blockStyling);
        lilv_node_free(dgbs_blockName);
        lilv_node_free(dgbs_bypass);
        lilv_node_free(dgbs_circle);
        lilv_node_free(dgbs_fader);
        lilv_node_free(dgbs_list);
        lilv_node_free(dgbs_paginationDots);
        lilv_node_free(dgbs_parameterStartPadding);
        lilv_node_free(dgbs_path);
        lilv_node_free(dgbs_size);
        lilv_node_free(dgbs_toggle);
        lilv_node_free(dgbs_widget);
        lilv_node_free(dgbs_widgets);
        lilv_node_free(lv2core_default);
        lilv_node_free(lv2core_designation);
        lilv_node_free(lv2core_minimum);
        lilv_node_free(lv2core_maximum);
        lilv_node_free(lv2core_portProperty);
        lilv_node_free(lv2core_shortName);
        lilv_node_free(modlicense_interface);
        lilv_node_free(patch_readable);
        lilv_node_free(patch_writable);
        lilv_node_free(rdf_type);
        lilv_node_free(rdfs_label);
        lilv_node_free(rdfs_range);
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
        pluginuris.reserve(lilv_plugins_size(plugins));

        LILV_FOREACH(plugins, it, plugins)
        {
            const LilvPlugin* const p = lilv_plugins_get(plugins, it);

            const std::string uri(lilv_node_as_uri(lilv_plugin_get_uri(p)));
            pluginuris.emplace_back(uri);
            pluginscache[uri] = nullptr;
            stylingcache[uri] = nullptr;

            if (char* const lilvparsed = _lilv_file_abspath(lilv_plugin_get_bundle_uri(p)))
            {
                if (const char* const bundlepath = _realpath_with_terminator(lilvparsed))
                {
                    const std::string bundlestr = bundlepath;

                    if (std::find(bundles.begin(), bundles.end(), bundlestr) == bundles.end())
                        bundles.push_back(bundlestr);
                }

                std::free(lilvparsed);
            }
        }
    }

    ~Impl()
    {
        for (const auto& pair : pluginscache)
            delete pair.second;
        for (const auto& pair : stylingcache)
            delete pair.second;

        ns.free();
        lilv_world_free(world);
    }

    // ----------------------------------------------------------------------------------------------------------------

    uint32_t getPluginCount() const noexcept
    {
        return pluginuris.size();
    }

    const std::string& getPluginURI(const uint32_t index) const noexcept
    {
        return pluginuris[index];
    }

    const Lv2Plugin* getPluginByIndex(const uint32_t index)
    {
        assert(index < getPluginCount());

        return getPluginByURI(pluginuris[index].c_str());
    }

    const Lv2Plugin* getPluginByURI(const char* const uri)
    {
        assert(uri != nullptr && *uri != '\0');

        if (pluginscache[uri] == nullptr)
        {
            std::string bundlepath;
            const LilvPlugin* plugin;

            if (LilvNode* const urinode = lilv_new_uri(world, uri))
            {
                plugin = lilv_plugins_get_by_uri(plugins, urinode);
                lilv_node_free(urinode);

                if (plugin == nullptr)
                {
                    last_error = "Invalid Plugin";
                    return nullptr;
                }

                if (char* const lilvparsed = _lilv_file_abspath(lilv_plugin_get_bundle_uri(plugin)))
                {
                    bundlepath = _realpath_with_terminator(lilvparsed);
                    std::free(lilvparsed);
                }

                if (bundlepath.empty())
                {
                    last_error = "Invalid Bundle path";
                    return nullptr;
                }
            }
            else
            {
                last_error = "Invalid URI";
                return nullptr;
            }

            Lv2Plugin* const retplugin = new Lv2Plugin;

            retplugin->uri = uri;
            retplugin->bundlepath = bundlepath;

            // --------------------------------------------------------------------------------------------------------
            // flags

            if (path_contains(bundlepath, homedir()))
                retplugin->flags |= Lv2PluginIsUserRemovable;

            if (lilv_plugin_has_extension_data(plugin, ns.modlicense_interface))
            {
                retplugin->flags |= Lv2PluginIsCommercial;

                static const std::string keysdir = _keysdir();

                std::string licensefile;
               #ifdef _DARKGLASS_DEVICE_PABLITO
                // system plugins in Anagram all share the same license URI
                // if bundle is not user removable, assume to be a system plugin
                if ((retplugin->flags & Lv2PluginIsUserRemovable) == 0)
                {
                    licensefile = keysdir + "149e897c16e874bea75961557c8fef52567ad3db";
                }
                else
               #endif
                {
                    licensefile = keysdir + _sha1(uri);
                }

                if (std::filesystem::exists(licensefile))
                    retplugin->flags |= Lv2PluginIsLicensed;
            }

            // TODO use lilv_world_ask
            if (LilvNodes* const stylingNodes = lilv_plugin_get_value(plugin, ns.dgbs_blockStyling))
            {
                retplugin->flags |= Lv2PluginHasBlockStyling;
                lilv_nodes_free(stylingNodes);
            }

#ifndef MOD_CONNECTOR_MINIMAL_LV2_WORLD
            // --------------------------------------------------------------------------------------------------------
            // name

            if (LilvNode* const node = lilv_plugin_get_name(plugin))
            {
                retplugin->name = lilv_node_as_string(node);
                lilv_node_free(node);
            }

            // --------------------------------------------------------------------------------------------------------
            // abbreviation

            if (LilvNodes* const nodes = lilv_plugin_get_value(plugin, ns.darkglass_abbreviation))
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
                        else if (std::strcmp(cat, "EnvelopePlugin") == 0)
                            retplugin->category = kLv2CategoryDynamicsEnvelope;
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
                                else if (std::strcmp(propuri, LV2_PORT_PROPS__expensive) == 0)
                                    retport.flags |= Lv2ParameterExpensive;
                                else if (std::strcmp(propuri, LV2_PORT_PROPS__logarithmic) == 0)
                                    retport.flags |= Lv2ParameterLogarithmic;
                                else if (std::strcmp(propuri, LV2_PORT_PROPS__notOnGUI) == 0)
                                    retport.flags |= Lv2ParameterHidden;
                                else if (std::strcmp(propuri, LV2_DARKGLASS_PROPERTIES__mayUpdateBlockedState) == 0)
                                    retport.flags |= Lv2ParameterMayUpdateBlockedState;
                                else if (std::strcmp(propuri, LV2_DARKGLASS_PROPERTIES__savedToPreset) == 0)
                                    retport.flags |= Lv2ParameterSavedToPreset;
                            }

                            lilv_nodes_free(nodes);
                        }

                        if (LilvNodes* const xdesignation = lilv_port_get_value(plugin, port, ns.lv2core_designation))
                        {
                            const char* const designation = lilv_node_as_string(lilv_nodes_get_first(xdesignation));

                            if (std::strcmp(designation, LV2_CORE__enabled) == 0)
                                retport.designation = kLv2DesignationEnabled;
                            else if (std::strcmp(designation, LV2_TIME__beatsPerMinute) == 0)
                                retport.designation = kLv2DesignationBPM;
                            else if (std::strcmp(designation, LV2_DARKGLASS_PROPERTIES__quickPot) == 0)
                                retport.designation = kLv2DesignationQuickPot;
                            else if (std::strcmp(designation, KXSTUDIO__Reset) == 0)
                                retport.designation = kLv2DesignationReset;

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
                                    retport.scalePoints.emplace_back(scalepoint.second);
                            }

                            lilv_scale_points_free(scalepoints);
                        }

                        if (LilvNodes* const uunits = lilv_port_get_value(plugin, port, ns.units_unit))
                        {
                            const char* uuri = lilv_node_as_uri(lilv_nodes_get_first(uunits));

                            // using pre-existing lv2 unit
                            if (uuri != nullptr)
                            {
                                if (std::strncmp(uuri, LV2_UNITS_PREFIX, std::strlen(LV2_UNITS_PREFIX)) == 0)
                                {
                                    uuri += std::strlen(LV2_UNITS_PREFIX);

                                    /**/ if (std::strcmp(uuri, "s") == 0)
                                        retport.unit = "s";
                                    else if (std::strcmp(uuri, "ms") == 0)
                                        retport.unit = "ms";
                                    else if (std::strcmp(uuri, "db") == 0)
                                        retport.unit = "dB";
                                    else if (std::strcmp(uuri, "pc") == 0)
                                        retport.unit = "%";
                                    else if (std::strcmp(uuri, "hz") == 0)
                                        retport.unit = "Hz";
                                    else if (std::strcmp(uuri, "khz") == 0)
                                        retport.unit = "kHz";
                                    else if (std::strcmp(uuri, "mhz") == 0)
                                        retport.unit = "MHz";
                                    else if (std::strcmp(uuri, "cent") == 0)
                                        retport.unit = "ct";
                                    else if (std::strcmp(uuri, "semitone12TET") == 0)
                                        retport.unit = "semi";
                                }
                                else if (std::strcmp(uuri, LV2_DARKGLASS_PROPERTIES__oneDecimalPoint) == 0)
                                {
                                    retport.unit = "1dPt";
                                }
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

                const auto get_properties = [=, &properties](const bool writable)
                {
                    if (LilvNodes* const patches = lilv_plugin_get_value(plugin, writable ? ns.patch_writable
                                                                                          : ns.patch_readable))
                    {
                        Lv2Property property;

                        // used to fetch default path value
                        LilvNode* const statenode = lilv_world_get(world,
                                                                   lilv_plugin_get_uri(plugin),
                                                                   ns.state_state,
                                                                   nullptr);

                        LILV_FOREACH(nodes, itpatches, patches)
                        {
                            const LilvNode* const patch = lilv_nodes_get(patches, itpatches);

                            property = {};
                            property.uri = lilv_node_as_uri(patch);

                            if (properties.find(property.uri) != properties.end())
                                continue;

                            // type must be lv2:Parameter
                            if (LilvNode* const typeNode = lilv_world_get(world, patch, ns.rdf_type, nullptr))
                            {
                                if (std::strcmp(lilv_node_as_uri(typeNode), LV2_CORE__Parameter) != 0)
                                {
                                    lilv_node_free(typeNode);
                                    continue;
                                }
                                lilv_node_free(typeNode);
                            }
                            else
                            {
                                continue;
                            }

                            // must contain label
                            if (LilvNode* const labelNode = lilv_world_get(world, patch, ns.rdfs_label, nullptr))
                            {
                                property.name = lilv_node_as_string(labelNode);
                                lilv_node_free(labelNode);
                            }

                            if (property.name.empty())
                                continue;

                            // must contain range and be atom type
                            if (LilvNode* const rangeNode = lilv_world_get(world, patch, ns.rdfs_range, nullptr))
                            {
                                const char* range = lilv_node_as_string(rangeNode);
                                if (std::strncmp(range, LV2_ATOM_PREFIX, std::strlen(LV2_ATOM_PREFIX)) == 0)
                                {
                                    range += std::strlen(LV2_ATOM_PREFIX);

                                    /**/ if (std::strcmp(range, "Bool") == 0)
                                        property.flags = Lv2PropertyIsParameter|Lv2ParameterInteger|Lv2ParameterToggled;
                                    else if (std::strcmp(range, "Int") == 0)
                                        property.flags = Lv2PropertyIsParameter|Lv2ParameterInteger;
                                    else if (std::strcmp(range, "Float") == 0)
                                        property.flags = Lv2PropertyIsParameter;
                                    else if (std::strcmp(range, "Path") == 0)
                                        property.flags = Lv2PropertyIsPath;
                                }

                                lilv_node_free(rangeNode);
                            }

                            if (property.flags == 0)
                                continue;

                            // set default value
                            if (property.flags == Lv2PropertyIsPath)
                            {
                                if (LilvNode* const keynode = lilv_new_uri(world, property.uri.c_str()))
                                {
                                    if (LilvNode* const valuenode = lilv_world_get(world, statenode, keynode, nullptr))
                                    {
                                        if (char* const path = _lilv_file_abspath(valuenode))
                                        {
                                            property.defpath = path;
                                            std::free(path);
                                        }

                                        lilv_node_free(valuenode);
                                    }

                                    lilv_node_free(keynode);
                                }
                            }
                            else
                            {
                                LilvNode* const xminimum = lilv_world_get(world, patch, ns.lv2core_minimum, nullptr);
                                LilvNode* const xmaximum = lilv_world_get(world, patch, ns.lv2core_maximum, nullptr);
                                LilvNode* const xdefault = lilv_world_get(world, patch, ns.lv2core_default, nullptr);

                                if (xminimum != nullptr && xmaximum != nullptr)
                                {
                                    property.min = lilv_node_as_float(xminimum);
                                    property.max = lilv_node_as_float(xmaximum);
                                }
                                else
                                {
                                    property.min = 0.f;
                                    property.max = 1.f;
                                }

                                if (xdefault != nullptr)
                                    property.def = std::min(property.max,
                                                            std::max(property.min,
                                                                     lilv_node_as_float(xdefault)));
                                else
                                    property.def = property.min;

                                lilv_node_free(xminimum);
                                lilv_node_free(xmaximum);
                                lilv_node_free(xdefault);
                            }

                            // set shortname (optional)
                            if (LilvNode* const node = lilv_world_get(world, patch, ns.lv2core_shortName, nullptr))
                            {
                                property.shortname = lilv_node_as_string(node);
                                lilv_node_free(node);
                            }

                            if (! writable)
                                property.flags |= Lv2PropertyIsReadOnly;
                        }

                        properties[property.uri] = property;

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
                        retplugin->properties.emplace_back(prop.second);
                }
            }

            // --------------------------------------------------------------------------------------------------------
            // block images

            {
                const auto assignResourcePath = [&](std::string& resourcePathRef,
                                                    const LilvNode* const resource)
                {
                    if (LilvNodes* const nodes = lilv_plugin_get_value(plugin, resource))
                    {
                        const LilvNode* const node = lilv_nodes_get_first(nodes);
                        if (lilv_node_is_uri(node))
                        {
                            if (char* const path = _lilv_file_abspath(node))
                            {
                                if (path_contains(path, bundlepath))
                                    resourcePathRef = path;
                                std::free(path);
                            }
                        }
                        lilv_nodes_free(nodes);
                    }
                };

                assignResourcePath(retplugin->blockImageOff, ns.darkglass_blockImageOff);
                assignResourcePath(retplugin->blockImageOn, ns.darkglass_blockImageOn);
            }

            // --------------------------------------------------------------------------------------------------------
#endif

            pluginscache[uri] = retplugin;
            return retplugin;
        }

        return pluginscache[uri];
    }

#ifndef MOD_CONNECTOR_MINIMAL_LV2_WORLD
    const CustomStyling::Block* getPluginCustomStyling(const char* const uri)
    {
        assert(uri != nullptr);
        assert(*uri != '\0');

        const Lv2Plugin* const retplugin = pluginscache[uri];
        assert_return(retplugin != nullptr, nullptr);
        assert_return(retplugin->flags & Lv2PluginHasBlockStyling, nullptr);

        if (stylingcache[uri] != nullptr)
            return stylingcache[uri];

        LilvNode* stylingNode = nullptr;
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

            LilvNodes* const stylingNodes = lilv_plugin_get_value(plugin, ns.dgbs_blockStyling);
            if (stylingNodes == nullptr)
            {
                last_error = "Plugin does not contain block styling";
                return nullptr;
            }

            stylingNode = lilv_node_duplicate(lilv_nodes_get_first(stylingNodes));

            lilv_nodes_free(stylingNodes);
        }

        const auto assignFont = [&](CustomStyling::Font& fontRef,
                                    const LilvNode* const subject,
                                    const LilvNode* const predicate)
        {
            LilvNode* const fontNode = lilv_world_get(world, subject, predicate, nullptr);
            if (fontNode == nullptr)
                return;

            LilvNode* const fontPathNode = lilv_world_get(world, fontNode, ns.dgbs_path, nullptr);
            if (fontPathNode == nullptr)
            {
                lilv_node_free(fontNode);
                return;
            }

            char* const path = _lilv_file_abspath(fontPathNode);
            if (path == nullptr)
            {
                lilv_node_free(fontPathNode);
                lilv_node_free(fontNode);
                return;
            }

            if (path_contains(path, retplugin->bundlepath))
            {
                if (LilvNode* const fontSizeNode = lilv_world_get(world, fontNode, ns.dgbs_size, nullptr))
                {
                    if (lilv_node_is_int(fontSizeNode))
                    {
                        fontRef.path = path;
                        fontRef.size = lilv_node_as_int(fontSizeNode);
                    }
                    lilv_node_free(fontSizeNode);
                }
            }

            std::free(path);
            lilv_node_free(fontPathNode);
            lilv_node_free(fontNode);
        };

        const auto assignImage = [&](
            CustomStyling::Image& imageRef,
            const LilvNode* const subject,
            const LilvNode* const predicate,
            const CustomStyling::Alignment alignmentDefault = CustomStyling::kAlignCenter)
        {
            assert(alignmentDefault != CustomStyling::kAlignNone);

            LilvNode* const imageNode = lilv_world_get(world, subject, predicate, nullptr);
            if (imageNode == nullptr)
                return;

            LilvNode* const imagePathNode = lilv_world_get(world, imageNode, ns.dgbs_path, nullptr);
            if (imagePathNode == nullptr)
            {
                lilv_node_free(imageNode);
                return;
            }

            char* const path = _lilv_file_abspath(imagePathNode);
            if (path == nullptr)
            {
                lilv_node_free(imagePathNode);
                lilv_node_free(imageNode);
                return;
            }

            if (path_contains(path, retplugin->bundlepath))
            {
                imageRef.alignment = alignmentDefault;
                imageRef.path = path;

                if (LilvNode* const alignmentNode = lilv_world_get(world, imageNode, ns.dgbs_alignment, nullptr))
                {
                    const char* alignmentURI = lilv_node_is_uri(alignmentNode)
                                                ? lilv_node_as_uri(alignmentNode)
                                                : nullptr;

                    if (alignmentURI != nullptr && std::strstr(alignmentURI, LV2_DARKGLASS_BLOCK_STYLING_PREFIX) != nullptr)
                    {
                        alignmentURI += std::strlen(LV2_DARKGLASS_BLOCK_STYLING_PREFIX);

                        /**/ if (std::strcmp(alignmentURI, "alignBottomLeft") == 0)
                            imageRef.alignment = CustomStyling::kAlignBottomLeft;
                        else if (std::strcmp(alignmentURI, "alignBottomMid") == 0)
                            imageRef.alignment = CustomStyling::kAlignBottomMid;
                        else if (std::strcmp(alignmentURI, "alignBottomRight") == 0)
                            imageRef.alignment = CustomStyling::kAlignBottomRight;
                        else if (std::strcmp(alignmentURI, "alignCenter") == 0)
                            imageRef.alignment = CustomStyling::kAlignCenter;
                        else if (std::strcmp(alignmentURI, "alignLeftMid") == 0)
                            imageRef.alignment = CustomStyling::kAlignLeftMid;
                        else if (std::strcmp(alignmentURI, "alignRightMid") == 0)
                            imageRef.alignment = CustomStyling::kAlignRightMid;
                        else if (std::strcmp(alignmentURI, "alignTopLeft") == 0)
                            imageRef.alignment = CustomStyling::kAlignTopLeft;
                        else if (std::strcmp(alignmentURI, "alignTopMid") == 0)
                            imageRef.alignment = CustomStyling::kAlignTopMid;
                        else if (std::strcmp(alignmentURI, "alignTopRight") == 0)
                            imageRef.alignment = CustomStyling::kAlignTopRight;
                    }
                    lilv_node_free(alignmentNode);
                }
            }

            std::free(path);
            lilv_node_free(imagePathNode);
            lilv_node_free(imageNode);
        };
        // template <class ParameterOrBypass>
        const auto assignParameter = [&](auto& paramRef, const LilvNode* const predicate)
        {
            LilvNode* const nodes = lilv_world_get(world, stylingNode, predicate, nullptr);
            if (nodes == nullptr)
                return;

            assignImage(paramRef.background, nodes, ns.dgbs_background);
            assignImage(paramRef.backgroundScenes, nodes, ns.dgbs_backgroundScenes);
            assignImage(paramRef.widget, nodes, ns.dgbs_widget);
            // TODO overlays

            // if constexpr (std::is_same(typeof(paramRef), CustomStyling::Parameter))
            {
            }

            lilv_node_free(nodes);
        };

        CustomStyling::Block* const styling = new CustomStyling::Block;

        if (LilvNode* const paddingNode = lilv_world_get(world, stylingNode, ns.dgbs_parameterStartPadding, nullptr))
        {
            if (lilv_node_is_int(paddingNode))
                styling->parameterStartPadding = std::clamp(lilv_node_as_int(paddingNode), 0, CustomStyling::kMaxParameterStartPadding);

            lilv_node_free(paddingNode);
        }

        assignImage(styling->background, stylingNode, ns.dgbs_background, CustomStyling::kAlignBottomRight);
        assignImage(styling->blockName.image, stylingNode, ns.dgbs_blockName, CustomStyling::kAlignTopLeft);
        assignFont(styling->blockName.font, stylingNode, ns.dgbs_blockName);
        assignImage(styling->paginationDots, stylingNode, ns.dgbs_paginationDots, CustomStyling::kAlignBottomMid);
        // TODO topbarButtons
        assignParameter(styling->bypass, ns.dgbs_bypass);
        assignParameter(styling->defaultWidgets.circle, ns.dgbs_circle);
        assignParameter(styling->defaultWidgets.fader, ns.dgbs_fader);
        assignParameter(styling->defaultWidgets.list, ns.dgbs_list);
        assignParameter(styling->defaultWidgets.toggle, ns.dgbs_toggle);
        // TODO customWidgets

        lilv_node_free(stylingNode);

        stylingcache[uri] = styling;
        return styling;
    }

    const Lv2Port& getPluginPort(const char* const uri, const char* const symbol)
    {
        assert(uri != nullptr && *uri != '\0');
        assert(symbol != nullptr && *symbol != '\0');

        if (const Lv2Plugin* const plugin = getPluginByURI(uri))
        {
            for (const Lv2Port& port : plugin->ports)
            {
                if (port.symbol == symbol)
                    return port;
            }
        }

        static const Lv2Port fallback;
        return fallback;
    }

    bool isPluginAvailable(const char* const uri) const
    {
        assert(uri != nullptr && *uri != '\0');

        return pluginscache.find(uri) != pluginscache.end();
    }

    std::unordered_map<std::string, float> loadPluginState(const char* const path)
    {
        assert(path != nullptr && *path != '\0');

        LV2_URID_Map uridMap = { this, _mapfn };
        LilvState* const state = lilv_state_new_from_file(world, &uridMap, nullptr, path);
        assert_return(state != nullptr, {});

        std::unordered_map<std::string, float> values;
        lilv_state_emit_port_values(state, _portfn, &values);
        lilv_state_free(state);

        return values;
    }
#endif

    bool bundleAdd(const char* const path, std::vector<std::string>* pluginsInBundlePtr = nullptr)
    {
        assert(path != nullptr && *path != '\0');
        assert(path[std::strlen(path) - 1] == PATH_SEP_CHAR);

        // stop now if bundle is already loaded
        if (std::find(bundles.begin(), bundles.end(), path) != bundles.end())
            return false;

        // query plugins in bundle
        std::vector<std::string> pluginsInBundleLocal;
        std::vector<std::string>& pluginsInBundle = pluginsInBundlePtr != nullptr
                                                  ? *pluginsInBundlePtr
                                                  : pluginsInBundleLocal;
        _pluginsInBundle(pluginsInBundle, path);
        assert_return(! pluginsInBundle.empty(), false);

        // load the bundle
        if (LilvNode* const b = lilv_new_file_uri(world, nullptr, path))
        {
            lilv_world_load_bundle(world, b);
            lilv_node_free(b);
        }

        // add to loaded list
        bundles.emplace_back(path);

        // refresh cache
        plugins = lilv_world_get_all_plugins(world);
        pluginuris.reserve(lilv_plugins_size(plugins));

        for (const std::string& uri : pluginsInBundle)
        {
            assert(pluginscache.find(uri) == pluginscache.end());

            pluginuris.emplace_back(uri);
            pluginscache[uri] = nullptr;
            stylingcache[uri] = nullptr;
        }

        return true;
    }

    bool bundleRemove(const char* const path, std::vector<std::string>* pluginsInBundlePtr = nullptr)
    {
        assert(path != nullptr && *path != '\0');
        assert(path[std::strlen(path) - 1] == PATH_SEP_CHAR);

        // stop now if bundle is not loaded
        if (std::find(bundles.begin(), bundles.end(), path) == bundles.end())
            return false;

        // query plugins in bundle
        std::vector<std::string> pluginsInBundleLocal;
        std::vector<std::string>& pluginsInBundle = pluginsInBundlePtr != nullptr
                                                  ? *pluginsInBundlePtr
                                                  : pluginsInBundleLocal;
        _pluginsInBundle(pluginsInBundle, path);
        assert_return(! pluginsInBundle.empty(), false);

        // unload the bundle
        if (LilvNode* const b = lilv_new_file_uri(world, nullptr, path))
        {
            lilv_world_unload_bundle(world, b);
            lilv_node_free(b);
        }

        // remove from loaded list
        bundles.remove(path);

        // refresh cache
        plugins = lilv_world_get_all_plugins(world);

        for (const std::string& uri : pluginsInBundle)
        {
            const std::vector<std::string>::const_iterator it = std::find(pluginuris.cbegin(), pluginuris.cend(), uri);
            assert_continue(it != pluginuris.cend());

            pluginuris.erase(it);

            const std::unordered_map<std::string, const Lv2Plugin*>::const_iterator it2 = pluginscache.find(uri);
            assert_continue(it2 != pluginscache.cend());

            delete it2->second;
            pluginscache.erase(it2);

            const std::unordered_map<std::string, const CustomStyling::Block*>::const_iterator it3 = stylingcache.find(uri);
            assert_continue(it3 != stylingcache.cend());

            delete it3->second;
            stylingcache.erase(uri);
        }

        return true;
    }

    static bool getPluginsInBundle(const char* const path, std::vector<std::string>& pluginsInBundle)
    {
        assert(path != nullptr && *path != '\0');
        assert(path[std::strlen(path) - 1] == PATH_SEP_CHAR);

        _pluginsInBundle(pluginsInBundle, path);

        return ! pluginsInBundle.empty();
    }

private:
    std::string& last_error;

    LilvWorld* const world = nullptr;
    const LilvPlugins* plugins = nullptr;

    Lv2NamespaceDefinitions ns;
    std::list<std::string> bundles;
    std::vector<std::string> pluginuris;
    std::unordered_map<std::string, const Lv2Plugin*> pluginscache;
    std::unordered_map<std::string, const CustomStyling::Block*> stylingcache;

    static LV2_URID _mapfn(LV2_URID_Map_Handle handle, const char* uri);
    static void _portfn(const char* symbol, void* userData, const void* value, uint32_t size, uint32_t type);
    static void _pluginsInBundle(std::vector<std::string>& pluginsInBundle, const char* bundlepath);
};

// --------------------------------------------------------------------------------------------------------------------

Lv2World::Lv2World() : impl(new Impl(last_error)) {}
Lv2World::~Lv2World() { delete impl; }

// --------------------------------------------------------------------------------------------------------------------

enum {
    k_urid_null,
    k_urid_atom_bool,
    k_urid_atom_int,
    k_urid_atom_long,
    k_urid_atom_float,
    k_urid_atom_double,
};

LV2_URID Lv2World::Impl::_mapfn(LV2_URID_Map_Handle, const char* const uri)
{
    assert_return(uri != nullptr && uri[0] != '\0', k_urid_null);

    static std::vector<std::string> mapping = {
        LV2_ATOM__Bool,
        LV2_ATOM__Int,
        LV2_ATOM__Long,
        LV2_ATOM__Float,
        LV2_ATOM__Double,
    };

    LV2_URID urid = 1;
    for (const std::string& uri2 : mapping)
    {
        if (uri2 == uri)
            return urid;
        ++urid;
    }

    mapping.emplace_back(uri);
    return urid;
}

void Lv2World::Impl::_portfn(const char* const symbol,
                             void* const userData,
                             const void* const value,
                             const uint32_t size,
                             const uint32_t type)
{
    std::unordered_map<std::string, float>& values = *static_cast<std::unordered_map<std::string, float>*>(userData);

    switch (type)
    {
    case k_urid_atom_bool:
    case k_urid_atom_int:
        if (size == sizeof(int32_t))
        {
            const int32_t svalue = *(const int32_t*)value;
            values[symbol] = static_cast<float>(svalue);
            return;
        }
        break;

    case k_urid_atom_long:
        if (size == sizeof(int64_t))
        {
            const int64_t svalue = *(const int64_t*)value;
            values[symbol] = static_cast<float>(svalue);
            return;
        }
        break;

    case k_urid_atom_float:
        if (size == sizeof(float))
        {
            const float svalue = *(const float*)value;
            values[symbol] = svalue;
            return;
        }
        break;

    case k_urid_atom_double:
        if (size == sizeof(double))
        {
            const double svalue = *(const double*)value;
            values[symbol] = static_cast<float>(svalue);
            return;
        }
        break;
    }

    mod_log_warn("Lv2World::_portfn(%s, %p, %p, %u, %u): called with unknown type\n",
                 symbol, userData, value, size, size);
}

void Lv2World::Impl::_pluginsInBundle(std::vector<std::string>& pluginsInBundle, const char* const bundlepath)
{
    if (LilvWorld* const w = lilv_world_new())
    {
        if (LilvNode* const b = lilv_new_file_uri(w, nullptr, bundlepath))
        {
            lilv_world_load_bundle(w, b);
            lilv_node_free(b);
        }

        const LilvPlugins* const wplugins = lilv_world_get_all_plugins(w);

        LILV_FOREACH(plugins, iter, wplugins)
        {
            const LilvPlugin* const p = lilv_plugins_get(wplugins, iter);

            pluginsInBundle.emplace_back(lilv_node_as_uri(lilv_plugin_get_uri(p)));
        }

        lilv_world_free(w);
    }
}

// --------------------------------------------------------------------------------------------------------------------

uint32_t Lv2World::getPluginCount() const noexcept
{
    return impl->getPluginCount();
}

const std::string& Lv2World::getPluginURI(const uint32_t index) const
{
    return impl->getPluginURI(index);
}

const Lv2Plugin* Lv2World::getPluginByIndex(const uint32_t index) const
{
    return impl->getPluginByIndex(index);
}

const Lv2Plugin* Lv2World::getPluginByURI(const char* const uri) const
{
    return impl->getPluginByURI(uri);
}

#ifndef MOD_CONNECTOR_MINIMAL_LV2_WORLD
const CustomStyling::Block* Lv2World::getPluginCustomStyling(const char* uri) const
{
    return impl->getPluginCustomStyling(uri);
}

const Lv2Port& Lv2World::getPluginPort(const char* const uri, const char* const symbol) const
{
    return impl->getPluginPort(uri, symbol);
}

bool Lv2World::isPluginAvailable(const char* const uri) const
{
    return impl->isPluginAvailable(uri);
}

std::unordered_map<std::string, float> Lv2World::loadPluginState(const char* const path) const
{
    return impl->loadPluginState(path);
}
#endif

bool Lv2World::bundleAdd(const char* const path, std::vector<std::string>* pluginsInBundle)
{
    return impl->bundleAdd(path, pluginsInBundle);
}

bool Lv2World::bundleRemove(const char* const path, std::vector<std::string>* pluginsInBundle)
{
    return impl->bundleRemove(path, pluginsInBundle);
}

bool Lv2World::getPluginsInBundle(const char* const path, std::vector<std::string>& pluginsInBundle)
{
    return Impl::getPluginsInBundle(path, pluginsInBundle);
}

// --------------------------------------------------------------------------------------------------------------------
