// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

/**
 * TODO document me
 */
struct Host {
   /**
    * string describing the last error, in case any operation fails.
    * will also be set during initialization in case of mod-host connection failure.
    */
    std::string last_error;

   /* add an LV2 plugin encapsulated as a jack client
    * @a instance_number must be any value between 0 ~ 9990, inclusively
    */
    bool add(const char* uri, int16_t instance_number);
 
   /**
    * remove an LV2 plugin instance (and also the jack client)
    */
    bool remove(int16_t instance_number);

   /**
    * load a preset state of an effect instance
    */
    bool preset_load(int16_t instance_number, const char* preset_uri);

   /**
    * save a preset state of an effect instance
    */
    bool preset_save(int16_t instance_number, const char* preset_name, const char* dir, const char* file_name);

   /**
    * show the preset information of requested URI
    */
    std::string preset_show(const char* preset_uri);

   /**
    * connect two jack ports
    */
    bool connect(const char* origin_port, const char* destination_port);

   /**
    * disconnect two jack ports
    */
    bool disconnect(const char* origin_port, const char* destination_port);

   /**
    * toggle effect processing
    */
    bool bypass(int16_t instance_number, bool bypass_value);

   /**
    * set the value of a control port
    */
    bool param_set(int16_t instance_number, const char* param_symbol, float param_value);

   /**
    * get the value of a control port
    */
    bool param_get(int16_t instance_number, const char* param_symbol);

   /**
    * monitor a control port according to a condition
    */
    bool param_monitor(int16_t instance_number, const char* param_symbol, const char* cond_op, float value);

   /**
    * set the value of a patch property
    */
    bool patch_set(int16_t instance_number, const char* property_uri, const char* value);

   /**
    * get the value of a patch property
    */
    // TODO proper return type
    bool patch_get(int16_t instance_number, const char* property_uri);

   /**
    * get the licensee name for a commercial plugin
    */
    bool licensee(int16_t instance_number);

   /**
    * set the global beats per minute transport value
    */
    bool set_bpm(double beats_per_minute);

   /**
    * set the global beats per bar transport value
    */
    bool set_bpb(double beats_per_bar);

   /**
    * open a socket port for monitoring parameter changes
    */
    bool monitor(const char* addr, int port, bool status);

   /**
    * request monitoring of an output control port (in the feedback port)
    */
    bool monitor_output(int16_t instance_number, const char* param_symbol);

   /**
    * start MIDI learn for a control port
    */
    bool midi_learn(int16_t instance_number, const char* param_symbol, float minimum, float maximum);

   /**
    * map a MIDI controller to a control port
    */
    bool midi_map(int16_t instance_number, const char* param_symbol, uint8_t midi_channel, uint8_t midi_cc, float minimum, float maximum);

   /**
    * unmap the MIDI controller from a control port
    */
    bool midi_unmap(int16_t instance_number, const char* param_symbol);

#if 0
#define MONITOR_MIDI_PROGRAM "monitor_midi_program %i %i"
   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    set_midi_program_change_pedalboard_bank_channel <enable> <midi_channel>
        * set the MIDI channel which changes pedalboard banks on MIDI program change. <midi_channel> is in the range of [0,15].
        e.g.: set_midi_program_change_pedalboard_bank_channel 1 5 to enable listening for bank changes on channel 6

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    set_midi_program_change_pedalboard_snapshot_channel <enable> <midi_channel>
        * set the MIDI channel which changes pedalboard snapshots on MIDI program change. <midi_channel> is in the range of [0,15].
        e.g.: set_midi_program_change_pedalboard_snapshot_channel 1 4 to enable listening for preset changes on channel 5
#endif

#if 0
#define CC_MAP               "cc_map %i %s %i %i %s %f %f %f %i %i %s %i ..."
#define CC_VALUE_SET         "cc_value_set %i %s %f"
#define CC_UNMAP             "cc_unmap %i %s"
   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    cc_map <instance_number> <param_symbol> <device_id> <actuator_id> <label> <value> <minimum> <maximum> <steps> <unit> <scalepoints_count> <scalepoints...>
        * map a Control Chain actuator to a parameter
        e.g.: cc_map 0 gain 0 1 "Gain" 0.0 -24.0 3.0 33 "dB" 0

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    cc_unmap <instance_number> <param_symbol>
        * unmap the Control Chain actuator from a parameter
        e.g.: unmap 0 gain

#define CV_MAP               "cv_map %i %s %s %f %f %s"
#define CV_UNMAP             "cv_unmap %i %s"

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    cv_map <instance_number> <param_symbol> <source_port_name> <minimum> <maximum> <operational-mode>
        * map a CV source port to a parameter, operational-mode being one of '-', '+', 'b' or '='
        e.g.: cv_map 0 gain "AMS CV Source:CV Out 1" -24.0 3.0 =

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    cv_unmap <instance_number> <param_symbol>
        * unmap the CV source port actuator from a parameter
        e.g.: cv_unmap 0 gain

#define HMI_MAP              "hmi_map %i %s %i %i %i %i %i %s %f %f %i"
#define HMI_UNMAP            "hmi_unmap %i %s"
#endif

   /**
    * return current jack cpu load
    */
    float cpu_load();

#if 0
#define LOAD_COMMANDS        "load %s"
#define SAVE_COMMANDS        "save %s"

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    load <file_name>
        * load a history command file
        * dummy way to save/load workspace state
        e.g.: load my_setup

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    save <file_name>
        * saves the history of typed commands
        * dummy way to save/load workspace state
        e.g.: save my_setup

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    bundle_add <bundle_path>
        * add a bundle to the running lv2 world
        e.g.: bundle_add /path/to/bundle.lv2

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    bundle_remove <bundle_path>
        * remove a bundle from the running lv2 world
        e.g.: bundle_remove /path/to/bundle.lv2

#define BUNDLE_ADD           "bundle_add %s"
#define BUNDLE_REMOVE        "bundle_remove %s %s"
#define STATE_LOAD           "state_load %s"
#define STATE_SAVE           "state_save %s"
#define STATE_TMPDIR         "state_tmpdir %s"
#define FEATURE_ENABLE       "feature_enable %s %i"

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    feature_enable <feature> <enable>
        * enable or disable a feature
        e.g.: feature_enable link 1
        current features are "link", "processing" and "midi_clock_slave"

   /**
    * xxxxxx
    */
    bool xxxxxx(int16_t instance_number, const char* preset_uri);

    transport <rolling> <beats_per_bar> <beats_per_minute>
        * change the current transport state
        e.g.: transport 1 4 120

#define TRANSPORT            "transport %i %f %f"
#define TRANSPORT_SYNC       "transport_sync %s"
#endif

    Host();
    ~Host();

private:
    struct Impl;
    Impl* const impl;
};
