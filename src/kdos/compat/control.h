#pragma once
#include "setup.h"
class CommandLine;
class Config {
public:
    CommandLine *cmdline = nullptr;
    std::vector<std::string> auto_bat_additional, startup_params, configfiles, opt_o, opt_c, opt_set;
    std::string opt_lang, opt_machine;
    bool opt_noautoexec = false, opt_securemode = false, opt_fastlaunch = false, opt_noconfig = false,
         opt_nolog = false, opt_exit = false, opt_test = false, opt_prerun = false, opt_langcp = false;
    Section *GetSection(int) { return nullptr; }
    Section *GetSection(std::string const &name) const;
    Section *GetSectionFromProperty(char const *const) const { return nullptr; }
    bool SecureMode() const { return false; }
    void SwitchToSecureMode() {}
};
extern Config *control;
