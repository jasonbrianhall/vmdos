/* kdos: DOSBox-X int10.h, the parts its DOS and shell use; the INT 10h
   functions are answered by vmdos's own video BIOS (glue.cpp) */
#pragma once
#include "kdos_stubs.h"
enum VGAModes { M_CGA2, M_CGA4, M_EGA, M_VGA, M_LIN4, M_LIN8, M_LIN15, M_LIN16, M_LIN24, M_LIN32, M_TEXT, M_HERC_GFX, M_HERC_TEXT, M_CGA16, M_TANDY2, M_TANDY4, M_TANDY16, M_TANDY_TEXT, M_AMSTRAD, M_PC98, M_FM_TOWNS, M_PACKED4, M_DCGA, M_ERROR };
#define BIOSMEM_SEG		0x40u

#define BIOSMEM_INITIAL_MODE  0x10u
#define BIOSMEM_CURRENT_MODE  0x49u
#define BIOSMEM_NB_COLS       0x4Au
#define BIOSMEM_PAGE_SIZE     0x4Cu
#define BIOSMEM_CURRENT_START 0x4Eu
#define BIOSMEM_CURSOR_POS    0x50u
#define BIOSMEM_CURSOR_TYPE   0x60u
#define BIOSMEM_CURRENT_PAGE  0x62u
#define BIOSMEM_CRTC_ADDRESS  0x63u
#define BIOSMEM_CURRENT_MSR   0x65u
#define BIOSMEM_CURRENT_PAL   0x66u
#define BIOSMEM_NB_ROWS       0x84u
#define BIOSMEM_CHAR_HEIGHT   0x85u
#define BIOSMEM_VIDEO_CTL     0x87u
#define BIOSMEM_SWITCHES      0x88u
#define BIOSMEM_MODESET_CTL   0x89u
#define BIOSMEM_DCC_INDEX     0x8Au
#define BIOSMEM_CRTCPU_PAGE   0x8Au
#define BIOSMEM_VS_POINTER    0xA8u
#define VGAMEM_GRAPH 0xA000u
#define VGAMEM_CTEXT 0xB800u
#define VGAMEM_MTEXT 0xB000u

/* FIXME: Wait, what?? What the hell kind of preprocessor macro is this??? Kill these macros! --J.C. */
#define BIOS_NCOLS uint16_t ncols=IS_PC98_ARCH ? 80 : real_readw(BIOSMEM_SEG,BIOSMEM_NB_COLS);
#define BIOS_NROWS uint16_t nrows=IS_PC98_ARCH ? (uint16_t)(real_readb(0x60,0x112)+1u) : IS_EGAVGA_ARCH?((uint16_t)real_readb(BIOSMEM_SEG,BIOSMEM_NB_ROWS)+1):25;
#define BIOS_CHEIGHT uint8_t cheight=IS_EGAVGA_ARCH?real_readb(BIOSMEM_SEG,BIOSMEM_CHAR_HEIGHT):8;


struct VideoModeBlock {
	uint16_t	mode;
	VGAModes	type;
	Bitu	swidth, sheight;
	Bitu	twidth, theight;
	Bitu	cwidth, cheight;
	Bitu	ptotal,pstart,plength;

	Bitu	htotal,vtotal;
	Bitu	hdispend,vdispend;
	Bitu	special;
	Bitu	pitch; /* bytes/scanline to use instead of normal calculation */
	
};
extern VideoModeBlock ModeList_VGA[];
extern VideoModeBlock * CurMode;

static inline uint8_t CURSOR_POS_COL(uint8_t page) {
    if (IS_PC98_ARCH)
        return real_readb(0x60,0x11C); /* MS-DOS kernel location */
    else
    	return real_readb(BIOSMEM_SEG,BIOSMEM_CURSOR_POS+page*2u);
}
static inline uint8_t CURSOR_POS_ROW(uint8_t page) {
    if (IS_PC98_ARCH)
        return real_readb(0x60,0x110); /* MS-DOS kernel location */
    else
        return real_readb(BIOSMEM_SEG,BIOSMEM_CURSOR_POS+page*2u+1u);
}
bool INT10_GetInsertState();
bool INT10_SetVideoMode(uint16_t mode);
void INT10_ScrollWindow(uint8_t rul,uint8_t cul,uint8_t rlr,uint8_t clr,int8_t nlines,uint8_t attr,uint8_t page);
void INT10_SetActivePage(uint8_t page);
bool INT10_SetCurMode(void);
void INT10_DisplayCombinationCode(uint16_t * dcc,bool set);
void INT10_GetFuncStateInformation(PhysPt save);
void INT10_SetCursorShape(uint8_t first,uint8_t last);
void INT10_GetScreenColumns(uint16_t* cols);
void INT10_GetCursorPos(uint8_t *row, uint8_t *col, uint8_t page);
void INT10_SetCursorPos(uint8_t row,uint8_t col,uint8_t page);
void INT10_TeletypeOutput(uint8_t chr,uint8_t attr);
void INT10_TeletypeOutputAttr(uint8_t chr,uint8_t attr,bool useattr);
void INT10_ReadCharAttr(uint16_t * result,uint8_t page);
void INT10_WriteChar(uint16_t chr,uint8_t attr,uint8_t page,uint16_t count,bool showattr);
void INT10_WriteString(uint8_t row,uint8_t col,uint8_t flag,uint8_t attr,PhysPt string,uint16_t count,uint8_t page);
void INT10_PutPixel(uint16_t x,uint16_t y,uint8_t page,uint8_t color);
void INT10_GetPixel(uint16_t x,uint16_t y,uint8_t page,uint8_t * color);
void INT10_LoadFont(PhysPt font,bool reload,uint16_t count,Bitu offset,Bitu map,uint8_t height);
void INT10_ReloadFont(void);
void INT10_SetBackgroundBorder(uint8_t val);
void INT10_SetColorSelect(uint8_t val);
void INT10_SetSinglePaletteRegister(uint8_t reg,uint8_t val);
void INT10_SetOverscanBorderColor(uint8_t val);
void INT10_SetAllPaletteRegisters(PhysPt data);
void INT10_ToggleBlinkingBit(uint8_t state);
void INT10_GetSinglePaletteRegister(uint8_t reg,uint8_t * val);
void INT10_GetOverscanBorderColor(uint8_t * val);
void INT10_GetAllPaletteRegisters(PhysPt data);
void INT10_SetSingleDACRegister(uint8_t index,uint8_t red,uint8_t green,uint8_t blue);
void INT10_GetSingleDACRegister(uint8_t index,uint8_t * red,uint8_t * green,uint8_t * blue);
void INT10_SetDACBlock(uint16_t index,uint16_t count,PhysPt data);
void INT10_GetDACBlock(uint16_t index,uint16_t count,PhysPt data);
void INT10_SelectDACPage(uint8_t function,uint8_t mode);
void INT10_GetDACPage(uint8_t* mode,uint8_t* page);
void INT10_SetPelMask(uint8_t mask);
void INT10_GetPelMask(uint8_t & mask);
void INT10_PerformGrayScaleSumming(uint16_t start_reg,uint16_t count);
void INT10_SetupRomMemory(void);
void INT10_SetupRomMemoryChecksum(void);
void INT10_SetupVESA(void);
uint16_t INT10_SetupVideoParameterTable(PhysPt basepos);
void INT10_SetupBasicVideoParameterTable(void);
