/****************************************************************************
 * Snes9x Nintendo Wii/Gamecube Port
 *
 * softdev July 2006
 * crunchy2 May 2007-July 2007
 * Michniewski 2008
 * Tantric 2008-2023
 *
 * snes9xgx.cpp
 *
 * This file controls overall program flow. Most things start and end here!
 ***************************************************************************/

#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <ogcsys.h>
#include <unistd.h>
#include <fat.h>
#include <debug.h>
#include <sys/iosupport.h>

#ifdef HW_RVL
#include <di/di.h>
#include <wiiuse/wpad.h>
#endif
#ifdef USE_VM
	#include "vmalloc.h"
#endif

#include "snes9xgx.h"
#include "networkop.h"
#include "video.h"
#include "audio.h"
#include "menu.h"
#include "sram.h"
#include "freeze.h"
#include "preferences.h"
#include "button_mapping.h"
#include "fileop.h"
#include "filebrowser.h"
#include "input.h"
#include "mem2.h"
#include "utils/wiidrc.h"
#include "utils/FreeTypeGX.h"

#include "snes9x/snes9x.h"
#include "snes9x/fxemu.h"
#include "snes9x/memmap.h"
#include "snes9x/apu/apu.h"
#include "snes9x/controls.h"

int ScreenshotRequested = 0;
int ConfigRequested = 0;
int ShutdownRequested = 0;
int ResetRequested = 0;
int ExitRequested = 0;
bool isWiiVC = false;
char appPath[1024] = { 0 };
static int currentMode;
bool firstRun = true;

extern "C" {
#ifdef USE_VM
	#include "utils/vm/vm.h"
#endif
extern char* strcasestr(const char *, const char *);
extern void __exception_setreload(int t);
}

extern void S9xInitSync();
extern uint32 prevRenderedFrameCount;

/****************************************************************************
 * Shutdown / Reboot / Exit
 ***************************************************************************/

void ExitCleanup()
{
	ShutdownAudio();
	StopGX();

	HaltDeviceThread();
	UnmountAllFAT();

#ifdef HW_RVL
	DI_Close();
#endif
}

#ifdef HW_DOL
	#define PSOSDLOADID 0x7c6000a6
	int *psoid = (int *) 0x80001800;
	void (*PSOReload) () = (void (*)()) 0x80001800;
#endif

void ExitApp()
{
#ifdef HW_RVL
	ShutoffRumble();
#endif

	SavePrefs(SILENT);

	if (SNESROMSize > 0 && !ConfigRequested && GCSettings.AutoSave == 1)
		SaveSRAMAuto(SILENT);

	ExitCleanup();

	if(ShutdownRequested) {
		SYS_ResetSystem(SYS_POWEROFF_STANDBY, 0, 0);
	}
	else if(GCSettings.AutoloadGame) {
		if( !!*(u32*)0x80001800 )
		{
			// Were we launched via HBC? (or via WiiFlow's stub replacement)
			exit(1);
		}
		else
		{
			// Wii channel support
			SYS_ResetSystem( SYS_RETURNTOMENU, 0, 0 );
		}
	}
	else {
		#ifdef HW_RVL
		if(GCSettings.ExitAction == 0) // Auto
		{
			char * sig = (char *)0x80001804;
			if(
				sig[0] == 'S' &&
				sig[1] == 'T' &&
				sig[2] == 'U' &&
				sig[3] == 'B' &&
				sig[4] == 'H' &&
				sig[5] == 'A' &&
				sig[6] == 'X' &&
				sig[7] == 'X')
				GCSettings.ExitAction = 3; // Exit to HBC
			else
				GCSettings.ExitAction = 1; // HBC not found
		}
		#endif

		if(GCSettings.ExitAction == 1) // Exit to Menu
		{
			#ifdef HW_RVL
				SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
			#else
				#define SOFTRESET_ADR ((volatile u32*)0xCC003024)
				*SOFTRESET_ADR = 0x00000000;
			#endif
		}
		else if(GCSettings.ExitAction == 2) // Shutdown Wii
		{
			SYS_ResetSystem(SYS_POWEROFF_STANDBY, 0, 0);
		}
		else // Exit to Loader
		{
			#ifdef HW_RVL
				exit(0);
			#else
				if (psoid[0] == PSOSDLOADID)
					PSOReload();
			#endif
		}
	}
}

#ifdef HW_RVL
void ShutdownCB()
{
	ShutdownRequested = 1;
}
void ResetCB()
{
	ResetRequested = 1;
}
#endif

#ifdef HW_DOL
/****************************************************************************
 * ipl_set_config
 * lowlevel Qoob Modchip disable
 ***************************************************************************/

void ipl_set_config(unsigned char c)
{
	volatile unsigned long* exi = (volatile unsigned long*)0xCC006800;
	unsigned long val,addr;
	addr=0xc0000000;
	val = c << 24;
	exi[0] = ((((exi[0]) & 0x405) | 256) | 48);	//select IPL
	//write addr of IPL
	exi[0 * 5 + 4] = addr;
	exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
	while (exi[0 * 5 + 3] & 1);
	//write the ipl we want to send
	exi[0 * 5 + 4] = val;
	exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
	while (exi[0 * 5 + 3] & 1);

	exi[0] &= 0x405;	//deselect IPL
}
#endif

/****************************************************************************
 * setFrameTimerMethod()
 * change frametimer method depending on whether ROM is NTSC or PAL
 ***************************************************************************/

void setFrameTimerMethod()
{
	/*
	Set frametimer method
	(timerstyle: 0=NTSC vblank, 1=PAL int timer)
	*/
	if ( Settings.PAL ) {
		if(vmode_60hz)
			timerstyle = 1;
		else
			timerstyle = 0;
	} else {
		if(vmode_60hz)
			timerstyle = 0;
		else
			timerstyle = 1;
	}
	return;
}

/****************************************************************************
 * IOS Check
 ***************************************************************************/
#ifdef HW_RVL
bool SupportedIOS(u32 ios)
{
	if(ios == 58 || ios == 61)
		return true;

	return false;
}

bool SaneIOS(u32 ios)
{
	bool res = false;
	u32 num_titles=0;
	u32 tmd_size;

	if(ios > 200)
		return false;

	if (ES_GetNumTitles(&num_titles) < 0)
		return false;

	if(num_titles < 1) 
		return false;

	u64 *titles = (u64 *)memalign(32, num_titles * sizeof(u64) + 32);
	
	if(!titles)
		return false;

	if (ES_GetTitles(titles, num_titles) < 0)
	{
		free(titles);
		return false;
	}
	
	u32 *tmdbuffer = (u32 *)memalign(32, MAX_SIGNED_TMD_SIZE);

	if(!tmdbuffer)
	{
		free(titles);
		return false;
	}

	for(u32 n=0; n < num_titles; n++)
	{
		if((titles[n] & 0xFFFFFFFF) != ios) 
			continue;

		if (ES_GetStoredTMDSize(titles[n], &tmd_size) < 0)
			break;

		if (tmd_size > 4096)
			break;

		if (ES_GetStoredTMD(titles[n], (signed_blob *)tmdbuffer, tmd_size) < 0)
			break;

		if (tmdbuffer[1] || tmdbuffer[2])
		{
			res = true;
			break;
		}
	}
	free(tmdbuffer);
    free(titles);
	return res;
}
#endif
/****************************************************************************
 * USB Gecko Debugging
 ***************************************************************************/

static bool gecko = false;
static mutex_t gecko_mutex = 0;

static ssize_t __out_write(struct _reent *r, void* fd, const char *ptr, size_t len)
{
	if (!gecko || len == 0)
		return len;
	
	if(!ptr || len < 0)
		return -1;

	u32 level;
	LWP_MutexLock(gecko_mutex);
	level = IRQ_Disable();
	usb_sendbuffer(1, ptr, len);
	IRQ_Restore(level);
	LWP_MutexUnlock(gecko_mutex);
	return len;
}

const devoptab_t gecko_out = {
	"stdout",	// device name
	0,			// size of file structure
	NULL,		// device open
	NULL,		// device close
	__out_write,// device write
	NULL,		// device read
	NULL,		// device seek
	NULL,		// device fstat
	NULL,		// device stat
	NULL,		// device link
	NULL,		// device unlink
	NULL,		// device chdir
	NULL,		// device rename
	NULL,		// device mkdir
	0,			// dirStateSize
	NULL,		// device diropen_r
	NULL,		// device dirreset_r
	NULL,		// device dirnext_r
	NULL,		// device dirclose_r
	NULL		// device statvfs_r
};

static void USBGeckoOutput()
{
	gecko = usb_isgeckoalive(1);
	LWP_MutexInit(&gecko_mutex, false);

	devoptab_list[STD_OUT] = &gecko_out;
	devoptab_list[STD_ERR] = &gecko_out;
}

extern "C" { 
	s32 __STM_Close();
	s32 __STM_Init();
}

void InitializeSnes9x() {
	S9xUnmapAllControls ();
	SetDefaultButtonMap ();

	// Allocate SNES Memory
	if (!Memory.Init ())
		ExitApp();

	// Allocate APU
	if (!S9xInitAPU ())
		ExitApp();

	S9xInitSound (64, 0); // Initialise Sound System

	// Initialise Graphics
	setGFX ();
	if (!S9xGraphicsInit ())
		ExitApp();

	AllocGfxMem();
}

int main(int argc, char *argv[])
{
	#ifdef USE_VM
	VM_Init(ARAM_SIZE, MRAM_BACKING);		// Setup Virtual Memory with the entire ARAM
	#endif
	
	#ifdef HW_RVL
	L2Enhance();
	
	u32 ios = IOS_GetVersion();

	if(!SupportedIOS(ios))
	{
		s32 preferred = IOS_GetPreferredVersion();

		if(SupportedIOS(preferred))
			IOS_ReloadIOS(preferred);
	}
	#else
	ipl_set_config(6); // disable Qoob modchip
	#endif
	
	USBGeckoOutput();
	__exception_setreload(8);

	DefaultSettings (); // Set defaults
	InitGCVideo(); // Initialise video
	InitializeSnes9x();
	ResetVideo_Menu (); // change to menu video mode
	
	#ifdef HW_RVL
	// Wii Power/Reset buttons
	__STM_Close();
	__STM_Init();
	__STM_Close();
	__STM_Init();
	SYS_SetPowerCallback(ShutdownCB);
	SYS_SetResetCallback(ResetCB);
	
	WiiDRC_Init();
	isWiiVC = WiiDRC_Inited();
	WPAD_Init();
	WPAD_SetPowerButtonCallback((WPADShutdownCallback)ShutdownCB);
	DI_Init();
	USBStorage_Initialize();
	#else
	DVD_Init (); // Initialize DVD subsystem (GameCube only)
	#endif
	
	SetupPads();
	InitDeviceThread();
	MountAllFAT(); // Initialize libFAT for SD and USB
	
	#ifdef HW_RVL
	// store path app was loaded from
	if(argc > 0 && argv[0] != NULL)
		CreateAppPath(argv[0]);
	
	InitMem2Manager();
	#endif

	S9xInitSync(); // initialize frame sync
	InitFreeType((u8*)font_ttf, font_ttf_size); // Initialize font system
#ifdef HW_RVL
	savebuffer = (unsigned char *)mem2_malloc(SAVEBUFFERSIZE);
	browserList = (BROWSERENTRY *)mem2_malloc(sizeof(BROWSERENTRY)*MAX_BROWSER_SIZE);
#else
	savebuffer = (unsigned char *)memalign(32,SAVEBUFFERSIZE);
#ifdef USE_VM
	browserList = (BROWSERENTRY *)vm_malloc(sizeof(BROWSERENTRY)*MAX_BROWSER_SIZE);
#else
	browserList = (BROWSERENTRY *)memalign(32,sizeof(BROWSERENTRY)*MAX_BROWSER_SIZE);
#endif
#endif
	InitGUIThreads();

	bool autoboot = false;
	if(argc > 2 && argv[1] != NULL && argv[2] != NULL) {
		LoadPrefs();
		if(strcasestr(argv[1], "sd:/") != NULL)
		{
			GCSettings.SaveMethod = DEVICE_SD;
			GCSettings.LoadMethod = DEVICE_SD;
		}
		else
		{
			GCSettings.SaveMethod = DEVICE_USB;
			GCSettings.LoadMethod = DEVICE_USB;
		}
		SavePrefs(SILENT);

		GCSettings.AutoloadGame = AutoloadGame(argv[1], argv[2]);
		autoboot = GCSettings.AutoloadGame;
	}

	while (1) // main loop
	{
		if(!autoboot) {
			// go back to checking if devices were inserted/removed
			// since we're entering the menu
			ResumeDeviceThread();

			SwitchAudioMode(1);

			if(SNESROMSize == 0)
				MainMenu(MENU_GAMESELECTION);
			else
				MainMenu(MENU_GAME);
		}

#ifdef HW_RVL
		SelectFilterMethod();
#endif
		if (firstRun)
		{
			firstRun = false;
			switch (GCSettings.sfxOverclock)
			{
				case 0: Settings.SuperFXSpeedPerLine = 5823405; break;
				case 1: Settings.SuperFXSpeedPerLine = 0.417 * 20.5e6; break;
				case 2: Settings.SuperFXSpeedPerLine = 0.417 * 40.5e6; break;
				case 3: Settings.SuperFXSpeedPerLine = 0.417 * 60.5e6; break;
				case 4: Settings.SuperFXSpeedPerLine = 0.417 * 80.5e6; break;
				case 5: Settings.SuperFXSpeedPerLine = 0.417 * 100.5e6; break;
				case 6: Settings.SuperFXSpeedPerLine = 0.417 * 120.5e6; break;
			}

			if (GCSettings.sfxOverclock > 0)
			S9xResetSuperFX();
			S9xReset();

			switch (GCSettings.Interpolation)
			{
			case 0: Settings.InterpolationMethod = DSP_INTERPOLATION_GAUSSIAN; break;
			case 1: Settings.InterpolationMethod = DSP_INTERPOLATION_LINEAR; break;
			case 2: Settings.InterpolationMethod = DSP_INTERPOLATION_CUBIC; break;
			case 3: Settings.InterpolationMethod = DSP_INTERPOLATION_SINC; break;
			case 4: Settings.InterpolationMethod = DSP_INTERPOLATION_NONE; break;
			}
		}
		
		autoboot = false;		
		ConfigRequested = 0;
		ScreenshotRequested = 0;
		SwitchAudioMode(0);

		Settings.Mute = GCSettings.MuteAudio;
		Settings.SupportHiRes = (GCSettings.HiResolution == 1);
		Settings.MaxSpriteTilesPerLine = (GCSettings.SpriteLimit ? 34 : 128);
		Settings.SkipFrames = (GCSettings.FrameSkip ? AUTO_FRAMERATE : 0);
		Settings.AutoDisplayMessages = (Settings.DisplayFrameRate || Settings.DisplayTime ? true : false);
		Settings.MultiPlayer5Master = (GCSettings.Controller == CTRL_PAD4 ? true : false);
		Settings.SuperScopeMaster = (GCSettings.Controller == CTRL_SCOPE ? true : false);
		Settings.MouseMaster = (GCSettings.Controller == CTRL_MOUSE || GCSettings.Controller == CTRL_MOUSE_PORT2 || GCSettings.Controller == CTRL_MOUSE_BOTH_PORTS);
		Settings.JustifierMaster = (GCSettings.Controller == CTRL_JUST ? true : false);
		SetControllers ();

		// stop checking if devices were removed/inserted
		// since we're starting emulation again
		HaltDeviceThread();

		AudioStart ();

		FrameTimer = 0;
		setFrameTimerMethod (); // set frametimer method every time a ROM is loaded

		CheckVideo = 2;		// force video update
		prevRenderedFrameCount = IPPU.RenderedFramesCount;
		currentMode = GCSettings.render;

		while(1) // emulation loop
		{
			S9xMainLoop ();
			ReportButtons ();

			if(ResetRequested)
			{
				S9xSoftReset (); // reset game
				ResetRequested = 0;
			}
			if (ConfigRequested)
			{
				ConfigRequested = 0;
				ResetVideo_Menu();
				break;
			}
			#ifdef HW_RVL
			if(ShutdownRequested)
				ExitApp();
			#endif
		} // emulation loop
	} // main loop
}
