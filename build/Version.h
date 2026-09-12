#pragma once
// clang-format off
// prevents C2018 during compilation
namespace {
const char *PLUGIN_NAME{ "RampAgent" };
#if DEV
const char *PLUGIN_VERSION{ "2.0.3-dev"};
#else
const char *PLUGIN_VERSION{ "2.0.3" };
#endif
const char *PLUGIN_AUTHOR{ "RampAgent Team+vACC-FR" };
const char *PLUGIN_LICENSE{ "GPLv3" };

static constexpr std::uint8_t PLUGIN_VERSION_MAJOR = 2;
static constexpr std::uint8_t PLUGIN_VERSION_MINOR = 0;
static constexpr std::uint8_t PLUGIN_VERSION_PATCH = 3;
}
