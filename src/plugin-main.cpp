#include <obs-module.h>
#include <util/platform.h>

#ifndef PLUGIN_RELEASE_CHANNEL
#define PLUGIN_RELEASE_CHANNEL ""
#endif

namespace autoframing {
void register_auto_framing_filter();
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char* obs_module_description(void)
{
    return "Automatic virtual crop and pan video filter for OBS Studio.";
}

bool obs_module_load(void)
{
    const char* release_channel = PLUGIN_RELEASE_CHANNEL;
    blog(LOG_INFO, "[obs-auto-framing] loaded (version %s%s%s)", PLUGIN_VERSION,
         release_channel[0] != '\0' ? " " : "", release_channel);
    autoframing::register_auto_framing_filter();
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-auto-framing] unloaded");
}
