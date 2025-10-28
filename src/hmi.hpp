// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "config.h"
#include "mod-protocol.h"

#include <array>
#include <string>

struct HMI;

// --------------------------------------------------------------------------------------------------------------------

struct HMIProto
{
    /**
     * Callback used for receiving messages, triggered via poll().
     */
    struct Callback {
        struct Data {
            enum {
                // web-ui is connected
                kConnected,
                // web-ui is disconnected
                kDisconnected,
                // assign a new hw control
                kControlAdd,
                // unassign a hw control
                kControlRemove,
                // sends assigned control data
                kControlSet,
                // host initial state
                kInitialState,
                // set the name of the current pedalboard on the device display
                kPedalboardNameSet,
                // clear all pedalboard related items
                kPedalboardClear,
            } type;
            union {
                // kControlAdd
                struct {
                    uint8_t hw_id;
                    const char* label;
                    const char* unit;
                    uint32_t flags;
                    float current;
                    float min;
                    float max;
                    int steps;
                } controlAdd;
                // kControlRemove
                struct {
                    uint8_t hw_id;
                } controlRemove;
                // kControlSet
                struct {
                    uint8_t hw_id;
                    float value;
                } controlSet;
                // kInitialState
                struct {
                    uint32_t numPedalboards;
                    uint32_t paginationStart;
                    uint32_t paginationEnd;
                    uint32_t bankId;
                    uint32_t pedalboardId;
                    // TODO pedalboard list
                } initialState;
                // kPedalboardNameSet
                struct {
                    const char* name;
                } pedalboardNameSet;
            };
        };

        /** destructor */
        virtual ~Callback() = default;

        /**
         * HMI message received.
         */
        virtual void hmiCallback(const Data &data) = 0;
    };

    /**
     * string describing the last error, in case any operation fails.
     * will also be set during initialization in case of HMI connection failure.
     */
    std::string last_error;

    // sends assigned control data
    bool control_set(uint8_t hw_id, float value);

    // sends back a control_add command with new control page data
    bool control_page(uint8_t hw_id, uint32_t prop_bitmask, uint8_t page_index_id);

    // returns a new page of banks
    bool banks(int8_t direction, uint32_t current_banks_hover_id);

    // creates a new bank
    bool bank_new(const char *bank_name);

    // deletes indicated bank
    bool bank_delete(uint32_t bank_id);

    // add the pedalboard uids to the bank
    // add all the pbs from a bank if bank_id_pbs_originate_from == -1, in that case bank uids are passed
    bool add_pbs_to_bank(uint32_t bank_id_to_add_to, int32_t bank_id_pbs_originate_from, const char *pb_uids, ...);

    // reorder the pedalboard in a bank
    bool reorder_pbs_in_bank(uint32_t bank_uid, uint32_t pb_to_move_uid, uint32_t index_to_move_to);

    // request a new page of pedalboards
    bool pedalboards(bool up_page, uint32_t current_page_index, uint32_t bank_uid);

    // resets the pedalboard to the last saved state
    bool pedalboard_reset();

    // saves the pedalboard in the current state
    bool pedalboard_save();

    // save current setup as new pedalboard
    // if the name already excists, return -1
    bool pedalboard_save_as(const char *name);

    // deletes indicated pedalboard
    bool pedalboard_delete(uint32_t bank_id, uint32_t pb_id);

    // reorder the snapshot within a pedalboard
    bool reorder_sss_in_pb(uint32_t snapshot_to_move_uid, uint32_t index_to_move_to);

    // returns a new page of pedalboards
    bool snapshots(bool up_page, uint32_t current_page_index);

    // loads the requested snapshot
    bool snapshots_load(uint32_t snapshot_uid);

    // saves the currently active snapshot
    bool snapshots_save();

    // save the current control config as a snapshot
    // if the name already excists, return -1
    bool snapshot_save_as(const char *name);

    // deletes indicated snapshot
    bool snapshot_delete(uint32_t snapshot_uid);

    // turn on the tuner
    bool tuner_on();

    // turn off the tuner
    bool tuner_off();

    // changes the tuner input source
    bool tuner_input(uint8_t input);

    HMIProto(const char *serial, int baudrate);
    ~HMIProto();

   /**
     * class to activate non-blocking mode during a function scope.
     * this allows to send a bunch of related messages in quick succession,
     * while only waiting once (in the class destructor).
     */
    class NonBlockingScope {
        HMIProto& hmi;
    public:
        NonBlockingScope(HMIProto& hmi);
        ~NonBlockingScope();
    };

private:
    // private details
    struct Impl;
    Impl *const impl;

    bool _poll(Callback* callback);
    friend struct HMI;
};

// --------------------------------------------------------------------------------------------------------------------

struct HMI : HMIProto,
             private HMIProto::Callback
{
    using Callback = HMIProto::Callback;

    struct Actuator {
        bool assigned = false;
        std::string label;
        std::string unit;
        uint32_t flags = 0;
        float current = 0.f;
        float min = 0.f;
        float max = 1.f;
        int steps = 0;
    };

    struct ActuatorPage {
        std::array<Actuator, NUM_BINDING_ACTUATORS> actuators;
        bool active;
    };

    // publicly accessible read-only data
    const std::array<ActuatorPage, NUM_BINDING_PAGES> &actuatorPages = _actuatorPages;
    const uint8_t &page = _page;
    const uint32_t &bankId = _bankId;
    const uint32_t &pedalboardId = _pedalboardId;
    const std::string &pedalboardName = _pedalboardName;
    const bool &webConnected = _webConnected;

    // helper for fetching current actuator page
    [[nodiscard]] inline const ActuatorPage& currentActuatorPage() const
    {
        return _actuatorPages[_page];
    }

    HMI(Callback* callback, const char *serial, int baudrate);

    bool poll();

private:
    // private writable data
    std::array<ActuatorPage, NUM_BINDING_PAGES> _actuatorPages;
    uint8_t _page = 0;
    uint32_t _bankId = 0;
    uint32_t _pedalboardId = 0;
    std::string _pedalboardName;
    bool _webConnected = false;

    // callback handling for updates of accessible data
    Callback* const _callback;
    void hmiCallback(const Data &data) final;
};

// --------------------------------------------------------------------------------------------------------------------

using HMICallbackData = HMIProto::Callback::Data;
