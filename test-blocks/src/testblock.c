#ifndef TESTBLOCK_URN
    #define TESTBLOCK_URN "urn:mod-connector:testblock"
#endif

#include <stdlib.h>

#include <lv2/core/lv2.h>

// --------------------------------------------------------------------------------------------------------------------

typedef struct {
    float* dummyporthandle;
} Plugin;

// --------------------------------------------------------------------------------------------------------------------

static LV2_Handle lv2_instantiate(const LV2_Descriptor* descriptor,
                                  double sampleRate,
                                  const char* bundle,
                                  const LV2_Feature* const* const features)
{
    Plugin* const self = calloc(1, sizeof(Plugin));

    if (self == NULL)
        return NULL;

    return self;

    // unused
    (void)descriptor;
    (void)sampleRate;
    (void)bundle;
    (void)features;
}

static void lv2_cleanup(LV2_Handle instance)
{
    free(instance);
}

static void lv2_connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    // unused
    (void)instance;
    (void)port;
    (void)data;
}

static void lv2_activate(LV2_Handle instance)
{
    (void)instance;
}

static void lv2_run(LV2_Handle instance, uint32_t nsamples)
{
    // unused
    (void)instance;
    (void)nsamples;
}

// --------------------------------------------------------------------------------------------------------------------

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    static const LV2_Descriptor descriptor = {
        .URI = TESTBLOCK_URN,
        .instantiate = lv2_instantiate,
        .connect_port = lv2_connect_port,
        .activate = lv2_activate,
        .run = lv2_run,
        .deactivate = NULL,
        .cleanup = lv2_cleanup,
        .extension_data = NULL,
    };

    return index == 0 ? &descriptor : NULL;
}
