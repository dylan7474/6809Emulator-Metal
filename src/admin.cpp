//
// admin.cpp - 6809Emulator-Metal: the admin screen (F12), the tape deck's
// files on the SD card, and safe shutdown
//
// While the admin screen is open the Dragon is paused (no frames run);
// USB, Wi-Fi and the heartbeat carry on.  The selected line is shown by a
// '>' marker as well as its colour.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "kernel.h"
#include "core/dragon.h"
#include "keymap_usb.h"

#include <circle/sound/soundbasedevice.h>
#include <circle/startup.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TAPE_DIR	"SD:/tapes"
#define REC_WAV		"SD:/tapes/_recording.wav"
#define REC_CAS		"SD:/tapes/_recording.cas"

#define MAX_FILES	200
#define NAME_LEN	256		// FatFs long names are up to 255 characters

// USB HID usages used by the menus
#define KEY_ENTER	0x28
#define KEY_BACKSPACE	0x2A
#define KEY_ESC		0x29
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E

static char s_Files[MAX_FILES][NAME_LEN];
static u32 s_FileSize[MAX_FILES];
static bool s_IsDir[MAX_FILES];
static unsigned s_nFiles;
static char s_Dir[256];			// folder being browsed, under /tapes ("" = top)
static DragonTapeProgram s_Programs[64];
static unsigned s_nPrograms;

enum { ITEM_RESUME, ITEM_CHOOSE, ITEM_PLAY, ITEM_REWIND, ITEM_BACK, ITEM_FORWARD,
       ITEM_PROGRAMS, ITEM_EJECT, ITEM_RESET, ITEM_HARDRESET, ITEM_SOUND, ITEM_TURBO, ITEM_SHUTDOWN,
       ITEMS };

// ---- Tape files ---------------------------------------------------------------

unsigned CKernel::TapeRead (void *pCtx, uint32_t nOffset, uint8_t *pBuf, unsigned nLen)
{
	CKernel *pThis = (CKernel *) pCtx;
	UINT nRead = 0;
	if (   f_lseek (&pThis->m_TapeFile, nOffset) != FR_OK
	    || f_read (&pThis->m_TapeFile, pBuf, nLen, &nRead) != FR_OK)
	{
		return 0;
	}
	return nRead;
}

static bool EndsWith (const char *p, const char *pExt)
{
	size_t n = strlen (p), e = strlen (pExt);
	return n >= e && strcasecmp (p + n - e, pExt) == 0;
}

bool CKernel::TapeInsert (const char *pName)
{
	TapeEject ();
	char Path[540];
	snprintf (Path, sizeof Path, TAPE_DIR "/%s", pName);	// pName may include folders
	if (f_open (&m_TapeFile, Path, FA_READ) != FR_OK)
	{
		Message ("CANNOT OPEN %s", pName);
		return false;
	}
	m_bTapeOpen = true;
	int nKind = EndsWith (pName, ".wav") ? DRAGON_TAPE_WAV : DRAGON_TAPE_CAS;
	if (!dragon_tape_insert (nKind, (u32) f_size (&m_TapeFile), TapeRead, this))
	{
		f_close (&m_TapeFile);
		m_bTapeOpen = false;
		Message ("%s IS NOT A TAPE THIS CAN PLAY", pName);
		Log ("Tape:   %s is not a usable tape", pName);
		return false;
	}
	strncpy (m_TapeName, pName, sizeof m_TapeName - 1);
	m_TapeName[sizeof m_TapeName - 1] = '\0';
	Log ("Tape:   inserted %s (%u s)", pName, dragon_tape_len_ms () / 1000);
	return true;
}

void CKernel::TapeEject (void)
{
	dragon_tape_eject ();
	if (m_bTapeOpen)
	{
		f_close (&m_TapeFile);
		m_bTapeOpen = false;
	}
	m_TapeName[0] = '\0';
}

// The folder s_Dir under /tapes: ".." (inside a folder), then folders,
// then .cas/.wav files, each alphabetically
void CKernel::ScanTapes (void)
{
	s_nFiles = 0;
	if (s_Dir[0] != '\0')
	{
		strcpy (s_Files[0], "..");
		s_IsDir[0] = true;
		s_FileSize[0] = 0;
		s_nFiles = 1;
	}
	char Path[300];
	snprintf (Path, sizeof Path, TAPE_DIR "%s%s", s_Dir[0] ? "/" : "", s_Dir);
	FATFS_DIR Dir;
	FILINFO Info;
	if (f_opendir (&Dir, Path) != FR_OK)
	{
		s_Dir[0] = '\0';		// a folder that has gone: back to the top
		return;
	}
	unsigned nFirst = s_nFiles;
	while (s_nFiles < MAX_FILES && f_readdir (&Dir, &Info) == FR_OK && Info.fname[0] != '\0')
	{
		bool bDir = (Info.fattrib & AM_DIR) != 0;
		if (   Info.fname[0] == '_' || Info.fname[0] == '.'
		    || (!bDir && !(EndsWith (Info.fname, ".cas") || EndsWith (Info.fname, ".wav"))))
		{
			continue;
		}
		snprintf (s_Files[s_nFiles], NAME_LEN, "%s", Info.fname);
		s_FileSize[s_nFiles] = (u32) Info.fsize;
		s_IsDir[s_nFiles] = bDir;
		s_nFiles++;
	}
	f_closedir (&Dir);

	// folders before files, then alphabetical ignoring case
	for (unsigned i = nFirst + 1; i < s_nFiles; i++)
	{
		for (unsigned j = i; j > nFirst; j--)
		{
			bool bSwap = s_IsDir[j] != s_IsDir[j - 1] ? s_IsDir[j]
				     : strcasecmp (s_Files[j - 1], s_Files[j]) > 0;
			if (!bSwap)
			{
				break;
			}
			char Tmp[NAME_LEN];
			memcpy (Tmp, s_Files[j], NAME_LEN);
			memcpy (s_Files[j], s_Files[j - 1], NAME_LEN);
			memcpy (s_Files[j - 1], Tmp, NAME_LEN);
			u32 t = s_FileSize[j]; s_FileSize[j] = s_FileSize[j - 1]; s_FileSize[j - 1] = t;
			bool d = s_IsDir[j]; s_IsDir[j] = s_IsDir[j - 1]; s_IsDir[j - 1] = d;
		}
	}
}

// Into a folder (pName) or up one (".." / NULL)
void CKernel::TapeFolder (const char *pName)
{
	if (pName == 0 || strcmp (pName, "..") == 0)
	{
		char *p = strrchr (s_Dir, '/');
		if (p != 0) *p = '\0'; else s_Dir[0] = '\0';
	}
	else if (strlen (s_Dir) + strlen (pName) + 2 < sizeof s_Dir)
	{
		if (s_Dir[0] != '\0')
		{
			strcat (s_Dir, "/");
		}
		strcat (s_Dir, pName);
	}
	ScanTapes ();
	m_nSel = 0;
	m_nTop = 0;
	m_bAdminClear = true;		// a different list: redraw from blank
}

// ---- Recording (CSAVE) ----------------------------------------------------------
// Written as tapes/_recording.wav + .cas, renamed to the program's name when
// the Dragon has finished (NAME.WAV, or NAME-2.WAV ... if that exists).

static void WavHeader (u8 *h, u32 nData)
{
	u32 nRiff = nData + 36, nFmt = 16, nRate = DRAGON_TAPE_WAV_RATE, nByteRate = DRAGON_TAPE_WAV_RATE;
	u16 nPCM = 1, nCh = 1, nAlign = 1, nBits = 8;
	memcpy (h, "RIFF", 4); memcpy (h + 4, &nRiff, 4);
	memcpy (h + 8, "WAVEfmt ", 8); memcpy (h + 16, &nFmt, 4);
	memcpy (h + 20, &nPCM, 2); memcpy (h + 22, &nCh, 2); memcpy (h + 24, &nRate, 4);
	memcpy (h + 28, &nByteRate, 4); memcpy (h + 32, &nAlign, 2); memcpy (h + 34, &nBits, 2);
	memcpy (h + 36, "data", 4); memcpy (h + 40, &nData, 4);
}

void CKernel::RecBegin (void *pCtx)
{
	CKernel *pThis = (CKernel *) pCtx;
	f_mkdir (TAPE_DIR);
	bool bWav = f_open (&pThis->m_RecWav, REC_WAV, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
	bool bCas = f_open (&pThis->m_RecCas, REC_CAS, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
	if (!bWav || !bCas)
	{
		if (bWav) f_close (&pThis->m_RecWav);
		if (bCas) f_close (&pThis->m_RecCas);
		pThis->m_bRecOpen = false;
		pThis->Log ("Tape:   cannot create the recording files in /tapes");
		return;
	}
	u8 Header[44];
	WavHeader (Header, 0);
	UINT n;
	f_write (&pThis->m_RecWav, Header, sizeof Header, &n);
	pThis->m_bRecOpen = true;
	pThis->Log ("Tape:   recording");
}

void CKernel::RecWav (void *pCtx, const uint8_t *pSamples, unsigned nCount)
{
	CKernel *pThis = (CKernel *) pCtx;
	UINT n;
	if (pThis->m_bRecOpen)
	{
		f_write (&pThis->m_RecWav, pSamples, nCount, &n);
	}
}

void CKernel::RecCas (void *pCtx, const uint8_t *pBytes, unsigned nCount)
{
	CKernel *pThis = (CKernel *) pCtx;
	UINT n;
	if (pThis->m_bRecOpen)
	{
		f_write (&pThis->m_RecCas, pBytes, nCount, &n);
	}
}

void CKernel::RecEnd (void *pCtx, const char *pName)
{
	CKernel *pThis = (CKernel *) pCtx;
	if (!pThis->m_bRecOpen)
	{
		return;
	}
	pThis->m_bRecOpen = false;

	u32 nData = (u32) f_size (&pThis->m_RecWav) - 44;
	u8 Header[44];
	WavHeader (Header, nData);
	UINT n;
	f_lseek (&pThis->m_RecWav, 0);
	f_write (&pThis->m_RecWav, Header, sizeof Header, &n);
	f_close (&pThis->m_RecWav);
	f_close (&pThis->m_RecCas);

	// A file name from the program's name: letters, digits, - and _
	char Base[16];
	unsigned k = 0;
	for (const char *p = pName; *p != '\0' && k < 8; p++)
	{
		char c = *p;
		Base[k++] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			    || c == '-' ? c : '_';
	}
	Base[k] = '\0';
	if (k == 0)
	{
		strcpy (Base, "NONAME");
	}

	// Never overwrite: NAME, then NAME-2, NAME-3 ...
	char Wav[64], Cas[64];
	FILINFO Info;
	for (unsigned nTry = 1; nTry < 1000; nTry++)
	{
		if (nTry == 1)
		{
			snprintf (Wav, sizeof Wav, TAPE_DIR "/%s.wav", Base);
			snprintf (Cas, sizeof Cas, TAPE_DIR "/%s.cas", Base);
		}
		else
		{
			snprintf (Wav, sizeof Wav, TAPE_DIR "/%s-%u.wav", Base, nTry);
			snprintf (Cas, sizeof Cas, TAPE_DIR "/%s-%u.cas", Base, nTry);
		}
		if (f_stat (Wav, &Info) != FR_OK && f_stat (Cas, &Info) != FR_OK)
		{
			break;
		}
	}
	f_rename (REC_WAV, Wav + 3);		// FatFs: new name without "SD:"
	f_rename (REC_CAS, Cas + 3);
	pThis->Log ("Tape:   saved %s + %s (%u s)", Wav + 10, Cas + 10,
		    nData / DRAGON_TAPE_WAV_RATE);
	pThis->Message ("SAVED %s", Cas + 10);
	pThis->FlushBootLog ();
}

static const DragonTapeRecorder *s_pRecorder;

void CKernel::StartTapes (void)
{
	f_mkdir (TAPE_DIR);
	f_unlink (REC_WAV);		// left over from a recording cut short
	f_unlink (REC_CAS);
	static DragonTapeRecorder Recorder;
	Recorder.ctx = this;
	Recorder.begin = RecBegin;
	Recorder.wav = RecWav;
	Recorder.cas = RecCas;
	Recorder.end = RecEnd;
	s_pRecorder = &Recorder;
	dragon_tape_recorder (s_pRecorder);

	// cmdline.txt tape=NAME inserts that tape at power-on
	const char *pTape = mOptions.GetAppOptionString ("tape", 0);
	if (pTape != 0 && *pTape != '\0')
	{
		TapeInsert (pTape);
	}
	ScanTapes ();
	Log ("Tape:   %u tape(s) in /tapes; CSAVE saves there (.wav + .cas)", s_nFiles);
}

// ---- Drawing ------------------------------------------------------------------

u32 CKernel::Pixel (unsigned r, unsigned g, unsigned b) const
{
	return m_nFBDepth == 16 ? ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)) : COLOR32 (r, g, b, 0xFF);
}

// Text in the 6847 font at nScale, from pixel (x, y), padded with Bg to
// nWidth characters.  Built line by line in RAM, then copied (see Blit()
// for why: the Zero's ARM11 faults on misaligned stores to the screen).
void CKernel::DrawText (unsigned x, unsigned y, unsigned nScale, const char *pText,
			unsigned nWidth, u32 Fg, u32 Bg)
{
	if (m_pFB == 0)
	{
		return;
	}
	const unsigned nBytesPP = m_nFBDepth / 8;
	if (nBytesPP != 2 && nBytesPP != 4)
	{
		return;
	}
	x &= ~1u;
	unsigned nPixels = nWidth * 8 * nScale;
	if (x + nPixels > m_nFBWidth)
	{
		nPixels = (m_nFBWidth - x) & ~1u;
	}
	if (y + 12 * nScale > m_nFBHeight)
	{
		return;
	}
	static u32 LineBuf[4096] __attribute__((aligned(64)));
	if (nPixels * nBytesPP > sizeof LineBuf)
	{
		return;
	}
	unsigned nChars = strlen (pText);
	static u8 Glyph[256][12];
	if (nChars > 256)
	{
		nChars = 256;
	}
	for (unsigned c = 0; c < nChars; c++)
	{
		for (unsigned r = 0; r < 12; r++)
		{
			Glyph[c][r] = vdg_font_row (pText[c], r);
		}
	}
	for (unsigned yy = 0; yy < 12 * nScale; yy++)
	{
		for (unsigned xx = 0; xx < nPixels; xx++)
		{
			unsigned nChar = xx / (8 * nScale);
			u32 Colour = Bg;
			if (nChar < nChars && (Glyph[nChar][yy / nScale] & (0x80 >> ((xx / nScale) & 7))))
			{
				Colour = Fg;
			}
			if (nBytesPP == 2)
			{
				((u16 *) LineBuf)[xx] = (u16) Colour;
			}
			else
			{
				LineBuf[xx] = Colour;
			}
		}
		memcpy (m_pFB + (y + yy) * m_nFBPitch + x * nBytesPP, LineBuf, nPixels * nBytesPP);
	}
}

void CKernel::FillScreen (u32 Colour)
{
	if (m_pFB == 0)
	{
		return;
	}
	const unsigned nBytesPP = m_nFBDepth / 8;
	static u32 LineBuf[4096] __attribute__((aligned(64)));
	unsigned nPixels = m_nFBWidth & ~1u;
	if (nPixels * nBytesPP > sizeof LineBuf)
	{
		return;
	}
	for (unsigned x = 0; x < nPixels; x++)
	{
		if (nBytesPP == 2) ((u16 *) LineBuf)[x] = (u16) Colour; else LineBuf[x] = Colour;
	}
	for (unsigned y = 0; y < m_nFBHeight; y++)
	{
		memcpy (m_pFB + y * m_nFBPitch, LineBuf, nPixels * nBytesPP);
	}
}

void CKernel::Message (const char *pFormat, ...)
{
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (m_Message, sizeof m_Message, pFormat, Args);
	va_end (Args);
	m_nMessageUntil = CTimer::GetClockTicks64 () + 4000000;
	m_bAdminDirty = true;
}

static void FormatMs (char *p, size_t n, u32 ms)
{
	snprintf (p, n, "%u.%01u", ms / 1000, ms % 1000 / 100);
}

void CKernel::TapeStatusText (char *pBuf, size_t nSize)
{
	char Pos[16], Len[16];
	if (!dragon_tape_inserted ())
	{
		snprintf (pBuf, nSize, "NO TAPE");
		return;
	}
	bool bEnd = dragon_tape_pos_ms () >= dragon_tape_len_ms ();
	FormatMs (Pos, sizeof Pos, dragon_tape_pos_ms ());
	FormatMs (Len, sizeof Len, dragon_tape_len_ms ());
	snprintf (pBuf, nSize, "%.40s  %s/%sS  %s  MOTOR %s", m_TapeName, Pos, Len,
		  bEnd ? "END" : dragon_tape_playing () ? "PLAY" : "STOP",
		  dragon_tape_motor () ? "ON" : "OFF");
}

// The strip under the Dragon's picture: clock, tape, F12 hint, messages
void CKernel::DrawTapeStrip (void)
{
	char Clock[16], Tape[96], Text[200];
	if (!ClockText (Clock, sizeof Clock))
	{
		strcpy (Clock, "");
	}
	TapeStatusText (Tape, sizeof Tape);
	// The Dragon is reading a tape that isn't there: it ignores the
	// keyboard until a tape plays (as a real one does), so say how out
	bool bStarved = dragon_tape_motor () && !dragon_tape_recording ()
			&& (!dragon_tape_inserted () || !dragon_tape_playing ()
			    || dragon_tape_pos_ms () >= dragon_tape_len_ms ());
	if (bStarved)
	{
		snprintf (Text, sizeof Text, "%s  %s: CLOAD IS WAITING - F12 TO REWIND/CHOOSE, F1 TO RESET",
			  Clock, !dragon_tape_inserted () ? "NO TAPE"
			  : !dragon_tape_playing () ? "TAPE STOPPED" : "TAPE AT END");
	}
	else if (m_Message[0] != '\0' && CTimer::GetClockTicks64 () < m_nMessageUntil)
	{
		snprintf (Text, sizeof Text, "%s  %s", Clock, m_Message);
	}
	else
	{
		snprintf (Text, sizeof Text, "%s  TAPE: %s%s   F12: MENU", Clock, Tape,
			  dragon_tape_recording () ? "  RECORDING" : "");
	}
	DrawStatus (Text);
}

// ---- The admin screen -------------------------------------------------------------

void CKernel::OpenAdmin (void)
{
	m_bAdmin = true;
	m_nAdminScreen = AdminMenu;
	m_nSel = 0;
	m_nTop = 0;
	m_bAdminDirty = true;
	m_bAdminClear = true;
	m_nAdminScale = m_nFBHeight >= 900 ? 3 : (m_nFBHeight >= 480 ? 2 : 1);
	Log ("Admin:  opened (F12)");
}

void CKernel::CloseAdmin (void)
{
	m_bAdmin = false;
	// Nothing held down carries into the Dragon
	keyfeed_reset ();
	u8 Matrix[8];
	memset (Matrix, 0x7F, sizeof Matrix);
	dragon_set_matrix (Matrix);
	if (m_bScreenOK)
	{
		Blit (true);		// the picture, and its border over the admin screen
	}
}

void CKernel::AdminLine (unsigned nRow, const char *pText, bool bSelected, bool bTitle)
{
	unsigned s = m_nAdminScale;
	unsigned nCols = m_nFBWidth / (8 * s);
	u32 Bg = Pixel (16, 24, 72), Fg = Pixel (235, 235, 235);
	if (bTitle)
	{
		Fg = Pixel (120, 220, 255);
	}
	char Line[256];
	if (bSelected)
	{
		Bg = Pixel (240, 210, 60);
		Fg = Pixel (0, 0, 0);
		snprintf (Line, sizeof Line, " > %s", pText);
	}
	else
	{
		snprintf (Line, sizeof Line, "   %s", pText);
	}
	DrawText (0, nRow * 12 * s, s, Line, nCols, Fg, Bg);
}

void CKernel::DrawAdmin (void)
{
	if (!m_bScreenOK)
	{
		return;
	}
	unsigned s = m_nAdminScale;
	unsigned nRows = m_nFBHeight / (12 * s);
	if (m_bAdminClear)
	{
		// Only when the page changes: blanking every second (for the
		// clock and counter) made the screen flicker
		FillScreen (Pixel (16, 24, 72));
		m_bAdminClear = false;
	}

	char Text[200], Clock[16];
	if (!ClockText (Clock, sizeof Clock))
	{
		strcpy (Clock, "NO CLOCK");
	}
	snprintf (Text, sizeof Text, "6809EMULATOR-METAL  ADMIN   %s", Clock);
	AdminLine (0, Text, false, true);
	char Tape[96];
	TapeStatusText (Tape, sizeof Tape);
	snprintf (Text, sizeof Text, "TAPE: %s%s", Tape, dragon_tape_recording () ? "  RECORDING" : "");
	AdminLine (2, Text, false, false);

	unsigned nFirst = 4;
	unsigned nVisible = nRows > nFirst + 4 ? nRows - nFirst - 4 : 1;

	if (m_nAdminScreen == AdminMenu)
	{
		for (unsigned i = 0; i < ITEMS; i++)
		{
			const char *p = "";
			switch (i)
			{
			case ITEM_RESUME:	p = "RESUME (ESC OR F12)"; break;
			case ITEM_CHOOSE:	p = "TAPE: CHOOSE A TAPE..."; break;
			case ITEM_PLAY:		p = dragon_tape_playing () ? "TAPE: STOP" : "TAPE: PLAY"; break;
			case ITEM_REWIND:	p = "TAPE: REWIND TO THE START"; break;
			case ITEM_BACK:		p = "TAPE: BACK 10 SECONDS"; break;
			case ITEM_FORWARD:	p = "TAPE: FORWARD 10 SECONDS"; break;
			case ITEM_PROGRAMS:	p = "TAPE: JUMP TO A PROGRAM..."; break;
			case ITEM_EJECT:	p = "TAPE: EJECT"; break;
			case ITEM_RESET:	p = "RESET (THE RESET BUTTON: KEEPS THE PROGRAM)"; break;
			case ITEM_HARDRESET:	p = "HARD RESET (POWER OFF AND ON: CLEARS MEMORY)"; break;
			case ITEM_SOUND:	p = m_bMuted ? "SOUND: OFF (TURN ON)" : "SOUND: ON (TURN OFF)"; break;
			case ITEM_TURBO:	p = m_bTurbo ? "TURBO: ON (TURN OFF)" : "TURBO: OFF (TURN ON)"; break;
			case ITEM_SHUTDOWN:	p = "SHUT DOWN (SAFE TO SWITCH OFF)"; break;
			}
			AdminLine (nFirst + i, p, i == m_nSel, false);
		}
	}
	else if (m_nAdminScreen == AdminFiles || m_nAdminScreen == AdminPrograms)
	{
		bool bFiles = m_nAdminScreen == AdminFiles;
		unsigned nCount = bFiles ? s_nFiles : s_nPrograms;
		if (bFiles)
		{
			snprintf (Text, sizeof Text, "/TAPES%s%.120s  (ENTER = INSERT OR OPEN):",
				  s_Dir[0] ? "/" : "", s_Dir);
			AdminLine (nFirst - 1, Text, false, true);
		}
		else
		{
			AdminLine (nFirst - 1, "PROGRAMS ON THIS TAPE (ENTER = WIND TO IT):", false, true);
		}
		if (nCount == 0)
		{
			AdminLine (nFirst + 1, bFiles ? "NO .CAS OR .WAV FILES OR FOLDERS HERE"
						      : "NONE FOUND (ONLY .CAS TAPES ARE LISTED)", false, false);
		}
		if (m_nSel < m_nTop)
		{
			m_nTop = m_nSel;
		}
		if (m_nSel >= m_nTop + nVisible)
		{
			m_nTop = m_nSel - nVisible + 1;
		}
		for (unsigned i = 0; i < nVisible; i++)
		{
			unsigned k = m_nTop + i;
			if (k >= nCount)
			{
				if (nCount > 0)
				{
					AdminLine (nFirst + i, "", false, false);	// clears old rows in place
				}
				continue;
			}
			if (bFiles)
			{
				// name cut/padded to 44 by hand, then the size
				char Name[48];
				unsigned n = 0;
				for (; n < 44 && s_Files[k][n] != '\0'; n++)
				{
					Name[n] = s_Files[k][n];
				}
				while (n < 44)
				{
					Name[n++] = ' ';
				}
				Name[n] = '\0';
				if (s_IsDir[k])
				{
					// a folder: "NAME/" (".." = up)
					char Folder[64];
					snprintf (Folder, sizeof Folder, "%.42s/", s_Files[k]);
					snprintf (Text, sizeof Text, "%-44s  FOLDER", strcmp (s_Files[k], "..") == 0
						  ? "../ (UP A FOLDER)" : Folder);
				}
				else
				{
					snprintf (Text, sizeof Text, "%s %6u KB", Name, (unsigned) ((s_FileSize[k] + 1023) / 1024));
				}
			}
			else
			{
				static const char *const Types[] = { "BASIC", "DATA", "MACHINE CODE" };
				char Pos[16];
				FormatMs (Pos, sizeof Pos, s_Programs[k].ms);
				snprintf (Text, sizeof Text, "%-8s  %-12s  AT %sS", s_Programs[k].name,
					  s_Programs[k].type <= 2 ? Types[s_Programs[k].type] : "?", Pos);
			}
			AdminLine (nFirst + i, Text, k == m_nSel, false);
		}
	}
	else if (m_nAdminScreen == AdminConfirmHard)
	{
		AdminLine (nFirst, "HARD RESET NOW?", false, true);
		AdminLine (nFirst + 2, "LIKE SWITCHING THE DRAGON OFF AND ON: MEMORY IS CLEARED", false, false);
		AdminLine (nFirst + 3, "AND ANY PROGRAM IN IT IS LOST. THE TAPE STAYS IN, AND", false, false);
		AdminLine (nFirst + 4, "A RECORDING IN PROGRESS IS FINISHED AND SAVED FIRST.", false, false);
		AdminLine (nFirst + 6, "ENTER = HARD RESET      ESC = BACK", false, false);
	}
	else if (m_nAdminScreen == AdminConfirm)
	{
		AdminLine (nFirst, "SHUT DOWN NOW?", false, true);
		AdminLine (nFirst + 2, "ANY RECORDING IS FINISHED AND SAVED, AND ALL FILES ON", false, false);
		AdminLine (nFirst + 3, "THE SD CARD ARE CLOSED. THE DRAGON'S MEMORY IS LOST.", false, false);
		AdminLine (nFirst + 5, "ENTER = SHUT DOWN      ESC = BACK", false, false);
	}

	AdminLine (nRows - 3, m_Message[0] != '\0' && CTimer::GetClockTicks64 () < m_nMessageUntil
			      ? m_Message : "", false, true);
	AdminLine (nRows - 2, m_nAdminScreen == AdminMenu
			      ? "UP/DOWN: CHOOSE   ENTER: DO IT   ESC/F12: BACK TO THE DRAGON"
			      : "UP/DOWN/PGUP/PGDN: CHOOSE   ENTER: DO IT   ESC: BACK", false, false);
	m_bAdminDirty = false;
}

void CKernel::AdminKey (u8 Usage)
{
	TAdminScreen OldScreen = m_nAdminScreen;
	AdminKeyDo (Usage);
	if (m_nAdminScreen != OldScreen)
	{
		m_bAdminClear = true;
	}
}

void CKernel::AdminKeyDo (u8 Usage)
{
	m_bAdminDirty = true;
	unsigned nCount = m_nAdminScreen == AdminMenu ? ITEMS
			: m_nAdminScreen == AdminFiles ? s_nFiles
			: m_nAdminScreen == AdminPrograms ? s_nPrograms : 0;
	unsigned nPage = m_nFBHeight / (12 * m_nAdminScale) > 12 ? m_nFBHeight / (12 * m_nAdminScale) - 10 : 4;

	switch (Usage)
	{
	case KEY_UP:	if (m_nSel > 0) m_nSel--; return;
	case KEY_DOWN:	if (m_nSel + 1 < nCount) m_nSel++; return;
	case KEY_PGUP:	m_nSel = m_nSel > nPage ? m_nSel - nPage : 0; return;
	case KEY_PGDN:	m_nSel = m_nSel + nPage < nCount ? m_nSel + nPage : (nCount ? nCount - 1 : 0); return;
	case KEY_ESC:
		if (m_nAdminScreen == AdminMenu)
		{
			CloseAdmin ();
		}
		else
		{
			m_nAdminScreen = AdminMenu;
			m_nSel = 0;
			m_nTop = 0;
		}
		return;
	case KEY_BACKSPACE:
	case KEY_LEFT:
		if (m_nAdminScreen == AdminFiles && s_Dir[0] != '\0')
		{
			TapeFolder (0);		// up a folder
		}
		return;
	case KEY_ENTER:
		break;
	default:
		return;
	}

	if (m_nAdminScreen == AdminFiles)
	{
		if (m_nSel >= s_nFiles)
		{
			return;
		}
		if (s_IsDir[m_nSel])
		{
			char Name[NAME_LEN];
			strcpy (Name, s_Files[m_nSel]);
			TapeFolder (Name);
			return;
		}
		char Rel[520];
		snprintf (Rel, sizeof Rel, "%.255s%s%.255s", s_Dir, s_Dir[0] ? "/" : "", s_Files[m_nSel]);
		if (TapeInsert (Rel))
		{
			Message ("INSERTED %s: TYPE CLOAD (OR CLOADM) TO LOAD", s_Files[m_nSel]);
			m_nAdminScreen = AdminMenu;
			m_nSel = ITEM_RESUME;
		}
		return;
	}
	if (m_nAdminScreen == AdminPrograms)
	{
		if (m_nSel < s_nPrograms)
		{
			dragon_tape_seek_ms (s_Programs[m_nSel].ms);
			Message ("WOUND TO %s", s_Programs[m_nSel].name);
			m_nAdminScreen = AdminMenu;
			m_nSel = ITEM_RESUME;
		}
		return;
	}
	if (m_nAdminScreen == AdminConfirm)
	{
		ShutDown ();		// does not return
		return;
	}
	if (m_nAdminScreen == AdminConfirmHard)
	{
		HardReset ();
		return;
	}

	u32 nPos = dragon_tape_pos_ms ();
	switch (m_nSel)
	{
	case ITEM_RESUME:
		CloseAdmin ();
		break;
	case ITEM_CHOOSE:
		ScanTapes ();
		m_nAdminScreen = AdminFiles;
		m_nSel = 0;
		m_nTop = 0;
		for (unsigned i = 0; i < s_nFiles; i++)
		{
			char Rel[520];
			snprintf (Rel, sizeof Rel, "%.255s%s%.255s", s_Dir, s_Dir[0] ? "/" : "", s_Files[i]);
			if (!s_IsDir[i] && strcmp (Rel, m_TapeName) == 0)
			{
				m_nSel = i;		// start at the tape that's in
			}
		}
		break;
	case ITEM_PLAY:
		dragon_tape_play (!dragon_tape_playing ());
		if (!dragon_tape_inserted ())
		{
			Message ("NO TAPE: CHOOSE ONE FIRST");
		}
		break;
	case ITEM_REWIND:
		dragon_tape_seek_ms (0);
		break;
	case ITEM_BACK:
		dragon_tape_seek_ms (nPos > 10000 ? nPos - 10000 : 0);
		break;
	case ITEM_FORWARD:
		dragon_tape_seek_ms (nPos + 10000);
		break;
	case ITEM_PROGRAMS:
		s_nPrograms = dragon_tape_programs (s_Programs, sizeof s_Programs / sizeof s_Programs[0]);
		m_nAdminScreen = AdminPrograms;
		m_nSel = 0;
		m_nTop = 0;
		break;
	case ITEM_EJECT:
		TapeEject ();
		Message ("TAPE EJECTED");
		break;
	case ITEM_RESET:
		dragon_reset ();
		Log ("Dragon: reset (admin)");
		CloseAdmin ();
		break;
	case ITEM_SOUND:
		m_bMuted = !m_bMuted;
		break;
	case ITEM_TURBO:
		m_bTurbo = !m_bTurbo;
		break;
	case ITEM_HARDRESET:
		m_nAdminScreen = AdminConfirmHard;
		break;
	case ITEM_SHUTDOWN:
		m_nAdminScreen = AdminConfirm;
		break;
	}
}

// Like power off and on: the whole Dragon starts again from nothing
// (RAM cleared, CPU, PIAs, SAM, VDG mode).  The cassette deck is outside
// the Dragon, so the tape stays in; a recording is finished first.
void CKernel::HardReset (void)
{
	dragon_tape_record_finish ();
	dragon_init (m_pROM);
	keyfeed_reset ();
	Log ("Dragon: hard reset (admin): memory cleared");
	CloseAdmin ();
}

// ---- Safe shutdown ----------------------------------------------------------------

void CKernel::ShutDown (void)
{
	Log ("Admin:  shutting down");
	dragon_tape_record_finish ();		// a CSAVE cut short is still saved
	TapeEject ();
	if (m_pSound != 0)
	{
		((CSoundBaseDevice *) m_pSound)->Cancel ();
	}
	Log ("Admin:  files closed; safe to switch off");
	FlushBootLog ();
	CloseBootLog ();
	f_mount (0, "SD:", 0);			// nothing more goes to the card

	if (m_bScreenOK)
	{
		unsigned s = m_nAdminScale;
		FillScreen (Pixel (0, 0, 0));
		unsigned nRows = m_nFBHeight / (12 * s);
		unsigned nCols = m_nFBWidth / (8 * s);
		DrawText (0, (nRows / 2 - 1) * 12 * s, s, "   IT IS NOW SAFE TO SWITCH OFF", nCols,
			  Pixel (255, 255, 255), Pixel (0, 0, 0));
		DrawText (0, (nRows / 2 + 1) * 12 * s, s, "   (ALL FILES ON THE SD CARD ARE CLOSED)", nCols,
			  Pixel (160, 160, 160), Pixel (0, 0, 0));
	}
	mActLED.Off ();
	while (1)
	{
		CTimer::SimpleMsDelay (1000);	// nothing more to do
	}
}
