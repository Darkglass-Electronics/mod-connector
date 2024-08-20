// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <cstdint>
#include <string>

struct cc_scalepoint {
    const char* label;
    float value;
};

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
     * when instance_number is -1 all plugins will be removed
     */
    bool remove(int16_t instance_number);

    /**
     * toggle effect activated state
     */
    bool activate(int16_t instance_number, int16_t instance_number_end, bool activate_value);

    /* add an LV2 plugin encapsulated as a jack client, in deactivated state
     * @a instance_number must be any value between 0 ~ 9990, inclusively
     */
    bool preload(const char* uri, int16_t instance_number);

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
     * open a socket port for monitoring parameter changes
     */
    bool monitor(const char* addr, int port, bool status);

    /**
     * request monitoring of an output control port (on the feedback port)
     */
    bool monitor_output(int16_t instance_number, const char* param_symbol);

    /**
     * start MIDI learn for a control port
     */
    bool midi_learn(int16_t instance_number, const char* param_symbol, float minimum, float maximum);

    /**
     * map a MIDI controller to a control port
     * a non-standard @a midi_cc value of 131 (0x83) is used for pitchbend
     */
    bool midi_map(int16_t instance_number, const char* param_symbol, uint8_t midi_channel, uint8_t midi_cc, float minimum, float maximum);

    /**
     * unmap the MIDI controller from a control port
     */
    bool midi_unmap(int16_t instance_number, const char* param_symbol);

    /**
     * listen to MIDI program change messages (on the feedback port)
     */
    bool monitor_midi_program(uint8_t midi_channel, bool enable);

    /**
     * map a Control Chain actuator to a control port
     */
    bool cc_map(int16_t instance_number, const char* param_symbol,
                int device_id, int actuator_id, const char* label,
                float value, float minimum, float maximum, int steps, const char* unit,
                unsigned int scalepoints_count, const cc_scalepoint* scalepoints);

    /**
     * unmap the Control Chain actuator from a control port
     */
    bool cc_unmap(int16_t instance_number, const char* param_symbol);

    /**
     * set the value of a mapped Control Chain actuator
     */
    bool cc_value_set(int16_t instance_number, const char* param_symbol, float value);

    /**
     * map a CV source port to a control port, operational-mode being one of '-', '+', 'b' or '='
     */
    bool cv_map(int16_t instance_number, const char* param_symbol, const char* source_port_name, float minimum, float maximum, char operational_mode);

    /**
     * unmap the CV source port actuator from a control port
     */
    bool cv_unmap(int16_t instance_number, const char* param_symbol);

    /**
     * report an HMI assignment to an effect instance, using the MOD Audio's HMI LV2 extension
     * @see https://github.com/moddevices/mod-lv2-extensions/blob/main/mod-hmi.lv2/mod-hmi.h
     */
    bool hmi_map(int16_t instance_number, const char* param_symbol,
                 int hw_id, int page, int subpage,
                 int caps, int flags, const char* label, float minimum, float maximum, int steps);

    /**
     * report an HMI unassignment to an effect instance
     */
    bool hmi_unmap(int16_t instance_number, const char* param_symbol);

    /**
     * return current jack cpu load
     */
    float cpu_load();

    /**
     * load a history command file
     * dummy way to save/load workspace state
     */
    bool load(const char* file_name);

    /**
     * saves the history of typed commands
     * dummy way to save/load workspace state
     */
    bool save(const char* file_name);

    /**
     * add a bundle to the running lv2 world
     */
    bool bundle_add(const char* bundle_path);

    /**
     * remove a bundle from the running lv2 world
     */
    bool bundle_remove(const char* bundle_path, const char* resource = nullptr);

    /**
     * load a custom effect state from a directory
     * (everything that is not from a control port, like files)
     */
    bool state_load(const char* dir);

    /**
     * save a custom effect state into a directory
     * (everything that is not from a control port, like files)
     */
    bool state_save(const char* dir);

    /**
     * set the temporary directory to use when effects request a directory to save state into
     */
    bool state_tmpdir(const char* dir);

   /**
     * enable or disable a feature
     * feature can be one of "aggregated-midi", "freewheeling" or "processing"
     * the "aggregated-midi" feature requires the use of jack2 and mod-midi-merger to be installed system-wide
     */
    bool feature_enable(const char* feature, bool enable);

    /**
     * set the global beats per minute transport value
     */
    bool set_bpm(double beats_per_minute);

    /**
     * set the global beats per bar transport value
     */
    bool set_bpb(double beats_per_bar);

   /**
     * change the global transport state
     */
    bool transport(bool rolling, double beats_per_bar, double beats_per_minute);

   /**
     * change the transport sync mode
     * @a mode can be one of "none", "link" or "midi"
     */
    bool transport_sync(const char* mode);

   /**
     * report feedback port ready for more messages
     */
    bool output_data_ready();

    bool poll_feedback();

    Host();
    ~Host();

   /**
     * class to activate non-blocking mode during a function scope.
     * this allows to send a bunch of related messages in quick succession,
     * while only waiting once (in the class destructor).
     */
    class NonBlockingScope {
        Host& host;
    public:
        NonBlockingScope(Host& host);
        ~NonBlockingScope();
    };

private:
    struct Impl;
    Impl* const impl;
};
