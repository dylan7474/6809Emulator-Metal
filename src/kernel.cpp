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
#include <circle_glue.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VERSION		"0.1.0"

#define FRAME_US	20000		// 50 Hz, like a UK Dragon
#define STATS_US	10000000	// speed line in the log every 10 s
#define SPEED_FILE_AT	6		// write the speed file after the 6th (60 s)

static const char *const ROMNames[] = { "SD:/d32.rom", "SD:/dragon32.rom", "SD:/dragon.rom" };

CKernel *CKernel::s_pThis = 0;

static u8 s_Picture[VDG_HEIGHT][VDG_WIDTH];	// this frame, palette indices
static u8 s_Shadow[VDG_HEIGHT][VDG_WIDTH];	// last frame drawn

// ---- Key queue, filled in interrupt context --------------------------------

struct TKeyEvent { u8 Usage; u8 Down; };

static TKeyEvent s_KeyQueue[64];
static volatile unsigned s_nKeyIn, s_nKeyOut;
static bool s_Held[256];			// main loop only
static unsigned s_nFrame;			// frames run so far
static unsigned s_DownFrame[256];		// frame each key went down

#define MIN_HOLD_FRAMES	2			// 40 ms

static void QueueKey (u8 Usage, bool bDown)
{
	unsigned nNext = (s_nKeyIn + 1) % (sizeof s_KeyQueue / sizeof s_KeyQueue[0]);
	if (nNext == s_nKeyOut)
	{
		return;		// full: drop
	}
	s_KeyQueue[s_nKeyIn].Usage = Usage;
	s_KeyQueue[s_nKeyIn].Down = bDown;
	s_nKeyIn = nNext;
}

// ---- Boot log -------------------------------------------------------------
// Everything said during startup, Circle's own log lines and the speed lines,
// kept in RAM and written to SD:/bootlog-<board>.txt at each stage.

#define BOOTLOG_SIZE	32768

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
	m_nUSBLogFlushes (0),
	m_bUSBRemoved (false),
	m_pROM (0),
	m_pFB (0)
{
	s_pThis = this;
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

void CKernel::FlushBootLog (void)
{
	if (!m_bSDOK)
	{
		return;
	}
	FIL File;
	if (f_open (&File, m_BootLogFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return;
	}
	UINT nWritten;
	f_write (&File, s_BootLog, s_nBootLog, &nWritten);
	f_close (&File);
}

void CKernel::LogBoard (void)
{
	CMachineInfo *pInfo = CMachineInfo::Get ();
	Log ("Board:  %s (revision code %08X), %s, %u MB RAM",
	     pInfo->GetMachineName (), (unsigned) pInfo->GetRevisionRaw (),
	     pInfo->GetSoCName (), pInfo->GetRAMSize ());
	Log ("Kernel: %u-bit, Circle RASPPI=%u, built " __DATE__ " " __TIME__,
	     (unsigned) (sizeof (void *) * 8), (unsigned) RASPPI);
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
		"Frame:   %u.%02u ms of CPU time per 20 ms frame (%u steps)\r\n"
		"Headroom: %u.%02ux (how much faster than needed)\r\n"
		"\r\n"
		"Written once per boot, 60 s after the Dragon starts.\r\n",
		VERSION, CMachineInfo::Get ()->GetMachineName (),
		(unsigned) (sizeof (void *) * 8), (unsigned) RASPPI,
		m_nFBWidth, m_nFBHeight, m_nFBDepth, m_nScale,
		nFrameUs / 1000, nFrameUs % 1000 / 10, (unsigned) DRAGON_FRAME_STEPS,
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

	Log ("USB:    devices changed; now %u keyboard(s)", CountKeyboards ());
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

		// Every key is held for at least MIN_HOLD_FRAMES, or the ROM's
		// keyboard scan can miss it: a release that comes too soon waits
		// in the queue (and so do the keys after it, keeping their order).
		// QEMU's virtual keyboard sends down + up within one frame.
		if (   !Event.Down && s_Held[Event.Usage]
		    && s_nFrame - s_DownFrame[Event.Usage] < MIN_HOLD_FRAMES)
		{
			break;
		}
		s_nKeyOut = (s_nKeyOut + 1) % (sizeof s_KeyQueue / sizeof s_KeyQueue[0]);
		if (Event.Down)
		{
			s_DownFrame[Event.Usage] = s_nFrame;
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
		bChanged = true;
	}

	if (bChanged)
	{
		u8 Matrix[8];
		keymap_matrix (s_Held, Matrix);
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
	m_bKeyLog = mOptions.GetAppOptionDecimal ("keylog", 0) == 1;
	if (m_bKeyLog)
	{
		Log ("Keys:   keylog=1 in cmdline.txt: every USB key goes in the log");
	}
	dragon_init (m_pROM);
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

	// Speed: CPU time spent inside dragon_run_frame()
	u64 nStatsStart = nNow;
	u64 nEmuUs = 0;
	unsigned nFrames = 0;
	unsigned nReports = 0;
	bool bLoggedFirstSecond = false;

	while (1)
	{
		DrainInput ();

		u64 nBefore = CTimer::GetClockTicks64 ();
		dragon_run_frame ();
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

		if (nNow >= nNextUSB)
		{
			UpdateUSB ();
			nNextUSB = nNow + 100000;
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
			Log ("Dragon: %u frames in 10 s, %u us of CPU per frame%s",
			     nFrames, nFrameUs, m_bTurbo ? " (turbo)" : "");
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
				// wait; USB keys arrive by interrupt meanwhile
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
