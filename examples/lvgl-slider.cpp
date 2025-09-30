// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"
#include "utils.hpp"
#include "lvgl.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// config for this example

static constexpr const char kPluginEgAmp[] = "http://lv2plug.in/plugins/eg-amp";
static constexpr const char kPluginEgAmpControl[] = "gain";
static constexpr const int16_t kPluginEgAmpId = 0;

static constexpr const char kPluginEgParams[] = "http://lv2plug.in/plugins/eg-params";
static constexpr const char kPluginEgParamsPatch[] = "http://lv2plug.in/plugins/eg-params#float";
static constexpr const int16_t kPluginEgParamsId = 1;

// --------------------------------------------------------------------------------------------------------------------
// OS details for safely closing down

static bool running = true;

#ifdef _WIN32
static BOOL WINAPI signal_cb(const DWORD dwCtrlType) noexcept
{
    if (dwCtrlType == CTRL_C_EVENT)
    {
        running = false;
        return TRUE;
    }
    return FALSE;
}
#else
static void signal_cb(const int sig) noexcept
{
    switch (sig)
    {
    case SIGINT:
    case SIGTERM:
        running = false;
        break;
    }
}
#endif

// --------------------------------------------------------------------------------------------------------------------
// host application details

struct HostApp : Host::FeedbackCallback {
    Host host;
    bool ok = host.last_error.empty();
    uint32_t lastHostUpdate = 0;

    void hostFeedbackCallback(const HostFeedbackData& data) override
    {
        switch (data.type)
        {
        case HostFeedbackData::kFeedbackParameterSet:
            fprintf(stdout,
                    "HostFeedbackData::kFeedbackParameterSet %d %s %f\n",
                    data.paramSet.effect_id,
                    data.paramSet.symbol,
                    data.paramSet.value);
            break;

        case HostFeedbackData::kFeedbackOutputMonitor:
            fprintf(stdout,
                    "HostFeedbackData::kFeedbackParameterSet %d %s %f\n",
                    data.paramSet.effect_id,
                    data.paramSet.symbol,
                    data.paramSet.value);
            break;

        case HostFeedbackData::kFeedbackPatchSet:
            fprintf(stdout,
                    "HostFeedbackData::kFeedbackParameterSet %d %s type:%c",
                    data.patchSet.effect_id,
                    data.patchSet.key,
                    data.patchSet.type);
            switch (data.patchSet.type)
            {
            case 'f':
                fprintf(stdout, "value:%f\n", data.patchSet.data.f);
                break;
            default:
                fprintf(stdout, "\n");
                break;
            }
            break;

        default:
            break;
        }
    }

    void idle()
    {
        const uint32_t tick = lv_tick_get();

        // poll audio host changes, may trigger hostFeedbackCallback
        host.poll_feedback(this);

        // lvgl idle timer, triggers repaints and events
        lv_timer_periodic_handler();

        // request audio host updates every 15ms, for targetting 60fps
        if (lastHostUpdate == 0 || tick - lastHostUpdate >= 15)
        {
            lastHostUpdate = tick;
            host.output_data_ready();
        }

        // give idle time to other threads, needed as we use 1ms lvgl refresh period
       #ifdef _WIN32
        Sleep(0);
       #else
        sched_yield();
        usleep(0);
       #endif
    }

    void requestHostUpdates(lv_event_t* e)
    {
        switch (lv_event_get_code(e))
        {
        case LV_EVENT_RENDER_START:
            // dont request updates when starting to render
            lastHostUpdate = 0;
            break;

        case LV_EVENT_RENDER_READY:
            // request updates after rendering is done
            lastHostUpdate = lv_tick_get();
            host.output_data_ready();
            break;

        default:
            break;
        }
    }

    static void _requestHostUpdates(lv_event_t* e)
    {
        static_cast<HostApp *>(lv_event_get_user_data(e))->requestHostUpdates(e);
    }
};

// --------------------------------------------------------------------------------------------------------------------

static void slider_event_callback (lv_event_t* e)
{
    const auto eventCode = lv_event_get_code (e);
    auto* slider = lv_event_get_target_obj (e);
    auto* app = static_cast<HostApp *>(lv_event_get_user_data(e));
    auto& host = app->host;

    if (eventCode == LV_EVENT_VALUE_CHANGED)
    {
        // trigger example plugin changes, for both LV2 control ports and patch parameters styles
        const auto value = lv_slider_get_value (slider);
        const auto value_as_string = std::to_string (value);

        host.param_set(kPluginEgAmpId, kPluginEgAmpControl, value);
        host.patch_set(kPluginEgParamsId, kPluginEgParamsPatch, value_as_string.c_str());
    }
    else if (eventCode == LV_EVENT_RELEASED)
    {
        // request patch parameter value, for testing
        host.patch_get(kPluginEgParamsId, kPluginEgParamsPatch);
    }
}

// --------------------------------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
    HostApp app;
    auto& host = app.host;

    if (!app.ok)
    {
        fprintf(stderr, "Failed to connect to host: %s.\n", host.last_error.c_str());
        fprintf(stderr, "For faking a mod-host connection set MOD_DEV_HOST=1 in the running environment\n");
        return 1;
    }

    lv_init();

    lv_display_t* display = lv_sdl_window_create(800, 300);
    lv_display_set_default (display);

    lv_indev_t* mouse = lv_sdl_mouse_create();
    lv_indev_set_display (mouse, display);

    lv_indev_t* keyboard = lv_sdl_keyboard_create();
    lv_indev_set_display (keyboard, display);
    lv_obj_set_style_bg_color (lv_screen_active(), lv_color_hex (0x888888), LV_PART_MAIN);

    // Chain setup
    {
        // effect client names
        const std::string egAmp = format(MOD_HOST_EFFECT_PREFIX "%d", kPluginEgAmpId);
        const std::string egParams = format(MOD_HOST_EFFECT_PREFIX "%d", kPluginEgParamsId);
        // known info from plugin ttl
        const std::string egAmpIn = egAmp + ":in";
        const std::string egAmpOut = egAmp + ":out";

        // scope for fade-out + fade-in
        const Host::NonBlockingScopeWithAudioFades hnbswaf(host);

        // remove all plugins, in case of restart
        host.remove(-1);

        // add example plugins
        host.add(kPluginEgAmp, kPluginEgAmpId);
        host.add(kPluginEgParams, kPluginEgParamsId);

        // connect effect 0 to chain endpoints
        host.connect(JACK_CAPTURE_PORT_1, egAmpIn.c_str());
        host.connect(egAmpOut.c_str(), JACK_PLAYBACK_PORT_1);
        host.connect(egAmpOut.c_str(), JACK_PLAYBACK_PORT_2);
    }

    // Set up a slider to control the TanhClipper's pre-gain
    lv_obj_t* preGainSlider = lv_slider_create (lv_screen_active());
    lv_slider_set_range (preGainSlider, -120, 80);
    lv_slider_set_value (preGainSlider, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb (preGainSlider, slider_event_callback, LV_EVENT_VALUE_CHANGED, &app);
    lv_obj_add_event_cb (preGainSlider, slider_event_callback, LV_EVENT_RELEASED, &app);
    lv_obj_set_size (preGainSlider, 300, 30);
    lv_obj_center (preGainSlider);

    // initial lvgl idle, ensures first screen contents are visible
    lv_timer_periodic_handler();

    // sync host update request with drawing ready
    lv_display_add_event_cb(display, HostApp::_requestHostUpdates, LV_EVENT_RENDER_START, &app);
    lv_display_add_event_cb(display, HostApp::_requestHostUpdates, LV_EVENT_RENDER_READY, &app);

    while (running)
    {
        app.idle();
    }

    lv_deinit();

    return 0;
}
