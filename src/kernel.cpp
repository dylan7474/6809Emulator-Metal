//
// kernel.cpp - 6809Emulator-Metal: bare-metal Dragon 32 on a Raspberry Pi
//
// The Circle side: screen, USB keyboards, SD card, 50 Hz timing.
// The Dragon itself lives behind core/dragon.h; the start-up, logging and
// USB code follow ClassicPi-Metal, where they were proven on real boards.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "kernel.h"
#include "core/dragon.h"
#include "keymap_usb.h"

#include <circle/bcmframebuffer.h>
#include <circle/machineinfo.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/net/dnsclient.h>
#include <circle/net/ntpclient.h>
#include <circle/net/ipaddress.h>
#include <circle/netdevice.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <stdlib.h>
#include <time.h>
#if RASPPI <= 4
#include <circle/sound/hdmisoundbasedevice.h>
#endif
#include <circle_glue.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VERSION		"0.1.0"

#define FRAME_US	20000		// 50 Hz, like a UK Dragon
#define STATS_US	10000000	// speed line in the log every 10 s
#define SPEED_FILE_AT	6		// write the speed file after the 6th (60 s)

#define SOUND_RATE	48000
#define SOUND_QUEUE_MS	200		// room in the HDMI sound queue
#define SOUND_FRAME	(SOUND_RATE / 50)	// 960 samples per 50 Hz frame
#define SOUND_LOW	(2 * SOUND_FRAME)	// below this: write a frame twice
#define SOUND_HIGH	(6 * SOUND_FRAME)	// above this: skip a frame

static const char *const ROMNames[] = { "SD:/d32.rom", "SD:/dragon32.rom", "SD:/dragon.rom" };

CKernel *CKernel::s_pThis = 0;

static u8 s_Picture[VDG_HEIGHT][VDG_WIDTH];	// this frame, palette indices
static u8 s_Shadow[VDG_HEIGHT][VDG_WIDTH];	// last frame drawn

// ---- Key queue, filled in interrupt context --------------------------------

struct TKeyEvent { u8 Usage; u8 Down; };

static TKeyEvent s_KeyQueue[64];
static volatile unsigned s_nKeyIn, s_nKeyOut;
static volatile unsigned s_nKeyReports;		// USB keyboard reports received (debug)
static volatile unsigned s_nKeyDrops;		// key events lost: queue full (debug)
static bool s_Held[256];			// main loop only
static unsigned s_nFrame;			// frames run so far

static void QueueKey (u8 Usage, bool bDown)
{
	unsigned nNext = (s_nKeyIn + 1) % (sizeof s_KeyQueue / sizeof s_KeyQueue[0]);
	if (nNext == s_nKeyOut)
	{
		s_nKeyDrops++;
		return;		// full: drop
	}
	s_KeyQueue[s_nKeyIn].Usage = Usage;
	s_KeyQueue[s_nKeyIn].Down = bDown;
	s_nKeyIn = nNext;
}

// ---- Boot log -------------------------------------------------------------
// Everything said during startup, Circle's own log lines and the speed lines,
// kept in RAM and written to SD:/bootlog-<board>.txt at each stage.

#define BOOTLOG_SIZE	65536

static char s_BootLog[BOOTLOG_SIZE];
static volatile unsigned s_nBootLog;

static void BootLogAppend (const char *p, size_t n)
{
	EnterCritical ();
	if (s_nBootLog + n > BOOTLOG_SIZE - 1)
	{
		n = BOOTLOG_SIZE - 1 - s_nBootLog;	// full: keep the start
	}
	memcpy (s_BootLog + s_nBootLog, p, n);
	s_nBootLog += n;
	LeaveCritical ();
}

int CLogTee::Write (const void *pBuffer, size_t nCount)
{
	BootLogAppend ((const char *) pBuffer, nCount);
	return m_pSerial->Write (pBuffer, nCount);
}

// ---- Kernel ---------------------------------------------------------------

CKernel::CKernel (void)
:	CStdlibAppScreen ("6809Emulator-Metal"),
	m_USBHCI (&mInterrupt, &mTimer, TRUE),
	m_EMMC (&mInterrupt, &mTimer, &mActLED),
	m_SerialConsole (&mSerial, &mSerial),
	m_LogTee (&mSerial),
	m_bSDOK (false),
	m_bScreenOK (false),
	m_bTurbo (false),
	m_bKeyLog (false),
	m_pSound (0),
	m_bAdmin (false),
	m_nAdminScreen (AdminMenu),
	m_nSel (0),
	m_nTop (0),
	m_nAdminScale (2),
	m_bAdminDirty (false),
	m_bAdminClear (false),
	m_bMuted (false),
	m_nMessageUntil (0),
	m_bTapeOpen (false),
	m_bRecOpen (false),
	m_pWLAN (0),
	m_pNet (0),
	m_pWPASupplicant (0),
	m_bNetActive (false),
	m_bTimeDone (false),
	m_nNetStart (0),
	m_bRawKeys (false),
	m_bDebug (false),
	m_nFlushOK (0),
	m_nFlushFail (0),
	m_nLastSaveMs (0),
	m_nUSBLogFlushes (0),
	m_bUSBRemoved (false),
	m_pROM (0),
	m_pFB (0),
	m_nBorder (-1)
{
	s_pThis = this;
	m_Message[0] = '\0';
	m_TapeName[0] = '\0';
	for (unsigned i = 0; i < MAX_KEYBOARDS; i++)
	{
		m_pKeyboards[i] = 0;
	}
}

// Our own start-up order (not CStdlibAppScreen::Initialize, which gives up
// silently if the screen fails): serial and log first, then the SD card so
// a log can be written, then the screen.
bool CKernel::Initialize (void)
{
	if (!CStdlibApp::Initialize ())		// interrupts
	{
		return false;
	}

	mSerial.Initialize (115200);
	mLogger.Initialize (&m_LogTee);		// UART + boot log, never the screen
	mLogger.RegisterPanicHandler (PanicHandler);
	mTimer.Initialize ();

	// Board tag for the file names: "Raspberry Pi Zero 2 W" -> "zero-2-w"
	char Tag[32];
	const char *pName = CMachineInfo::Get ()->GetMachineName ();
	static const char Prefix[] = "Raspberry Pi ";
	if (strncmp (pName, Prefix, sizeof Prefix - 1) == 0)
	{
		pName += sizeof Prefix - 1;
	}
	unsigned n = 0;
	for (; *pName != '\0' && n < sizeof Tag - 1; pName++)
	{
		char c = *pName;
		if (c >= 'A' && c <= 'Z')
		{
			c += 'a' - 'A';
		}
		if (c == ' ' || c == '-')
		{
			c = '-';
		}
		else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+'))
		{
			continue;
		}
		Tag[n++] = c;
	}
	Tag[n] = '\0';
	if (n == 0)
	{
		strcpy (Tag, "unknown");
	}
	snprintf (m_SpeedFile, sizeof m_SpeedFile, "SD:/speed-%s.txt", Tag);
	snprintf (m_BootLogFile, sizeof m_BootLogFile, "SD:/bootlog-%s.txt", Tag);

	// The core's printf goes to the UART, never onto the Dragon's screen
	if (m_SerialConsole.Initialize ())
	{
		CGlueStdioInit (m_SerialConsole);
	}

	m_bSDOK =    m_EMMC.Initialize ()
		  && f_mount (&m_FileSystem, "SD:", 1) == FR_OK;

	// Keep the previous boot's log (after a hang, that is the useful one)
	if (m_bSDOK)
	{
		char Prev[72];
		snprintf (Prev, sizeof Prev, "%.*s-prev.txt",
			  (int) (strlen (m_BootLogFile) - 4), m_BootLogFile);
		f_unlink (Prev);
		f_rename (m_BootLogFile, Prev + 3);	// FatFs: new name without "SD:"
	}

	m_bScreenOK = mScreen.Initialize ();

	return true;
}

static int FormatLine (char *pBuf, size_t nSize, const char *pFormat, va_list Args)
{
	int n = vsnprintf (pBuf, nSize - 2, pFormat, Args);
	if (n < 0)
	{
		return 0;
	}
	if (n > (int) nSize - 3)
	{
		n = nSize - 3;
	}
	pBuf[n++] = '\n';
	pBuf[n] = '\0';
	return n;
}

// Boot log copy gets an uptime stamp; screen and serial get the plain line
static void LogLine (CDevice *pSerial, const char *pLine, int n)
{
	char Stamp[24];
	u64 nMs = CTimer::GetClockTicks64 () / 1000;
	int s = snprintf (Stamp, sizeof Stamp, "[%5u.%03u] ",
			  (unsigned) (nMs / 1000), (unsigned) (nMs % 1000));
	BootLogAppend (Stamp, s);
	BootLogAppend (pLine, n);
	pSerial->Write (pLine, n);
}

void CKernel::Say (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	int n = FormatLine (Buf, sizeof Buf, pFormat, Args);
	va_end (Args);

	if (m_bScreenOK)
	{
		mScreen.Write (Buf, n);
	}
	LogLine (&mSerial, Buf, n);
}

void CKernel::Log (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	int n = FormatLine (Buf, sizeof Buf, pFormat, Args);
	va_end (Args);

	LogLine (&mSerial, Buf, n);
}

// Circle calls this after logging a panic (e.g. a CPU exception), just
// before it halts: the log tail goes on screen (plain memory writes work
// from an exception), then one try at the SD card.
void CKernel::PanicHandler (void)
{
	static bool bOnce;
	if (!bOnce && s_pThis != 0)
	{
		bOnce = true;
		if (s_pThis->m_bScreenOK)
		{
			static const char Head[] = "\x1b[H\x1b[J\x1b[?25h"
				"*** 6809Emulator-Metal crashed. Last lines of the boot log:\n\n";
			s_pThis->mScreen.Write (Head, sizeof Head - 1);
			unsigned nLen = s_nBootLog;
			unsigned nFrom = nLen > 1500 ? nLen - 1500 : 0;
			while (nFrom < nLen && nFrom > 0 && s_BootLog[nFrom - 1] != '\n')
			{
				nFrom++;	// start at a line boundary
			}
			s_pThis->mScreen.Write (s_BootLog + nFrom, nLen - nFrom);
		}
		s_pThis->FlushBootLog ();
	}
}

// The log file stays open: each save appends only what is new and syncs.
// (Rewriting the whole file every time stalled a Zero W for 0.5-1 s per
// save once the log had grown, which made the keyboard feel laggy.)
static FIL s_LogFile;
static bool s_bLogOpen;
static unsigned s_nLogSaved;		// bytes of s_BootLog already in the file

void CKernel::CloseBootLog (void)
{
	if (s_bLogOpen)
	{
		f_close (&s_LogFile);
		s_bLogOpen = false;
	}
}

void CKernel::FlushBootLog (void)
{
	if (!m_bSDOK)
	{
		return;
	}
	u64 nStart = CTimer::GetClockTicks64 ();
	if (!s_bLogOpen)
	{
		if (f_open (&s_LogFile, m_BootLogFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		{
			m_nFlushFail++;
			return;
		}
		s_bLogOpen = true;
		s_nLogSaved = 0;
	}
	unsigned nEnd = s_nBootLog;
	UINT nWritten = 0;
	FRESULT Result = FR_OK;
	if (nEnd > s_nLogSaved)
	{
		Result = f_write (&s_LogFile, s_BootLog + s_nLogSaved, nEnd - s_nLogSaved, &nWritten);
	}
	if (Result == FR_OK && nWritten == nEnd - s_nLogSaved && f_sync (&s_LogFile) == FR_OK)
	{
		s_nLogSaved = nEnd;
		m_nFlushOK++;
	}
	else
	{
		m_nFlushFail++;
	}
	m_nLastSaveMs = (unsigned) ((CTimer::GetClockTicks64 () - nStart) / 1000);
}

void CKernel::LogBoard (void)
{
	CMachineInfo *pInfo = CMachineInfo::Get ();
	Log ("Board:  %s (revision code %08X), %s, %u MB RAM",
	     pInfo->GetMachineName (), (unsigned) pInfo->GetRevisionRaw (),
	     pInfo->GetSoCName (), pInfo->GetRAMSize ());
	Log ("Kernel: %u-bit, Circle RASPPI=%u, built " __DATE__ " " __TIME__,
	     (unsigned) (sizeof (void *) * 8), (unsigned) RASPPI);
#ifdef USE_USB_FIQ
	Log ("USB:    FIQ on (USE_USB_FIQ)");
#endif
	Log ("SD card: %s", m_bSDOK ? "OK" : "FAILED");
	if (m_bScreenOK)
	{
		CBcmFrameBuffer *pFB = mScreen.GetFrameBuffer ();
		Log ("Screen: OK, %ux%u, %u bpp, pitch %u",
		     pFB->GetWidth (), pFB->GetHeight (), pFB->GetDepth (), pFB->GetPitch ());
	}
	else
	{
		Log ("Screen: FAILED to start (no HDMI picture); the Dragon runs anyway");
	}
}

void CKernel::Fatal (const char *pFormat, ...)
{
	char Buf[256];
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (Buf, sizeof Buf, pFormat, Args);
	va_end (Args);

	Say ("");
	Say ("*** %s", Buf);
	Say ("");
	Say ("Fix the SD card on a PC and power the Pi off and on again.");
	FlushBootLog ();

	while (1)
	{
		mActLED.On ();
		CTimer::SimpleMsDelay (200);
		mActLED.Off ();
		CTimer::SimpleMsDelay (800);
	}
}

bool CKernel::LoadROM (void)
{
	static u8 ROM[DRAGON_ROM_SIZE] __attribute__((aligned(64)));

	for (const char *pName : ROMNames)
	{
		FIL File;
		if (f_open (&File, pName, FA_READ) != FR_OK)
		{
			continue;
		}

		FSIZE_t nSize = f_size (&File);
		if (nSize != DRAGON_ROM_SIZE)
		{
			f_close (&File);
			Fatal ("%s is %u bytes; a Dragon 32 ROM is 16384 bytes.",
			       pName + 4, (unsigned) nSize);
		}

		UINT nRead;
		FRESULT Result = f_read (&File, ROM, DRAGON_ROM_SIZE, &nRead);
		f_close (&File);
		if (Result != FR_OK || nRead != DRAGON_ROM_SIZE)
		{
			Fatal ("Cannot read %s (FatFs error %d).", pName + 4, (int) Result);
		}

		// Sanity check: the reset vector ($BFFE) points into the ROM
		unsigned nReset = (unsigned) ROM[0x3FFE] << 8 | ROM[0x3FFF];
		if (nReset < 0x8000 || nReset > 0xBFFF)
		{
			Fatal ("%s does not look like a Dragon 32 ROM (reset vector %04X).",
			       pName + 4, nReset);
		}

		Say ("ROM:    %s (reset vector %04X)", pName + 4, nReset);
		m_pROM = ROM;
		return true;
	}

	Fatal ("No Dragon ROM found. Put your 16 KB Dragon 32 ROM in the root\n"
	       "    of the SD card, named d32.rom.");
	return false;
}

void CKernel::WriteSpeedFile (unsigned nFrameUs)
{
	char Text[512];
	int n = snprintf (Text, sizeof Text,
		"6809Emulator-Metal %s\r\n"
		"Board:   %s\r\n"
		"Kernel:  %u-bit, Circle RASPPI=%u\r\n"
		"Screen:  %ux%u, %u bpp, Dragon at %ux\r\n"
		"Frame:   %u.%02u ms of CPU time per 20 ms frame (%u Dragon cycles)\r\n"
		"Headroom: %u.%02ux (how much faster than needed)\r\n"
		"\r\n"
		"Written once per boot, 60 s after the Dragon starts.\r\n",
		VERSION, CMachineInfo::Get ()->GetMachineName (),
		(unsigned) (sizeof (void *) * 8), (unsigned) RASPPI,
		m_nFBWidth, m_nFBHeight, m_nFBDepth, m_nScale,
		nFrameUs / 1000, nFrameUs % 1000 / 10, (unsigned) DRAGON_FRAME_CYCLES,
		FRAME_US / (nFrameUs ? nFrameUs : 1),
		(unsigned) ((u64) FRAME_US * 100 / (nFrameUs ? nFrameUs : 1) % 100));
	if (n <= 0)
	{
		return;
	}
	if (n >= (int) sizeof Text)
	{
		n = sizeof Text - 1;
	}

	FIL File;
	if (f_open (&File, m_SpeedFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		Log ("cannot write %s", m_SpeedFile);
		return;
	}
	UINT nWritten;
	f_write (&File, Text, n, &nWritten);
	f_close (&File);
	Log ("wrote %s", m_SpeedFile);
}

// ---- Wi-Fi + internet time ------------------------------------------------------
// The Dragon has no clock, but the files written to the SD card (tapes,
// logs) get their dates from the Pi's clock.  With SD:/wpa_supplicant.conf
// the Pi joins Wi-Fi and asks an NTP server once; the network only gets
// turns in the idle part of each 20 ms frame, so the Dragon never slows.
// From ClassicPi-Metal, where it runs on real Zero W / Zero 2 W boards.

#define WIFI_CONFIG	"SD:/wpa_supplicant.conf"
#define WIFI_FIRMWARE	"SD:/firmware/"
#define HOSTNAME	"dragon"
#define NET_GIVE_UP_US	120000000	// stop the network after 2 min without time

static volatile unsigned s_nNTPTime;		// Unix seconds, 0 = not yet
static volatile bool s_bNTPDone;		// task finished (with or without time)

class CTimeTask : public CTask
{
public:
	CTimeTask (CNetSubSystem *pNet, const char *pServer)
	:	m_pNet (pNet), m_pServer (pServer)
	{
		SetName ("ntp");
	}

	void Run (void) override
	{
		CScheduler *pSched = CScheduler::Get ();
		while (!m_pNet->IsRunning ())		// Wi-Fi joined + DHCP
		{
			pSched->MsSleep (200);
		}
		CString IP;
		m_pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
		CLogger::Get ()->Write ("dragon", LogNotice, "Network: connected, IP address %s",
					(const char *) IP);

		for (unsigned nTry = 0; nTry < 5 && s_nNTPTime == 0; nTry++)
		{
			CIPAddress Server;
			CDNSClient DNS (m_pNet);
			if (DNS.Resolve (m_pServer, &Server))
			{
				CNTPClient NTP (m_pNet);
				s_nNTPTime = NTP.GetTime (Server);
			}
			if (s_nNTPTime == 0)
			{
				pSched->MsSleep (2000);
			}
		}
		s_bNTPDone = true;
	}

private:
	CNetSubSystem *m_pNet;
	const char *m_pServer;
};

void CKernel::StartWiFi (void)
{
	strncpy (m_TimeZone, mOptions.GetAppOptionString ("tz", "GMT0BST,M3.5.0/1,M10.5.0"),
		 sizeof m_TimeZone - 1);
	strncpy (m_NTPServer, mOptions.GetAppOptionString ("ntp", "pool.ntp.org"),
		 sizeof m_NTPServer - 1);
	m_TimeZone[sizeof m_TimeZone - 1] = '\0';
	m_NTPServer[sizeof m_NTPServer - 1] = '\0';
	setenv ("TZ", m_TimeZone, 1);
	tzset ();

	if (strcmp (mOptions.GetAppOptionString ("wifi", "on"), "off") == 0)
	{
		Log ("Network: off (wifi=off in cmdline.txt)");
		return;
	}
	FILINFO Info;
	if (f_stat (WIFI_CONFIG, &Info) != FR_OK)
	{
		Log ("Network: off (no wpa_supplicant.conf; files are dated 1980)");
		return;
	}
	m_pWLAN = new CBcm4343Device (WIFI_FIRMWARE);
	if (m_pWLAN == 0 || !m_pWLAN->Initialize ())
	{
		Log ("Wi-Fi:  no Wi-Fi chip on this board, or its firmware is not in /firmware");
		m_pWLAN = 0;		// a half-started driver is left alone
		return;
	}
	m_pNet = new CNetSubSystem (0, 0, 0, 0, HOSTNAME, NetDeviceTypeWLAN);
	if (m_pNet == 0 || !m_pNet->Initialize (FALSE))
	{
		Log ("Wi-Fi:  network start failed");
		m_pNet = 0;
		return;
	}
	m_pWPASupplicant = new CWPASupplicant (WIFI_CONFIG);
	if (m_pWPASupplicant == 0 || !m_pWPASupplicant->Initialize ())
	{
		Log ("Wi-Fi:  cannot start WPA (check wpa_supplicant.conf)");
		m_pWPASupplicant = 0;
		return;
	}
	new CTimeTask (m_pNet, m_NTPServer);
	m_bNetActive = true;
	m_nNetStart = CTimer::GetClockTicks64 ();
	Say ("Wi-Fi:  joining; the clock is set from %s when it answers", m_NTPServer);
}

static long DaysFromCivil (int y, unsigned m, unsigned d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned) (y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long) doe - 719468;
}

// Main loop, after each frame: once the time is in, set the Pi's clock
// (local time, so FatFs dates files in the UK's own time)
void CKernel::UpdateNetwork (u64 nNow)
{
	if (!m_bNetActive || m_bTimeDone)
	{
		return;
	}
	if (!s_bNTPDone && nNow - m_nNetStart < NET_GIVE_UP_US)
	{
		return;
	}
	m_bTimeDone = true;
	m_bNetActive = false;		// nothing else uses the network yet
	if (s_nNTPTime == 0)
	{
		Log ("Network: no time from %s (not connected, or no answer)", m_NTPServer);
		FlushBootLog ();
		return;
	}
	time_t t = s_nNTPTime;
	struct tm Tm;
	localtime_r (&t, &Tm);
	long nLocal = DaysFromCivil (Tm.tm_year + 1900, Tm.tm_mon + 1, Tm.tm_mday) * 86400L
		    + Tm.tm_hour * 3600L + Tm.tm_min * 60L + Tm.tm_sec;
	mTimer.SetTime ((unsigned) nLocal, TRUE);
	Log ("Clock:  set from %s: %04d-%02d-%02d %02d:%02d:%02d (%s)", m_NTPServer,
	     Tm.tm_year + 1900, Tm.tm_mon + 1, Tm.tm_mday, Tm.tm_hour, Tm.tm_min, Tm.tm_sec,
	     Tm.tm_isdst > 0 ? "summer time" : "winter time");
	FlushBootLog ();
}

bool CKernel::ClockText (char *pBuf, size_t nSize)
{
	unsigned nTime = mTimer.GetLocalTime ();
	if (!m_bTimeDone || nTime < 1000000000)
	{
		return false;
	}
	snprintf (pBuf, nSize, "%02u:%02u:%02u", nTime / 3600 % 24, nTime / 60 % 60, nTime % 60);
	return true;
}

// ---- Sound over HDMI (Pi 1-4 and the Zeros; Circle has none on the Pi 5) ----

void CKernel::StartSound (void)
{
#if RASPPI <= 4
	if (strcmp (mOptions.GetAppOptionString ("sound", "hdmi"), "off") == 0)
	{
		Log ("Sound:  off (sound=off in cmdline.txt)");
		return;
	}
	if (!m_bScreenOK)
	{
		Log ("Sound:  off (HDMI not running)");
		return;
	}
	CHDMISoundBaseDevice *pSound = new CHDMISoundBaseDevice (&mInterrupt, SOUND_RATE, 384 * 10);
	if (pSound == 0 || !pSound->AllocateQueue (SOUND_QUEUE_MS))
	{
		Log ("Sound:  cannot start HDMI sound");
		return;			// left alone: its destructor resets HDMI
	}
	pSound->SetWriteFormat (SoundFormatSigned16, 2);
	if (!pSound->Start ())
	{
		Log ("Sound:  cannot start HDMI sound");
		return;
	}
	m_pSound = pSound;
	dragon_audio_rate (SOUND_RATE);
	Say ("Sound:  HDMI, %u Hz (sound=off in cmdline.txt turns it off)", SOUND_RATE);
#else
	Log ("Sound:  none (Circle has no HDMI sound on the Pi 5)");
#endif
}

// After each Dragon frame: its ~960 samples, as stereo, into the HDMI queue.
// The Pi's 50 Hz and the HDMI clock drift a little apart, so now and then
// a whole frame is played twice or dropped, only when the queue gets near
// empty or full (steering the amount per frame made a warble in
// ClassicPi-Metal).  In turbo the Dragon makes sound too fast: it is
// dropped whenever the queue is full.
void CKernel::FeedSound (void)
{
	static s16 Mono[SOUND_FRAME * 2];
	unsigned n = dragon_audio_take (Mono, sizeof Mono / sizeof Mono[0]);
	if (m_bMuted)
	{
		memset (Mono, 0, n * sizeof Mono[0]);	// keep the queue fed, silently
	}
	if (m_pSound == 0 || n == 0)
	{
		return;
	}
	static s16 Stereo[SOUND_FRAME * 4];
	for (unsigned i = 0; i < n; i++)
	{
		Stereo[2 * i] = Stereo[2 * i + 1] = Mono[i];
	}

	CSoundBaseDevice *pSound = (CSoundBaseDevice *) m_pSound;
	unsigned nQueued = pSound->GetQueueFramesAvail ();	// frames waiting to be played
	if (nQueued > SOUND_HIGH)
	{
		return;				// running ahead: skip this frame
	}
	pSound->Write (Stereo, n * 2 * sizeof (s16));
	if (nQueued < SOUND_LOW)
	{
		pSound->Write (Stereo, n * 2 * sizeof (s16));	// running dry: once more
	}
}

void CKernel::SetupDisplay (void)
{
	if (!m_bScreenOK)
	{
		m_pFB = 0;
		m_nFBWidth = m_nFBHeight = m_nFBPitch = m_nFBDepth = 0;
		m_nScale = m_nOffX = m_nOffY = 0;
		return;
	}

	CBcmFrameBuffer *pFrameBuffer = mScreen.GetFrameBuffer ();
	m_pFB       = (u8 *) (uintptr) pFrameBuffer->GetBuffer ();
	m_nFBWidth  = pFrameBuffer->GetWidth ();
	m_nFBHeight = pFrameBuffer->GetHeight ();
	m_nFBPitch  = pFrameBuffer->GetPitch ();
	m_nFBDepth  = pFrameBuffer->GetDepth ();

	unsigned nScaleX = m_nFBWidth / VDG_WIDTH;
	unsigned nScaleY = m_nFBHeight / VDG_HEIGHT;
	m_nScale = nScaleX < nScaleY ? nScaleX : nScaleY;
	if (m_nScale == 0)
	{
		m_nScale = 1;		// screen smaller than 256x192: top-left only
	}

	// cmdline.txt "scale=N" forces a smaller whole-number scale
	unsigned nWanted = mOptions.GetAppOptionDecimal ("scale", 0);
	if (nWanted >= 1 && nWanted != (unsigned) -1)
	{
		if (nWanted < m_nScale)
		{
			m_nScale = nWanted;
		}
		Log ("Screen: scale=%u in cmdline.txt -> %ux", nWanted, m_nScale);
	}

	unsigned nW = VDG_WIDTH * m_nScale;
	unsigned nH = VDG_HEIGHT * m_nScale;
	m_nOffX = nW < m_nFBWidth  ? ((m_nFBWidth - nW) / 2) & ~1u : 0;	// even: aligned rows
	m_nOffY = nH < m_nFBHeight ? (m_nFBHeight - nH) / 2 : 0;

	for (unsigned i = 0; i < VDG_COLOURS; i++)
	{
		unsigned r = vdg_palette[i][0], g = vdg_palette[i][1], b = vdg_palette[i][2];
		m_Palette[i] = m_nFBDepth == 16 ? ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
						: COLOR32 (r, g, b, 0xFF);
	}

	Say ("Screen: %ux%u, %u bpp, Dragon at %ux scale", m_nFBWidth, m_nFBHeight,
	     m_nFBDepth, m_nScale);
}

// Draw the rows of the VDG picture that changed, scaled, into the framebuffer.
void CKernel::Blit (bool bForce)
{
	if (m_pFB == 0)
	{
		return;		// no screen: the Dragon runs blind (see the boot log)
	}
	vdg_render (dragon_memory (), s_Picture);
	u8 Border = vdg_border (dragon_memory ());
	if (bForce || Border != m_nBorder)
	{
		DrawBorder (Border);
	}

	const unsigned nBytesPP = m_nFBDepth / 8;
	unsigned nRows = VDG_HEIGHT;
	unsigned nCols = VDG_WIDTH;
	if (m_nOffY + nRows * m_nScale > m_nFBHeight)
	{
		nRows = (m_nFBHeight - m_nOffY) / m_nScale;
	}
	if (m_nOffX + nCols * m_nScale > m_nFBWidth)
	{
		nCols = (m_nFBWidth - m_nOffX) / m_nScale;
	}

	for (unsigned y = 0; y < nRows; y++)
	{
		const u8 *pSrc = s_Picture[y];
		if (!bForce && memcmp (pSrc, s_Shadow[y], VDG_WIDTH) == 0)
		{
			continue;
		}
		memcpy (s_Shadow[y], pSrc, VDG_WIDTH);

		// Build the scaled row in ordinary RAM, then copy it to the screen.
		// Never write pixels straight into the framebuffer: at odd scales
		// the compiler packs two 16-bit pixels into one 32-bit store, which
		// lands on a 2-byte boundary every other time, and the Pi 1/Zero's
		// ARM11 faults on misaligned stores to screen memory (found on a
		// Zero W in ClassicPi-Metal, 2026-11-20).  RAM is fine.
		static u32 LineBuf[4096] __attribute__((aligned(64)));	// 16 KB
		unsigned nLineBytes = nCols * m_nScale * nBytesPP;
		if (nLineBytes > sizeof LineBuf)
		{
			continue;	// wider than 4096 px at 32 bpp: not drawn
		}

		if (nBytesPP == 2)
		{
			u16 *pOut = (u16 *) LineBuf;
			for (unsigned x = 0; x < nCols; x++)
			{
				u16 Colour = (u16) m_Palette[pSrc[x]];
				for (unsigned s = 0; s < m_nScale; s++)
				{
					*pOut++ = Colour;
				}
			}
		}
		else if (nBytesPP == 4)
		{
			u32 *pOut = LineBuf;
			for (unsigned x = 0; x < nCols; x++)
			{
				u32 Colour = m_Palette[pSrc[x]];
				for (unsigned s = 0; s < m_nScale; s++)
				{
					*pOut++ = Colour;
				}
			}
		}
		else
		{
			continue;	// 8 bpp not supported (Circle default is 16)
		}

		// One copy per screen row (also the vertical scaling); the row
		// start is 4-byte aligned (pitch and m_nOffX are kept even)
		u8 *pLine = m_pFB + (m_nOffY + y * m_nScale) * m_nFBPitch + m_nOffX * nBytesPP;
		for (unsigned s = 0; s < m_nScale; s++)
		{
			memcpy (pLine + s * m_nFBPitch, LineBuf, nLineBytes);
		}
	}
}

// debug=1: one line of text in the black strip under the Dragon picture
// (white on black, the 6847 font at 2x).  Drawn straight from the main
// loop, so it keeps working when the SD card or USB stop: a photo of the
// screen then shows which part died.
void CKernel::DrawStatus (const char *pText)
{
	const unsigned nScale = 2, nRows = 12 * nScale;
	unsigned nTop = m_nOffY + VDG_HEIGHT * m_nScale + 4;
	if (m_pFB == 0 || nTop + nRows > m_nFBHeight)
	{
		return;		// no room under the picture
	}
	const unsigned nBytesPP = m_nFBDepth / 8;
	unsigned nChars = strlen (pText);
	if (nChars > m_nFBWidth / (8 * nScale))
	{
		nChars = m_nFBWidth / (8 * nScale);
	}
	unsigned nPixels = m_nFBWidth & ~1u;
	static u32 LineBuf[4096] __attribute__((aligned(64)));
	if (nPixels * nBytesPP > sizeof LineBuf || (nBytesPP != 2 && nBytesPP != 4))
	{
		return;
	}
	u32 White = nBytesPP == 2 ? 0xFFFF : COLOR32 (255, 255, 255, 255);
	u32 Black = nBytesPP == 2 ? 0 : COLOR32 (0, 0, 0, 255);
	static u8 Glyph[256][12];		// font rows of each character, looked up once
	for (unsigned c = 0; c < nChars; c++)
	{
		for (unsigned r = 0; r < 12; r++)
		{
			Glyph[c][r] = vdg_font_row (pText[c], r);
		}
	}
	for (unsigned y = 0; y < nRows; y++)
	{
		for (unsigned x = 0; x < nPixels; x++)
		{
			unsigned nChar = x / (8 * nScale);
			u32 Colour = Black;
			if (nChar < nChars)
			{
				u8 Bits = Glyph[nChar][y / nScale];
				Colour = (Bits & (0x80 >> ((x / nScale) & 7))) ? White : Black;
			}
			if (nBytesPP == 2)
			{
				((u16 *) LineBuf)[x] = (u16) Colour;
			}
			else
			{
				LineBuf[x] = Colour;
			}
		}
		memcpy (m_pFB + (nTop + y) * m_nFBPitch, LineBuf, nPixels * nBytesPP);
	}
}

// The margins around the Dragon's picture in its border colour (black
// around text, green or buff around graphics), like the VDG's border on a
// TV.  Drawn only when the colour changes.
void CKernel::DrawBorder (u8 Colour)
{
	m_nBorder = Colour;
	if (m_pFB == 0)
	{
		return;
	}
	const unsigned nBytesPP = m_nFBDepth / 8;
	if (nBytesPP != 2 && nBytesPP != 4)
	{
		return;
	}
	static u32 LineBuf[4096] __attribute__((aligned(64)));
	unsigned nWidth = m_nFBWidth & ~1u;
	if (nWidth * nBytesPP > sizeof LineBuf)
	{
		return;
	}
	u32 Pixel = m_Palette[Colour];
	for (unsigned x = 0; x < nWidth; x++)
	{
		if (nBytesPP == 2) ((u16 *) LineBuf)[x] = (u16) Pixel; else LineBuf[x] = Pixel;
	}
	unsigned nTop = m_nOffY, nBottom = m_nOffY + VDG_HEIGHT * m_nScale;
	unsigned nLeft = m_nOffX, nRight = m_nOffX + VDG_WIDTH * m_nScale;
	if (nBottom > m_nFBHeight) nBottom = m_nFBHeight;
	if (nRight > nWidth) nRight = nWidth;
	for (unsigned y = 0; y < m_nFBHeight; y++)
	{
		u8 *pLine = m_pFB + y * m_nFBPitch;
		if (y < nTop || y >= nBottom)
		{
			memcpy (pLine, LineBuf, nWidth * nBytesPP);	// above / below the picture
		}
		else
		{
			if (nLeft > 0)
			{
				memcpy (pLine, LineBuf, nLeft * nBytesPP);
			}
			if (nRight < nWidth)
			{
				memcpy (pLine + nRight * nBytesPP, LineBuf, (nWidth - nRight) * nBytesPP);
			}
		}
	}
}

// ---- USB keyboards ------------------------------------------------------------
// A keyboard often shows up as more than one device (ukbd1 + ukbd2), so
// several are watched at once, each with its own slot and key state.

static unsigned char s_ucOldModifiers[MAX_KEYBOARDS];
static unsigned char s_OldKeys[MAX_KEYBOARDS][6];

unsigned CKernel::CountKeyboards (void) const
{
	unsigned nCount = 0;
	for (unsigned i = 0; i < MAX_KEYBOARDS; i++)
	{
		nCount += m_pKeyboards[i] != 0;
	}
	return nCount;
}

void CKernel::UpdateUSB (void)
{
	if (!m_USBHCI.UpdatePlugAndPlay ())
	{
		// A removal may not count as an update, but its handler has run
		if (m_bUSBRemoved)
		{
			m_bUSBRemoved = false;
			Log ("USB:    now %u keyboard(s)", CountKeyboards ());
			if (m_nUSBLogFlushes < USB_LOG_FLUSHES)
			{
				m_nUSBLogFlushes++;
				FlushBootLog ();
			}
		}
		return;
	}
	m_bUSBRemoved = false;

	for (unsigned n = 1; n <= MAX_KEYBOARDS; n++)
	{
		char Name[16];
		snprintf (Name, sizeof Name, "ukbd%u", n);
		CUSBKeyboardDevice *pKeyboard =
			(CUSBKeyboardDevice *) mDeviceNameService.GetDevice (Name, FALSE);
		if (pKeyboard == 0)
		{
			continue;
		}
		bool bKnown = false;
		for (unsigned i = 0; i < MAX_KEYBOARDS; i++)
		{
			bKnown |= m_pKeyboards[i] == pKeyboard;
		}
		if (bKnown)
		{
			continue;
		}
		for (unsigned i = 0; i < MAX_KEYBOARDS; i++)
		{
			if (m_pKeyboards[i] == 0)
			{
				s_ucOldModifiers[i] = 0;
				memset (s_OldKeys[i], 0, 6);
				m_pKeyboards[i] = pKeyboard;
				pKeyboard->RegisterRemovedHandler (KeyRemovedHandler, (void *) (uintptr) i);
				pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandler, FALSE,
									(void *) (uintptr) i);
				Log ("USB:    keyboard %s attached", Name);
				break;
			}
		}
	}

	Log ("USB:    devices changed (frame %u); now %u keyboard(s)", s_nFrame, CountKeyboards ());
	if (m_nUSBLogFlushes < USB_LOG_FLUSHES)
	{
		m_nUSBLogFlushes++;
		FlushBootLog ();
	}
}

// Interrupt context: turn a USB report into key down/up events.
void CKernel::KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6], void *pArg)
{
	unsigned nSlot = (unsigned) (uintptr) pArg;
	s_nKeyReports++;
	unsigned char &ucOldModifiers = s_ucOldModifiers[nSlot];
	unsigned char *OldKeys = s_OldKeys[nSlot];

	unsigned char ucChanged = ucModifiers ^ ucOldModifiers;
	for (unsigned i = 0; i < 8; i++)
	{
		if (ucChanged & (1 << i))
		{
			QueueKey (0xE0 + i, (ucModifiers >> i) & 1);
		}
	}
	ucOldModifiers = ucModifiers;

	// 0x01 = keyboard rollover error: keep the previous state
	for (unsigned i = 0; i < 6; i++)
	{
		if (RawKeys[i] == 0x01)
		{
			return;
		}
	}

	for (unsigned i = 0; i < 6; i++)		// released
	{
		u8 Key = OldKeys[i];
		if (Key == 0)
		{
			continue;
		}
		bool bStill = false;
		for (unsigned j = 0; j < 6; j++)
		{
			bStill |= RawKeys[j] == Key;
		}
		if (!bStill)
		{
			QueueKey (Key, false);
		}
	}
	for (unsigned i = 0; i < 6; i++)		// pressed
	{
		u8 Key = RawKeys[i];
		if (Key == 0)
		{
			continue;
		}
		bool bWas = false;
		for (unsigned j = 0; j < 6; j++)
		{
			bWas |= OldKeys[j] == Key;
		}
		if (!bWas)
		{
			QueueKey (Key, true);
		}
	}
	memcpy (OldKeys, RawKeys, 6);
}

// Called inside UpdatePlugAndPlay() when a keyboard is unplugged: release
// whatever it was holding down, so no key sticks.
void CKernel::KeyRemovedHandler (CDevice *pDevice, void *pContext)
{
	unsigned nSlot = (unsigned) (uintptr) pContext;
	for (unsigned i = 0; i < 8; i++)
	{
		if (s_ucOldModifiers[nSlot] & (1 << i))
		{
			QueueKey (0xE0 + i, false);
		}
	}
	for (unsigned i = 0; i < 6; i++)
	{
		if (s_OldKeys[nSlot][i] > 0x01)
		{
			QueueKey (s_OldKeys[nSlot][i], false);
		}
	}
	s_ucOldModifiers[nSlot] = 0;
	memset (s_OldKeys[nSlot], 0, 6);
	s_pThis->m_pKeyboards[nSlot] = 0;
	s_pThis->Log ("USB:    a keyboard was unplugged");
	s_pThis->m_bUSBRemoved = true;
}

// Main loop: apply queued key changes to the Dragon's matrix.
// F1 = reset, F2 = turbo on/off (handled here, not Dragon keys).
void CKernel::DrainInput (void)
{
	bool bChanged = false;
	while (s_nKeyOut != s_nKeyIn)
	{
		TKeyEvent Event = s_KeyQueue[s_nKeyOut];

		s_nKeyOut = (s_nKeyOut + 1) % (sizeof s_KeyQueue / sizeof s_KeyQueue[0]);

		if (Event.Down && Event.Usage == USB_KEY_F12)
		{
			if (m_bAdmin) CloseAdmin (); else OpenAdmin ();
			s_Held[Event.Usage] = true;
			continue;
		}
		if (m_bAdmin)
		{
			// Keys belong to the admin screen; the Dragon sees none of them
			s_Held[Event.Usage] = Event.Down;
			if (Event.Down)
			{
				AdminKey (Event.Usage);
			}
			continue;
		}
		if (!Event.Down && Event.Usage == USB_KEY_F12)
		{
			s_Held[Event.Usage] = false;
			continue;
		}
		if (Event.Down && Event.Usage == USB_KEY_F1)
		{
			dragon_reset ();
			Log ("Dragon: reset (F1)");
		}
		else if (Event.Down && Event.Usage == USB_KEY_F2)
		{
			m_bTurbo = !m_bTurbo;
			Log ("Dragon: turbo %s (F2)", m_bTurbo ? "on" : "off");
		}
		if (m_bKeyLog)
		{
			Log ("Key:    %02X %s (frame %u)", Event.Usage, Event.Down ? "down" : "up", s_nFrame);
		}
		s_Held[Event.Usage] = Event.Down;
		keyfeed_event (Event.Usage, Event.Down, s_nFrame);
		bChanged = true;
	}

	if (m_bAdmin)
	{
		return;
	}
	u8 Matrix[8];
	if (m_bRawKeys)
	{
		// keys=raw: every key held goes straight to the matrix (games
		// that read two keys at once; typing fast loses keys)
		if (bChanged)
		{
			keymap_matrix (s_Held, Matrix);
			dragon_set_matrix (Matrix);
		}
	}
	else
	{
		keyfeed_frame (s_nFrame, Matrix);	// one key at a time (see keymap_usb.h)
		dragon_set_matrix (Matrix);
	}
}

CStdlibApp::TShutdownMode CKernel::Run (void)
{
	Say ("6809Emulator-Metal " VERSION " - Dragon 32 (6809)");
	Say ("");
	LogBoard ();

	if (!m_bSDOK)
	{
		Fatal ("Cannot start the SD card, or mount its FAT partition.");
	}
	LoadROM ();

	// Few log writes during start-up (each costs ~0.3 s on a Zero W)
	Say ("USB:    starting...");
	FlushBootLog ();			// in case USB start-up hangs
	if (!m_USBHCI.Initialize ())
	{
		Say ("USB:    failed to start; no keyboard.");
	}
	UpdateUSB ();
	Say ("USB:    %u keyboard(s) found (more can be plugged in later)", CountKeyboards ());

	SetupDisplay ();
	StartWiFi ();
	m_bKeyLog = mOptions.GetAppOptionDecimal ("keylog", 0) == 1;
	m_bDebug = mOptions.GetAppOptionDecimal ("debug", 0) == 1;
	m_bRawKeys = strcmp (mOptions.GetAppOptionString ("keys", "typing"), "raw") == 0;
	keyfeed_reset ();
	Log ("Keys:   %s", m_bRawKeys ? "raw (keys=raw: straight to the matrix)"
				  : "typing (one key at a time; keys=raw for games)");
	if (m_bDebug)
	{
		Log ("Debug:  debug=1 in cmdline.txt: status line + log saved every 5 s");
	}
	if (m_bKeyLog)
	{
		Log ("Keys:   keylog=1 in cmdline.txt: every USB key goes in the log");
	}
	dragon_init (m_pROM);
	StartTapes ();
	StartSound ();
	Log ("Dragon: started");
	FlushBootLog ();

	// From here the screen belongs to the Dragon
	if (m_bScreenOK)
	{
		static const char ClearScreen[] = "\x1b[H\x1b[J\x1b[?25l";
		mScreen.Write (ClearScreen, sizeof ClearScreen - 1);
		Blit (true);
	}

	u64 nNow = CTimer::GetClockTicks64 ();
	u64 nNextFrame = nNow + FRAME_US;
	u64 nNextUSB = nNow + 100000;
	u64 nNextDraw = nNow;
	u64 nNextBeat = nNow;			// ACT LED heartbeat
	bool bLED = false;
	u64 nNextDebug = nNow + 5000000;
	u64 nNextStatus = nNow + 1000000;
	u64 nLoopStart = nNow;

	// Speed: CPU time spent inside dragon_run_frame()
	u64 nStatsStart = nNow;
	u64 nEmuUs = 0;
	unsigned nFrames = 0;
	unsigned nReports = 0;
	bool bLoggedFirstSecond = false;

	while (1)
	{
		DrainInput ();

		if (m_bAdmin)
		{
			// Admin screen open: the Dragon is paused
			nNow = CTimer::GetClockTicks64 ();
			static u64 nNextAdminDraw;
			if (m_bAdminDirty || nNow >= nNextAdminDraw)
			{
				DrawAdmin ();
				nNextAdminDraw = nNow + 1000000;	// clock + tape counter
			}
		}
		else
		{
			u64 nBefore = CTimer::GetClockTicks64 ();
			dragon_run_frame ();
			FeedSound ();
			s_nFrame++;
			nNow = CTimer::GetClockTicks64 ();
			nEmuUs += nNow - nBefore;
			nFrames++;

			// Draw at most 50 times a second (turbo runs many frames per draw)
			if (nNow >= nNextDraw)
			{
				Blit (false);
				nNextDraw = nNow + FRAME_US;
			}

			// The tape strip under the picture (debug=1 shows its own)
			static u64 nNextStrip;
			if (!m_bDebug && nNow >= nNextStrip)
			{
				DrawTapeStrip ();
				nNextStrip = nNow + 250000;
			}
		}

		UpdateNetwork (nNow);

		// Tape motor starts/stops go in the log (a session can be followed)
		static bool bMotorWas;
		if (dragon_tape_motor () != bMotorWas)
		{
			bMotorWas = !bMotorWas;
			Log ("Tape:   motor %s at %u.%01u s%s", bMotorWas ? "on" : "off",
			     dragon_tape_pos_ms () / 1000, dragon_tape_pos_ms () % 1000 / 100,
			     dragon_tape_inserted () ? "" : " (no tape inserted)");
			FlushBootLog ();
		}

		if (nNow >= nNextUSB)
		{
			UpdateUSB ();
			nNextUSB = nNow + 100000;
		}

		// Heartbeat: the ACT LED blinks twice a second while this loop runs.
		// Frozen picture + blinking LED = the Dragon is stuck, not the Pi.
		if (nNow >= nNextBeat)
		{
			bLED = !bLED;
			if (bLED) mActLED.On (); else mActLED.Off ();
			nNextBeat = nNow + 250000;
		}

		if (m_bDebug && !m_bAdmin && nNow >= nNextStatus)
		{
			char Text[160], Clock[16];
			if (!ClockText (Clock, sizeof Clock))
			{
				strcpy (Clock, "--:--:--");
			}
			unsigned nQueued = m_pSound != 0
				? ((CSoundBaseDevice *) m_pSound)->GetQueueFramesAvail () : 0;
			snprintf (Text, sizeof Text,
				  "%s UP %us TICKS %u FRAMES %u IRQS %u KEYRPT %u DROP %u WAIT %u KBD %u SDOK %u SDFAIL %u SAVE %uMS SND %u",
				  Clock, (unsigned) ((nNow - nLoopStart) / 1000000), (unsigned) CTimer::Get ()->GetTicks (),
				  s_nFrame, dragon_irqs (), s_nKeyReports, s_nKeyDrops, keyfeed_waiting (), CountKeyboards (),
				  m_nFlushOK, m_nFlushFail, m_nLastSaveMs, nQueued);
			DrawStatus (Text);
			nNextStatus = nNow + 1000000;
		}

		if (m_bDebug && nNow >= nNextDebug)
		{
			DragonRegs Regs;
			dragon_regs (&Regs);
			unsigned nQueued = m_pSound != 0
				? ((CSoundBaseDevice *) m_pSound)->GetQueueFramesAvail () : 0;
			static const char *const Wait[] = { "", " SYNC", " CWAI" };
			Log ("Debug:  %us ticks %u frame %u irqs %u PC=%04X CC=%02X S=%04X%s sound %u keyrpt %u drop %u sd %u/%u last save %u ms%s",
			     (unsigned) ((nNow - nLoopStart) / 1000000), (unsigned) CTimer::Get ()->GetTicks (),
			     s_nFrame, dragon_irqs (), Regs.pc, Regs.cc, Regs.s, Wait[Regs.wait], nQueued,
			     s_nKeyReports, s_nKeyDrops, m_nFlushOK, m_nFlushFail, m_nLastSaveMs,
			     m_bTurbo ? " turbo" : "");
			FlushBootLog ();
			nNextDebug = CTimer::GetClockTicks64 () + 5000000;
		}

		if (!bLoggedFirstSecond && nNow - nStatsStart >= 1000000)
		{
			bLoggedFirstSecond = true;
			Log ("Dragon: running (first second: %u frames)", nFrames);
			FlushBootLog ();
		}

		if (nNow - nStatsStart >= STATS_US)
		{
			unsigned nFrameUs = (unsigned) (nEmuUs / (nFrames ? nFrames : 1));
			Log ("Dragon: %u frames in 10 s, %u us of CPU per frame, %u key reports%s%s",
			     nFrames, nFrameUs, s_nKeyReports, m_bTurbo ? " (turbo)" : "",
			     m_bAdmin ? " (admin screen open)" : "");
			if (nReports % 6 == 5)
			{
				FlushBootLog ();		// once a minute (a quick append)
			}
			if (++nReports == SPEED_FILE_AT)
			{
				WriteSpeedFile (nFrameUs);
				FlushBootLog ();	// once, at 60 s
			}
			nEmuUs = 0;
			nFrames = 0;
			nStatsStart = CTimer::GetClockTicks64 ();
		}

		// Hold to 50 frames a second unless in turbo
		if (!m_bTurbo)
		{
			while (CTimer::GetClockTicks64 () < nNextFrame)
			{
				// wait; USB keys arrive by interrupt meanwhile, and the
				// network (while it has work) gets the spare time
				if (m_bNetActive)
				{
					m_Scheduler.Yield ();
				}
			}
			nNextFrame += FRAME_US;
			if (CTimer::GetClockTicks64 () > nNextFrame + 4 * FRAME_US)
			{
				nNextFrame = CTimer::GetClockTicks64 () + FRAME_US;	// fell behind: don't burst
			}
		}
		else
		{
			nNextFrame = CTimer::GetClockTicks64 () + FRAME_US;
		}
	}

	return ShutdownHalt;
}
