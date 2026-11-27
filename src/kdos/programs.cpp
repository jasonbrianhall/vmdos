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


#include <vector>
#include <sstream>
#include <ctype.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "programs.h"
#include "callback.h"
#include "logging.h"
#include "regs.h"
#include "support.h"
#include "cross.h"
#include "control.h"
#include "shell.h"
#include "menudef.h"
#include "hardware.h"
#include "../dos/drives.h"
#include "mapper.h"
#include "menu.h"
#include "bios.h"
#include "timer.h"
#include "jfont.h"
#include "render.h"
#include "../ints/int10.h"
#include "bios_disk.h"
#include "imagedisk_msdosblockdev.h"
#include "sdlmain.h"
#include "cpu.h"
#if defined(WIN32)
#include "windows.h"
RECT monrect;
int curscreen;
#endif

#include <output/output_tools.h>
#include <output/output_ttf.h>

Bitu call_program;
extern char lastmount;
extern const char *modifier;
extern unsigned int sendkeymap;
extern std::string langname, configfile, dosbox_title;
extern int autofixwarn, enablelfn, fat32setver, paste_speed, wheel_key, freesizecap, wpType, wpVersion, wpBG, wpFG, lastset, blinkCursor, msgcodepage;
extern bool dos_kernel_disabled, force_nocachedir, wpcolon, convertimg, lockmount, enable_config_as_shell_commands, lesssize, load, winrun, winautorun, startcmd, startwait, startquiet, starttranspath, mountwarning, wheel_guest, clipboard_dosapi, dos_utf8names, noremark_save_state, force_load_state, sync_time, manualtime, ttfswitch, loadlang, showbold, showital, showline, showsout, char512, printfont, rtl, gbk, chinasea, uao, showdbcs, dbcs_sbcs, autoboxdraw, halfwidthkana, ticksLocked, outcon, enable_dbcs_tables, show_recorded_filename, internal_program, pipetmpdev, notrysgf, uselangcp, incall;
extern bool clipboard_biospaste;
/* This registers a file on the virtual drive and creates the correct structure for it*/

static uint8_t exe_block[]={
	0xbc,0x00,0x04,					//0x100 MOV SP,0x400  decrease stack size
	0xbb,0x40,0x00,					//0x103 MOV BX,0x0040 for memory resize
	0xb4,0x4a,					//0x106 MOV AH,0x4A   Resize memory block
	0xcd,0x21,					//0x108 INT 0x21      ...
	0x30,0xc0,					//0x10A XOR AL,AL     Clear AL (exit code). Program will write AL to modify exit status
//pos 14 is callback number
	0xFE,0x38,0x00,0x00,				//0x10C CALLBack number
	0xb4,0x4c,					//0x110 Mov AH,0x4C   Prepare to exit, preserve AL
	0xcd,0x21					//0x112 INT 0x21      Exit to DOS
};							//0x114 --DONE--

#define CB_POS 14

class InternalProgramEntry {
public:
	InternalProgramEntry() {
		main = NULL;
		comsize = 0;
		comdata = NULL;
	}
	~InternalProgramEntry() {
		if (comdata != NULL) free(comdata);
		comdata = NULL;
		comsize = 0;
		main = NULL;
	}
public:
	std::string	name;
	uint8_t*		comdata;
	uint32_t		comsize;
	PROGRAMS_Main*	main;
};

static std::vector<InternalProgramEntry*> internal_progs;
uint8_t DOS_GetAnsiAttr(void);
int setTTFMap(bool changecp);
char *FormatDate(uint16_t year, uint8_t month, uint8_t day);
bool isDBCSCP(void), CheckBoxDrawing(uint8_t c1, uint8_t c2, uint8_t c3, uint8_t c4), DOS_SetAnsiAttr(uint8_t attr), GFX_GetPreventFullscreen(void), toOutput(const char *what);
void EMS_DoShutDown(void), UpdateDefaultPrinterFont(void), GFX_ForceRedrawScreen(void), resetFontSize(void), ttf_reset_colors(void), makestdcp950table(void), makeseacp951table(void), clearFontCache(void), DOSBox_SetSysMenu(void), MSG_Init(void), initRand(void), PRINTER_Init(void), SetKEYBCP(void);
void EMS_Startup(Section* sec), DOSV_SetConfig(Section_prop *section), DOSBOX_UnlockSpeed2(bool pressed), RebootLanguage(std::string filename, bool confirm=false), SetWindowTransparency(int trans), SetOutputSwitch(const char *outputstr), runRescan(const char *str), runSerial(const char *str), runParallel(const char *str), DOS_AddDays(uint8_t days), PRINTER_Shutdown(Section* sec), setAspectRatio(Section_prop * section);

void PROGRAMS_Shutdown(void) {
	LOG(LOG_MISC,LOG_DEBUG)("Shutting down internal programs list");

	for (size_t i=0;i < internal_progs.size();i++) {
		if (internal_progs[i] != NULL) {
			delete internal_progs[i];
			internal_progs[i] = NULL;
		}
	}
	internal_progs.clear();
}

void PROGRAMS_MakeFile(char const * const name,PROGRAMS_Main * main,const char *dir) {
	uint32_t size=sizeof(exe_block)+sizeof(uint8_t);
	InternalProgramEntry *ipe;
	uint8_t *comdata;
	uint8_t index;

	/* Copy save the pointer in the vector and save its index */
	if (internal_progs.size()>255) E_Exit("PROGRAMS_MakeFile program size too large (%d)",static_cast<int>(internal_progs.size()));

	index = (uint8_t)internal_progs.size();
	comdata = (uint8_t *)malloc(32); //MEM LEAK
	memcpy(comdata,&exe_block,sizeof(exe_block));
	memcpy(&comdata[sizeof(exe_block)],&index,sizeof(index));
	comdata[CB_POS]=(uint8_t)(call_program&0xff);
	comdata[CB_POS+1]=(uint8_t)((call_program>>8)&0xff);

	ipe = new InternalProgramEntry();
	ipe->main = main;
	ipe->name = name;
	ipe->comsize = size;
	ipe->comdata = comdata;
	internal_progs.push_back(ipe);
	internal_program = true;
	VFILE_Register(name,ipe->comdata,ipe->comsize,dir);
	internal_program = false;
}

static Bitu PROGRAMS_Handler(void) {
	/* This sets up everything for a program start up call */
	Bitu size=sizeof(uint8_t);
	uint8_t index;
	/* Read the index from program code in memory */
	PhysPt reader=PhysMake(dos.psp(),256+sizeof(exe_block));
	HostPt writer=(HostPt)&index;
	for (;size>0;size--) *writer++=mem_readb(reader++);
	Program * new_program = NULL;
	if (index >= internal_progs.size()) E_Exit("something is messing with the memory");
	InternalProgramEntry *ipe = internal_progs[index];
	if (ipe == NULL) E_Exit("Attempt to run internal program slot with nothing allocated");
	if (ipe->main == NULL) return CBRET_NONE;
	PROGRAMS_Main * handler = internal_progs[index]->main;
	(*handler)(&new_program);

	/* kdos: no exceptions */
	new_program->Run();
	delete new_program;

	return CBRET_NONE;
}

/* Main functions used in all program */

Program::Program(const unsigned int fl) {
	if (dos_kernel_disabled && !(fl & prg_nopsp))
		LOG(LOG_MISC,LOG_DEBUG)("Program constructor: DOS kernel disabled and prg_nopsp was not specified");

	if (!dos_kernel_disabled && !(fl & prg_nopsp)) {
		/* Find the command line and setup the PSP */
		psp = new DOS_PSP(dos.psp());
		/* Scan environment for filename */
		PhysPt envscan=PhysMake(psp->GetEnvironment(),0);
		while (mem_readb(envscan)) envscan+=(PhysPt)(mem_strlen(envscan)+1);	
		envscan+=3;
		CommandTail tail;
		MEM_BlockRead(PhysMake(dos.psp(),CTBUF+1),&tail,CTBUF+1);
		if (tail.count<CTBUF) tail.buffer[tail.count]=0;
		else tail.buffer[CTBUF-1]=0;
		char filename[256+1];
		MEM_StrCopy(envscan,filename,256);
		cmd = new CommandLine(filename,tail.buffer);
	}
	else {
		psp = NULL;
		cmd = NULL;
	}

	exit_status = 0;
}

extern std::string full_arguments;

void Program::WriteExitStatus() {
	/* the exe block was modified that on return to DOS only AH is modified, leaving AL normally
	 * zero but we're free to set AL to any other value to set exit code */
	reg_al = exit_status;
}

void Program::ChangeToLongCmd() {
	/* 
	 * Get arguments directly from the shell instead of the psp.
	 * this is done in securemode: (as then the arguments to mount and friends
	 * can only be given on the shell ( so no int 21 4b) 
	 * Securemode part is disabled as each of the internal command has already
	 * protection for it. (and it breaks games like cdman)
	 * it is also done for long arguments too as it is convenient (as the total commandline can be longer than 127 characters.
	 * imgmount with lots of parameters
	 * Length of arguments can be ~120. but switch when above 100 to be sure
	 */

	if (/*control->SecureMode() ||*/ cmd->Get_arglength() > 100 && full_arguments.size()) {
		CommandLine* temp = new CommandLine(cmd->GetFileName(),full_arguments.c_str());
		delete cmd;
		cmd = temp;
	}
	full_arguments.assign(""); //Clear so it gets even more save
}

bool resetcolor = false;
static char last_written_character = 0;//For 0xA to OxD 0xA expansion
void Program::WriteOut(const char * format,...) {
	if (dos_kernel_disabled) {
		LOG(LOG_MISC,LOG_WARN)("Call to Program::WriteOut while DOS kernel is disabled string format='%s'",format);
		return;
	}

	uint8_t attr = DOS_GetAnsiAttr();
	char buf[2048];
	va_list msg;
	
	va_start(msg,format);
	vsnprintf(buf,2047,format,msg);
	va_end(msg);

	uint16_t size = (uint16_t)strlen(buf);
	dos.internal_output=true;
	for(uint16_t i = 0; i < size;i++) {
		uint8_t out;uint16_t s=1;
		if (buf[i] == 0xA && last_written_character != 0xD) {
			out = 0xD;DOS_WriteFile(STDOUT,&out,&s);
		}
		last_written_character = (char)(out = (uint8_t)buf[i]);
        if(isDBCSCP() && isKanji1(buf[i]) && i + 1 < size && isKanji2(buf[i + 1])) {
            uint8_t dbchar[3] = { (uint8_t)buf[i], (uint8_t)buf[i + 1],0 };
            s = 2;
            DOS_WriteFile(STDOUT, dbchar, &s); // send two bytes if next character is a DBCS character
            i++;
            s = 1;
            continue;
        }
        DOS_WriteFile(STDOUT,&out,&s);
	}
	dos.internal_output=false;
	if (resetcolor && attr) DOS_SetAnsiAttr(attr);
	resetcolor = false;
	
//	DOS_WriteFile(STDOUT,(uint8_t *)buf,&size);
}

void Program::WriteOut(const char *format, const char *arguments) {
	if (dos_kernel_disabled) {
		LOG(LOG_MISC,LOG_WARN)("Call to Program::WriteOut while DOS kernel is disabled string format='%s'",format);
		return;
	}

	char buf[2048 + CMD_MAXLINE];
	sprintf(buf,format,arguments);

	uint16_t size = (uint16_t)strlen(buf);
	dos.internal_output=true;
	for(uint16_t i = 0; i < size;i++) {
        uint8_t out; uint16_t s = 1;
		if (buf[i] == 0xA && last_written_character != 0xD) {
			out = 0xD;DOS_WriteFile(STDOUT,&out,&s);
		}
		last_written_character = (char)(out = (uint8_t)buf[i]);
        if(isDBCSCP() && isKanji1(buf[i]) && i + 1 < size && isKanji2(buf[i + 1])) {
            uint8_t dbchar[3] = { (uint8_t)buf[i], (uint8_t)buf[i + 1], 0};
            s = 2;
            DOS_WriteFile(STDOUT, dbchar, &s); // send two bytes if next character is a DBCS character
            i++;
            s = 1;
            continue;
        }
        DOS_WriteFile(STDOUT,&out,&s);
	}
	dos.internal_output=false;

//	DOS_WriteFile(STDOUT,(uint8_t *)buf,&size);
}

DOS_Device* DOS_GetCONDevice(void);
int WriteOut_NoParsing_Internal(const char* format, bool dbcs, bool direct_con);

int Program::WriteOut_NoParsing(const char* format, bool dbcs) {
    return WriteOut_NoParsing_Internal(format, dbcs, false);
}

int Program::WriteCON(const char* str, bool dbcs) {
    return WriteOut_NoParsing_Internal(str, dbcs, true);
}

int WriteOut_NoParsing_Internal(const char* format, bool dbcs, bool direct_con) {
    if(dos_kernel_disabled) {
        LOG(LOG_MISC, LOG_WARN)("Call to Program::WriteOut while DOS kernel is disabled string format='%s'", format);
        return 0;
    }
    if(!format || (direct_con && !DOS_GetCONDevice())) return 0;

    uint16_t size = (uint16_t)strlen(format);
    char const* buf = format;
    char last2 = 0, last3 = 0;
    int lastcol = 0;
    uint8_t page = outcon ? real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE) : 0;
    bool lead = false;
    bool old_internal_output = dos.internal_output;
    dos.internal_output = true;
    int rcount = 0;
    for(uint16_t i = 0; i < size; i++) {
        uint8_t out; uint16_t s = 1;
        BIOS_NCOLS;
        if(!CURSOR_POS_COL(page)) last2 = last3 = 0;
        if(lead) lead = false;
        else if((IS_PC98_ARCH || isDBCSCP())
#if defined(USE_TTF)
            && dbcs_sbcs
#endif
            && dbcs && isKanji1(buf[i])) lead = true;
        if(buf[i] == 0xA) {
            if(last_written_character != 0xD) {
                out = 0xD;
                if(direct_con) DOS_GetCONDevice()->Write(&out, &s);
                else DOS_WriteFile(STDOUT, &out, &s);
            }
            if(outcon) rcount++;
        }
        else if(outcon && lead && CURSOR_POS_COL(page) == ncols - 1 && !(TTF_using()
#if defined(USE_TTF)
            && autoboxdraw
#endif
            && CheckBoxDrawing(last3, last2, last_written_character, buf[i]))) {
            out = 0xD;
            if(direct_con) DOS_GetCONDevice()->Write(&out, &s);
            else DOS_WriteFile(STDOUT, &out, &s);
            out = 0xA;
            if(direct_con) DOS_GetCONDevice()->Write(&out, &s);
            else DOS_WriteFile(STDOUT, &out, &s);
            rcount++;
        }
        else if(outcon && !CURSOR_POS_COL(page) && lastcol == ncols - 1)
            rcount++;
        lastcol = CURSOR_POS_COL(page);
        last3 = last2; last2 = last_written_character;
        last_written_character = (char)(out = (uint8_t)buf[i]);
        if(direct_con) DOS_GetCONDevice()->Write(&out, &s);
        else DOS_WriteFile(STDOUT, &out, &s);
    }
    dos.internal_output = old_internal_output;
    return rcount;
}


static bool LocateEnvironmentBlock(PhysPt &env_base,PhysPt &env_fence,Bitu env_seg) {
	if (env_seg == 0) {
		/* The DOS program might have freed it's environment block perhaps. */
		return false;
	}

	DOS_MCB env_mcb((uint16_t)(env_seg-1)); /* read the environment block's MCB to determine how large it is */
	env_base = PhysMake((uint16_t)env_seg,0);
	env_fence = env_base + (PhysPt)(env_mcb.GetSize() << 4u);
	return true;
}

int EnvPhys_StrCmp(PhysPt es,PhysPt ef,const char *ls) {
    (void)ef;//UNUSED
	while (1) {
		unsigned char a = mem_readb(es++);
		unsigned char b = (unsigned char)(*ls++);
		if (a == '=') a = 0;
		if (a == 0 && b == 0) break;
		if (a == b) continue;
		return (int)a - (int)b;
	}

	return 0;
}

void EnvPhys_StrCpyToCPPString(std::string &result,PhysPt &env_scan,PhysPt env_fence) {
	char tmp[512],*w=tmp,*wf=tmp+sizeof(tmp)-1;

	result.clear();
	while (env_scan < env_fence) {
		char c;
		if ((c=(char)mem_readb(env_scan++)) == 0) break;

		if (w >= wf) {
			*w = 0;
			result += tmp;
			w = tmp;
		}

		assert(w < wf);
		*w++ = c;
	}
	if (w != tmp) {
		*w = 0;
		result += tmp;
	}
}

bool EnvPhys_ScanUntilNextString(PhysPt &env_scan,PhysPt env_fence) {
	/* scan until end of block or NUL */
	while (env_scan < env_fence && mem_readb(env_scan) != 0) env_scan++;

	/* if we hit the fence, that's something to warn about. */
	if (env_scan >= env_fence) {
		LOG_MSG("Warning: environment string scan hit the end of the environment block without terminating NUL\n");
		return false;
	}

	/* if we stopped at anything other than a NUL, that's something to warn about */
	if (mem_readb(env_scan) != 0) {
		LOG_MSG("Warning: environment string scan scan stopped without hitting NUL\n");
		return false;
	}

	env_scan++; /* skip NUL */
	return true;
}

bool Program::GetEnvStr(const char * entry,std::string & result) {
	PhysPt env_base,env_fence,env_scan;

	if (dos_kernel_disabled) {
		LOG_MSG("BUG: Program::GetEnvNum() called with DOS kernel disabled (such as OS boot).\n");
		return false;
	}

	if (!LocateEnvironmentBlock(env_base,env_fence,psp->GetEnvironment())) {
		LOG_MSG("Warning: GetEnvCount() was not able to locate the program's environment block\n");
		return false;
	}

	std::string bigentry(entry);
	for (std::string::iterator it = bigentry.begin(); it != bigentry.end(); ++it) *it = toupper(*it);

	env_scan = env_base;
	while (env_scan < env_fence) {
		/* "NAME" + "=" + "VALUE" + "\0" */
		/* end of the block is a NULL string meaning a \0 follows the last string's \0 */
		if (mem_readb(env_scan) == 0) break; /* normal end of block */

		if (EnvPhys_StrCmp(env_scan,env_fence,bigentry.c_str()) == 0) {
			EnvPhys_StrCpyToCPPString(result,env_scan,env_fence);
			return true;
		}

		if (!EnvPhys_ScanUntilNextString(env_scan,env_fence)) break;
	}

	return false;
}

bool Program::GetEnvNum(Bitu want_num,std::string & result) {
	PhysPt env_base,env_fence,env_scan;
	Bitu num = 0;

	if (dos_kernel_disabled) {
		LOG_MSG("BUG: Program::GetEnvNum() called with DOS kernel disabled (such as OS boot).\n");
		return false;
	}

	if (!LocateEnvironmentBlock(env_base,env_fence,psp->GetEnvironment())) {
		LOG_MSG("Warning: GetEnvCount() was not able to locate the program's environment block\n");
		return false;
	}

	result.clear();
	env_scan = env_base;
	while (env_scan < env_fence) {
		/* "NAME" + "=" + "VALUE" + "\0" */
		/* end of the block is a NULL string meaning a \0 follows the last string's \0 */
		if (mem_readb(env_scan) == 0) break; /* normal end of block */

		if (num == want_num) {
			EnvPhys_StrCpyToCPPString(result,env_scan,env_fence);
			return true;
		}

		num++;
		if (!EnvPhys_ScanUntilNextString(env_scan,env_fence)) break;
	}

	return false;
}

Bitu Program::GetEnvCount(void) {
	PhysPt env_base,env_fence,env_scan;
	Bitu num = 0;

	if (dos_kernel_disabled) {
		LOG_MSG("BUG: Program::GetEnvCount() called with DOS kernel disabled (such as OS boot).\n");
		return 0;
	}

	if (!LocateEnvironmentBlock(env_base,env_fence,psp->GetEnvironment())) {
		LOG_MSG("Warning: GetEnvCount() was not able to locate the program's environment block\n");
		return false;
	}

	env_scan = env_base;
	while (env_scan < env_fence) {
		/* "NAME" + "=" + "VALUE" + "\0" */
		/* end of the block is a NULL string meaning a \0 follows the last string's \0 */
		if (mem_readb(env_scan++) == 0) break; /* normal end of block */
		num++;
		if (!EnvPhys_ScanUntilNextString(env_scan,env_fence)) break;
	}

	return num;
}

void Program::DebugDumpEnv() {
	PhysPt env_base,env_fence,env_scan;
	unsigned char c;
	std::string tmp;

	if (dos_kernel_disabled)
		return;

	if (!LocateEnvironmentBlock(env_base,env_fence,psp->GetEnvironment()))
		return;

	env_scan = env_base;
	LOG_MSG("DebugDumpEnv()");
	while (env_scan < env_fence) {
		if (mem_readb(env_scan) == 0) break;

		while (env_scan < env_fence) {
			if ((c=mem_readb(env_scan++)) == 0) break;
			tmp += (char)c;
		}

		LOG_MSG("...%s",tmp.c_str());
		tmp = "";
	}
}

bool Program::FirstEnv(const char * entry) {
	PhysPt env_base,env_fence,env_scan,env_first,env_last;
	bool found = false;

	if (dos_kernel_disabled) {
		LOG_MSG("BUG: Program::FirstEnv() called with DOS kernel disabled (such as OS boot).\n");
		return false;
	}

	if (!LocateEnvironmentBlock(env_base,env_fence,psp->GetEnvironment())) {
		LOG_MSG("Warning: GetEnvCount() was not able to locate the program's environment block\n");
		return false;
	}

	std::string bigentry(entry);
	for (std::string::iterator it = bigentry.begin(); it != bigentry.end(); ++it) *it = toupper(*it);

	env_scan = env_base;
	while (env_scan < env_fence) {
		/* "NAME" + "=" + "VALUE" + "\0" */
		/* end of the block is a NULL string meaning a \0 follows the last string's \0 */
		if (mem_readb(env_scan) == 0) break; /* normal end of block */

		if (EnvPhys_StrCmp(env_scan,env_fence,bigentry.c_str()) == 0) {
			found = true;
			break;
		}

		if (!EnvPhys_ScanUntilNextString(env_scan,env_fence)) break;
	}

	if (found) {
		env_first = env_scan;
		if (!EnvPhys_ScanUntilNextString(env_scan,env_fence)) return false;
		env_last = env_scan;

#if 0//DEBUG
		fprintf(stderr,"Env base=%x fence=%x first=%x last=%x\n",
			(unsigned int)env_base,  (unsigned int)env_fence,
			(unsigned int)env_first, (unsigned int)env_last);
#endif

		assert(env_first <= env_last);

		/* if the variable is already at the beginning, do nothing */
		if (env_first == env_base) return true;

		{
			std::vector<uint8_t> tmp;
			tmp.resize(size_t(env_last-env_first));

			/* save variable */
			for (size_t i=0;i < tmp.size();i++)
				tmp[i] = mem_readb(env_first+(PhysPt)i);

			/* shift all variables prior to it forward over the variable, BACKWARDS */
			const size_t pl = size_t(env_first - env_base);
			assert((env_first-pl) == env_base);
			assert((env_last-pl) >= env_base);
			assert(env_first < env_last);
			assert(pl != 0);
			for (size_t i=0;i < pl;i++) mem_writeb(env_last-(i+1), mem_readb(env_first-(i+1)));

			/* put the variable in at the beginning */
			assert((env_base+tmp.size()) == (env_last-pl));
			for (size_t i=0;i < tmp.size();i++)
				mem_writeb(env_base+(PhysPt)i,tmp[i]);
		}
	}

	return true;
}

bool Program::EraseEnv(void) {
	PhysPt env_base,env_fence;
	size_t nsl = 0,el = 0,needs;

	if (dos_kernel_disabled) {
		LOG_MSG("BUG: Program::EraseEnv() called with DOS kernel disabled (such as OS boot).\n");
		return false;
	}

	if (!LocateEnvironmentBlock(env_base,env_fence,psp->GetEnvironment())) {
		LOG_MSG("Warning: SetEnv() was not able to locate the program's environment block\n");
		return false;
	}

	for (PhysPt w=env_base;w < env_fence;w++)
		mem_writeb(w,0);

	return true;
}

/* NTS: "entry" string must have already been converted to uppercase */
bool Program::SetEnv(const char * entry,const char * new_string) {
	PhysPt env_base,env_fence,env_scan;
	size_t nsl = 0,el = 0,needs;

	if (dos_kernel_disabled) {
		LOG_MSG("BUG: Program::SetEnv() called with DOS kernel disabled (such as OS boot).\n");
		return false;
	}

	if (!LocateEnvironmentBlock(env_base,env_fence,psp->GetEnvironment())) {
		LOG_MSG("Warning: SetEnv() was not able to locate the program's environment block\n");
		return false;
	}

	std::string bigentry(entry);
	for (std::string::iterator it = bigentry.begin(); it != bigentry.end(); ++it) *it = toupper(*it);

	el = strlen(bigentry.c_str());
	if (*new_string != 0) nsl = strlen(new_string);
	needs = nsl+1+el+1+1; /* entry + '=' + new_string + '\0' + '\0' */

	/* look for the variable in the block. break the loop if found */
	env_scan = env_base;
	while (env_scan < env_fence) {
		if (mem_readb(env_scan) == 0) break;

		if (EnvPhys_StrCmp(env_scan,env_fence,bigentry.c_str()) == 0) {
			/* found it. remove by shifting the rest of the environment block over */
			int zeroes=0;
			PhysPt s,d;

			/* before we remove it: is there room for the new value? */
			if (nsl != 0) {
				if ((env_scan+needs) > env_fence) {
					LOG_MSG("Program::SetEnv() error, insufficient room for environment variable %s=%s (replacement)\n",bigentry.c_str(),new_string);
					DebugDumpEnv();
					return false;
				}
			}

			s = env_scan; d = env_scan;
			while (s < env_fence && mem_readb(s) != 0) s++;
			if (s < env_fence && mem_readb(s) == 0) s++;

			while (s < env_fence) {
				unsigned char b = mem_readb(s++);

				if (b == 0) zeroes++;
				else zeroes=0;

				mem_writeb(d++,b);
				if (zeroes >= 2) break; /* two consecutive zeros means the end of the block */
			}
		}
		else {
			/* scan to next string */
			if (!EnvPhys_ScanUntilNextString(env_scan,env_fence)) break;
		}
	}

	/* At this point, env_scan points to the first byte beyond the block */
	/* add the string to the end of the block */
	if (*new_string != 0) {
		if ((env_scan+needs) > env_fence) {
			LOG_MSG("Program::SetEnv() error, insufficient room for environment variable %s=%s (addition)\n",bigentry.c_str(),new_string);
			DebugDumpEnv();
			return false;
		}

		assert(env_scan < env_fence);
		for (const char *s=bigentry.c_str();*s != 0;) mem_writeb(env_scan++,(uint8_t)(*s++));
		mem_writeb(env_scan++,'=');

		assert(env_scan < env_fence);
		for (const char *s=new_string;*s != 0;) mem_writeb(env_scan++,(uint8_t)(*s++));
		mem_writeb(env_scan++,0);
		mem_writeb(env_scan++,0);

		assert(env_scan <= env_fence);
	}

	return true;
}

bool MSG_Write(const char *, const char *);

/*! \brief          CONFIG.COM utility to control configuration and files
 *
 *  \description    Utility to write configuration, set configuration,
 *                  and other configuration related functions.
 */
/* kdos: CONFIG, settings and DEVICE loading removed (lines 753-2736 of DOSBox-X programs.cpp) */
void PROGRAMS_DOS_Boot(Section *) {
}

/* FIXME: Rename the function to clarify it does not init programs, it inits the callback mechanism
 *        that program generation on drive Z: needs to tie a .COM executable to a callback */
void PROGRAMS_Init() {
	LOG(LOG_MISC,LOG_DEBUG)("PROGRAMS_Init(): initializing Z: drive .COM stub and program management");

	/* Setup a special callback to start virtual programs */
	call_program=CALLBACK_Allocate();
	CALLBACK_Setup(call_program,&PROGRAMS_Handler,CB_RETF,"internal program");

    /* kdos: no VM events */

	// listconf
	MSG_Add("PROGRAM_CONFIG_NOCONFIGFILE","No config file loaded!\n");
	MSG_Add("PROGRAM_CONFIG_PRIMARY_CONF","Primary config file: \n%s\n");
	MSG_Add("PROGRAM_CONFIG_ADDITIONAL_CONF","Additional config files:\n");
	MSG_Add("PROGRAM_CONFIG_GLOBAL_CONF","\nGlobal config file: \n%s\n");
	MSG_Add("PROGRAM_CONFIG_CONFDIR","DOSBox-X %s user configuration directory: \n%s\n\n");
	MSG_Add("PROGRAM_CONFIG_WORKDIR","DOSBox-X's working directory: \n%s\n\n");
	
	// writeconf
	MSG_Add("PROGRAM_CONFIG_FILE_ERROR","\nCan't open file %s\n");
	MSG_Add("PROGRAM_CONFIG_FILE_WHICH","Writing config file %s\n");
	MSG_Add("PROGRAM_LANGUAGE_FILE_WHICH","Written to language file %s\n");
	
	// help
	MSG_Add("PROGRAM_CONFIG_USAGE","The DOSBox-X command-line configuration utility. Supported options:\n"\
		"-wc (or -writeconf) without parameter: Writes to primary loaded config file.\n"\
		"-wc (or -writeconf) with filename: Writes file to the config directory.\n"\
		"-wl (or -writelang) with filename: Writes the current language strings.\n"\
		"-ln (or -langname) Displays (without arguments) or specifies the language name.\n"\
		"-wcp [filename] Writes file to program directory (dosbox-x.conf or filename).\n"\
		"-wcd Writes to the default config file in the config directory.\n"\
		"-all, -mod Use with -wc, -wcp, or -wcd to write ALL or only modified options.\n"\
		"-wcboot, -wcpboot, or -wcdboot will reboot DOSBox-X after writing the file.\n"\
		"-bootconf (or -bc) reboots with specified config file (or primary loaded file).\n"\
		"-norem Use this with -wc, -wcp, or -wcd to not write config option remarks.\n"\
		"-l Lists DOSBox-X's configuration parameters.\n"\
		"-h, -help, -? Shows this help; add the word \"sections\" for a list of sections.\n"\
		"-h, -help, -? section / property Shows info on a specific section or property.\n"\
		"-axclear Clears the [autoexec] section.\n"\
		"-axadd [line] Adds a line to the [autoexec] section.\n"\
		"-axtype Prints the content of the [autoexec] section.\n"\
		"-avistart, -avistop Starts or stops AVI recording.\n"\
		"-securemode Enables secure mode where features like mounting will be disabled.\n"\
		"-startmapper Starts the DOSBox-X mapper editor.\n"\
		"-gui Starts the graphical configuration tool.\n"
		"-get \"section property\" returns the value of the property (also to %%CONFIG%%).\n"\
		"-set (-setf for force) \"section property=value\" sets the value of the property.\n");
	MSG_Add("PROGRAM_CONFIG_HLP_PROPHLP","Purpose of property \"%s\" (contained in section \"%s\"):\n%s\n\nPossible Values: %s\nDefault value: %s\nCurrent value: %s\n");
	MSG_Add("PROGRAM_CONFIG_HLP_LINEHLP","Purpose of section \"%s\":\n%s\nCurrent value:\n%s\n");
	MSG_Add("PROGRAM_CONFIG_HLP_NOCHANGE","This property cannot be changed at runtime.\n");
	MSG_Add("PROGRAM_CONFIG_APPLY_RESTART","Do you want to restart now to apply the setting [Y/N]?");
	MSG_Add("PROGRAM_CONFIG_HLP_POSINT","positive integer");
	MSG_Add("PROGRAM_CONFIG_HLP_SECTHLP","Section %s contains the following properties:\n");
	MSG_Add("PROGRAM_CONFIG_HLP_SECTLIST","DOSBox-X configuration contains the following sections:\n\n");

	MSG_Add("PROGRAM_CONFIG_SECURE_ON","Switched to secure mode.\n");
	MSG_Add("PROGRAM_CONFIG_SECURE_DISALLOW","This operation is not permitted in secure mode.\n");
	MSG_Add("PROGRAM_CONFIG_SECTION_ERROR","Section %s doesn't exist.\n");
	MSG_Add("PROGRAM_CONFIG_VALUE_ERROR","\"%s\" is not a valid value for property %s.\n");
	MSG_Add("PROGRAM_CONFIG_PROPERTY_ERROR","No such section or property.\n");
	MSG_Add("PROGRAM_CONFIG_PROPERTY_DUPLICATE","There may be other sections with the same property name.\n");
	MSG_Add("PROGRAM_CONFIG_NO_PROPERTY","There is no property %s in section %s.\n");
	MSG_Add("PROGRAM_CONFIG_SET_SYNTAX","Correct syntax: config -set \"section property=value\".\n");
	MSG_Add("PROGRAM_CONFIG_GET_SYNTAX","Correct syntax: config -get \"section property\".\n");
	MSG_Add("PROGRAM_CONFIG_PRINT_STARTUP","\nDOSBox-X was started with the following command line parameters:\n%s\n");
	MSG_Add("PROGRAM_CONFIG_MISSINGPARAM","Missing parameter.\n");
}
