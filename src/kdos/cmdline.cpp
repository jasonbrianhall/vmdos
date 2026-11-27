/* kdos: DOSBox-X CommandLine (from src/misc/setup.cpp, lines 1315-1790), unchanged */
/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#include "dosbox.h"
#include "logging.h"
#include "programs.h"
#include "support.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
bool CommandLine::FindExist(char const * const name,bool remove) {
    cmd_it it;
    if (!(FindEntry(name,it,false))) return false;
    if (remove) cmds.erase(it);
    return true;
}

bool CommandLine::FindHex(char const * const name,unsigned int & value,bool remove) {
    cmd_it it,it_next;
    if (!(FindEntry(name,it,true))) return false;
    it_next=it;++it_next;
    sscanf((*it_next).c_str(),"%X",(unsigned int*)(&value));
    if (remove) cmds.erase(it,++it_next);
    return true;
}

bool CommandLine::FindInt(char const * const name,int & value,bool remove) {
    cmd_it it,it_next;
    if (!(FindEntry(name,it,true))) return false;
    it_next=it;++it_next;
    value=atoi((*it_next).c_str());
    if (remove) cmds.erase(it,++it_next);
    return true;
}

bool CommandLine::FindString(char const * const name,std::string & value,bool remove) {
    cmd_it it,it_next;
    if (!(FindEntry(name,it,true))) return false;
    it_next=it;++it_next;
    value=*it_next;
    if (remove) cmds.erase(it,++it_next);
    return true;
}

bool CommandLine::FindCommand(unsigned int which,std::string & value,bool remove) {
    if (which<1) return false;
    if (which>cmds.size()) return false;
    cmd_it it=cmds.begin();
    for (;which>1;--which) ++it;
    value=(*it);
    if (remove) cmds.erase(it);
    return true;
}

bool CommandLine::EraseCommand(unsigned int which) {
    if (which<1) return false;
    if (which>cmds.size()) return false;
    cmd_it it=cmds.begin();
    for (;which>1;--which) ++it;
    cmds.erase(it);
    return true;
}

bool CommandLine::ExistsCommand(unsigned int which) {
    if (which<1) return false;
    if (which>cmds.size()) return false;
    return true;
}

void CommandLine::DebugDump(const int debuglevel) {
    unsigned int i=1;

    if (debuglevel >= 0)
        LOG(LOG_MISC,(LOG_SEVERITIES)debuglevel)("CommandLine::DebugDump(this=%p)",(void*)this);
    else
        LOG_MSG("CommandLine::DebugDump(this=%p)",(void*)this);

    for (auto it=cmds.begin();it!=cmds.end();it++,i++) {
        if (debuglevel >= 0)
            LOG(LOG_MISC,(LOG_SEVERITIES)debuglevel)("  [%u] = \"%s\"",i,(*it).c_str());
        else
            LOG_MSG("  [%u] = \"%s\"",i,(*it).c_str());
    }
}

bool CommandLine::FindEntry(char const * const name,cmd_it & it,bool neednext) {
    for (it = cmds.begin(); it != cmds.end(); ++it) {
        const char *d = (*it).c_str();

        /* HACK: If the search string starts with -, it's a switch,
         *       so adjust pointers so that it matches --switch or -switch */
        if (*name == '-' && d[0] == '-' && d[1] == '-') d++;

        if (!strcasecmp(d,name)) {
            cmd_it itnext=it;++itnext;
            if (neednext && (itnext==cmds.end())) return false;
            return true;
        }
    }
    return false;
}

bool CommandLine::FindStringBegin(char const* const begin,std::string & value, bool remove) {
    size_t len = strlen(begin);
    for (cmd_it it = cmds.begin(); it != cmds.end();++it) {
        if (strncasecmp(begin,(*it).c_str(),len)==0) {
            value=((*it).c_str() + len);
            if (remove) cmds.erase(it);
            return true;
        }
    }
    return false;
}

bool CommandLine::FindStringFullBegin(char const* const begin,std::string & value, bool squote, bool remove) {
    size_t len = strlen(begin);
    char c = squote?'\'':'\"';
    for (cmd_it it=cmds.begin();it!=cmds.end();++it) {
        if (strncmp(begin,(*it).c_str(),len)==0) {
            bool q=(*it)[len]==c;
            value=((*it).c_str() + len + (q?1:0));
            if (remove) cmds.erase(it);
            if (q) {
                std::string str=value;
                if (str.size() && str.back()==c)
                    value.pop_back();
                else while (str.size()&&++it!=cmds.end()) {
                    str=(*it);
                    if (remove) cmds.erase(it);
                    value+=" "+str;
                    if (str.size()&&str.back()==c) {
                        value.pop_back();
                        break;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool CommandLine::FindStringRemain(char const * const name,std::string & value) {
    cmd_it it;value.clear();
    if (!FindEntry(name,it)) return false;
    ++it;
    for (;it != cmds.end();++it) {
        value += " ";
        value += (*it);
    }
    return true;
}

/* Only used for parsing command.com /C
 * Allowing /C dir and /Cdir
 * Restoring quotes back into the commands so command /C mount d "/tmp/a b" works as intended
 */
bool CommandLine::FindStringRemainBegin(char const * const name,std::string & value) {
    cmd_it it;value.clear();
    if (!FindEntry(name,it)) {
        size_t len = strlen(name);
            for (it = cmds.begin();it != cmds.end();++it) {
                if (strncasecmp(name,(*it).c_str(),len)==0) {
                    std::string temp = (*it).c_str() + len;
                    //Restore quotes for correct parsing in later stages
                    if(temp.find(' ') != std::string::npos)
                        value = std::string("\"") + temp + std::string("\"");
                    else
                        value = temp;
                    break;
                }
            }
        if (it == cmds.end()) return false;
    }
    ++it;
    for (;it != cmds.end();++it) {
        value += " ";
        std::string temp = *it;
        if(temp.find(' ') != std::string::npos)
            value += std::string("\"") + temp + std::string("\"");
        else
            value += temp;
    }
    return true;
}

bool CommandLine::GetStringRemain(std::string & value) {
    if (!cmds.size()) return false;

    cmd_it it = cmds.begin();value = (*it++);
    for(;it != cmds.end();++it) {
        value += " ";
        value += (*it);
    }
    return true;
}


unsigned int CommandLine::GetCount(void) {
    return (unsigned int)cmds.size();
}

bool CommandLine::GetCurrentArgv(std::string &argv) {
    if (opt_scan != cmds.end())
        argv = *opt_scan;

    return false;
}

bool CommandLine::CurrentArgvEnd(void) {
    return (opt_scan == cmds.end());
}

void CommandLine::EatCurrentArgv(void) {
    if (opt_scan != cmds.end()) opt_scan = cmds.erase(opt_scan);
}

void CommandLine::NextArgv(void) {
    if (opt_scan != cmds.end()) ++opt_scan;
}

bool CommandLine::NextOptArgv(std::string &argv) {
    argv.clear();

    /* no argv to return if we're doing single-char GNU switches */
    if (!opt_gnu_getopt_singlechar.empty()) return false;

    if (opt_scan == cmds.end()) return false;
    argv = *opt_scan;
    if (opt_eat_argv) opt_scan = cmds.erase(opt_scan);
    else ++opt_scan;
    return true;
}

void CommandLine::ChangeOptStyle(enum opt_style opt_style) {
    this->opt_style = opt_style;
}

bool CommandLine::BeginOpt(bool eat_argv) {
    opt_gnu_getopt_singlechar.clear();
    opt_scan = cmds.begin();
    if (opt_scan == cmds.end()) return false;
    opt_eat_argv = eat_argv;
    return true;
}

bool CommandLine::GetOptGNUSingleCharCheck(std::string &name) {
    /* return another char, skipping spaces or invalid chars */
    name.clear();
    while (!opt_gnu_getopt_singlechar.empty()) {
        char c = opt_gnu_getopt_singlechar.at(0);
        opt_gnu_getopt_singlechar = opt_gnu_getopt_singlechar.substr(1);
        if (c <= ' ' || c > 126) continue;

        name = c;
        return true;
    }

    return false;
}

bool CommandLine::GetOpt(std::string &name) {
    name.clear();

    /* if we're still doing GNU getopt single-char switches, then parse another and return */
    if (GetOptGNUSingleCharCheck(name))
        return true;

    while (opt_scan != cmds.end()) {
        std::string &argv = *opt_scan;
        const char *str = argv.c_str();

        if (opt_style == CommandLine::either_except && *str == '/' && (strchr(str+1, '/')==NULL && strchr(str+1, '\\')==NULL && (strlen(str)<6 || (strlen(str)>5 && strcasecmp(str+strlen(str)-4, ".bat") && strcasecmp(str+strlen(str)-4, ".com") && strcasecmp(str+strlen(str)-4, ".exe"))))) {
            /* MS-DOS style /option, with the exception of ending with .BAT, .COM, .EXE */
            name = str+1; /* copy to caller minus leaking slash, then erase/skip */
            if (opt_eat_argv) opt_scan = cmds.erase(opt_scan);
            else ++opt_scan;
            return true;
        }
        else if ((opt_style == CommandLine::either || opt_style == CommandLine::dos) && *str == '/') {
            /* MS-DOS style /option. Example: /A /OPT /HAX /BLAH */
            name = str+1; /* copy to caller minus leaking slash, then erase/skip */
            if (opt_eat_argv) opt_scan = cmds.erase(opt_scan);
            else ++opt_scan;
            return true;
        }
        else if ((opt_style == CommandLine::either || opt_style == CommandLine::either_except || opt_style == CommandLine::gnu || opt_style == CommandLine::gnu_getopt) && *str == '-') {
            str++; /* step past '-' */
            if (str[0] == '-' && str[1] == 0) { /* '--' means to stop parsing */
                opt_scan = cmds.end();
                if (opt_eat_argv) opt_scan = cmds.erase(opt_scan);
                break;
            }

            if (opt_style == CommandLine::gnu_getopt) {
                /* --switch => "switch"
                 * -switch => -s -w -i -t -c -h */
                if (*str == '-') {
                    str++;
                }
                else {
                    /* assign to single-char parse then eat the argv */
                    opt_gnu_getopt_singlechar = str;
                    if (opt_eat_argv) opt_scan = cmds.erase(opt_scan);
                    else ++opt_scan;

                    /* if we parse a single-char switch, great */
                    if (GetOptGNUSingleCharCheck(name))
                        return true;

                    continue; /* if we're here then there was nothing to parse, continue */
                }
            }
            else {
                /* -switch and --switch mean the same thing */
                if (*str == '-') str++;
            }

            name = str; /* copy to caller, then erase/skip */
            if (opt_eat_argv) opt_scan = cmds.erase(opt_scan);
            else ++opt_scan;
            return true;
        }
        else {
            ++opt_scan;
        }
    }

    return false;
}

void CommandLine::EndOpt() {
    opt_scan = cmds.end();
}

void CommandLine::FillVector(std::vector<std::string> & vector) {
    for(cmd_it it = cmds.begin(); it != cmds.end(); ++it) {
        vector.push_back((*it));
    }
#ifdef WIN32
    // add back the \" if the parameter contained a space
    for(Bitu i = 0; i < vector.size(); i++) {
        if (vector[i].find(' ') != std::string::npos) {
            vector[i] = "\""+vector[i]+"\"";
        }
    }
#endif
}

int CommandLine::GetParameterFromList(const char* const params[], std::vector<std::string> & output) {
    // return values: 0 = P_NOMATCH, 1 = P_NOPARAMS
    // TODO return nomoreparams
    int retval = 1;
    output.clear();
    enum {
        P_START, P_FIRSTNOMATCH, P_FIRSTMATCH
    } parsestate = P_START;
    cmd_it it = cmds.begin();
    while(it != cmds.end()) {
        bool found = false;
        for(unsigned int i = 0; params[i] != NULL; i++) {
            if (*params[i] == 0) {
                LOG_MSG("FIXME: GetParameterFromList: terminating params[] with \"\" is deprecated. Please terminate the param list with NULL");
                break;
            }

            if (!strcasecmp((*it).c_str(),params[i])) {
                // found a parameter
                found = true;
                switch(parsestate) {
                case P_START:
                    retval = int(i)+2;
                    parsestate = P_FIRSTMATCH;
                    break;
                case P_FIRSTMATCH:
                case P_FIRSTNOMATCH:
                    return retval;
                }
            }
        }
        if (!found) 
            switch(parsestate) {
            case P_START:
                retval = 0; // no match
                parsestate = P_FIRSTNOMATCH;
                output.push_back(*it);
                break;
            case P_FIRSTMATCH:
            case P_FIRSTNOMATCH:
                output.push_back(*it);
                break;
            }
        cmd_it itold = it;
        ++it;
        cmds.erase(itold);

    }

    return retval;
}

CommandLine::CommandLine(int argc,char const * const argv[],enum opt_style opt) {
    if (argc>0) {
        file_name=argv[0];
    }
    int i=1;
    opt_style = opt;
    while (i<argc) {
        if (!raw_cmdline.empty()) raw_cmdline += " ";
        raw_cmdline += argv[i];

        cmds.emplace_back(argv[i]);
        i++;
    }
}

const std::string& CommandLine::GetRawCmdline(void) {
    return raw_cmdline;
}

uint16_t CommandLine::Get_arglength() {
    if (cmds.empty()) return 0;
    uint16_t i=1;
    for(cmd_it it = cmds.begin();it != cmds.end(); ++it)
        i+=(*it).size() + 1;
    return --i;
}

CommandLine::CommandLine(char const * const name,char const * const cmdline,enum opt_style opt, bool squote) {
    if (name) file_name=name;
    /* Parse the cmds and put them in the list */
    bool inword,inquote;char c;
    inword=false;inquote=false;
    std::string str;
    raw_cmdline = cmdline;
    const char * c_cmdline=cmdline;
    opt_style = opt;
    while ((c=*c_cmdline)!=0) {
        if (inquote) {
            if (c!='"'&&!squote) str+=c;
            else {
                inquote=false;
                cmds.push_back(str);
                str.erase();
            }
        } else if (inword) {
            if (c!=' ') str+=c;
            else {
                inword=false;
                cmds.push_back(str);
                str.erase();
            }
        }
        else if (c=='\"'&&!squote) { inquote=true;}
        else if (c!=' ') { str+=c;inword=true;}
        c_cmdline++;
    }
    if (inword || inquote) cmds.push_back(str);
}

void CommandLine::Shift(unsigned int amount) {
    while(amount--) {
        file_name = cmds.size()?*(cmds.begin()):"";
        if (cmds.size()) cmds.erase(cmds.begin());
    }
}
