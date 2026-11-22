//
// kernel.h - 6809Emulator-Metal: bare-metal Dragon 32 on a Raspberry Pi
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef _kernel_h
#define _kernel_h

#include <circle_stdlib_app.h>
#include <circle/usb/usbkeyboard.h>
#include <fatfs/ff.h>

#define MAX_KEYBOARDS	4	// keyboards in use at once (one often shows up as two)
#define USB_LOG_FLUSHES	40	// boot log rewrites for USB changes per boot

// Circle's logger writes here: every line goes to the UART and into the
// boot log kept in RAM (written to SD:/bootlog-<board>.txt at each stage).
class CLogTee : public CDevice
{
public:
	CLogTee (CDevice *pSerial) : m_pSerial (pSerial) {}
	int Write (const void *pBuffer, size_t nCount) override;
private:
	CDevice *m_pSerial;
};

class CKernel : public CStdlibAppScreen
{
public:
	CKernel (void);

	bool Initialize (void) override;
	TShutdownMode Run (void) override;

private:
	// Startup and logging
	void Say (const char *pFormat, ...);	// screen + boot log + serial
	void Log (const char *pFormat, ...);	// boot log + serial only
	void Fatal (const char *pFormat, ...);	// say, flush the log, then stop for good
	void FlushBootLog (void);
	static void PanicHandler (void);	// Circle panic: crash text on screen, save the log
	void LogBoard (void);
	bool LoadROM (void);
	void WriteSpeedFile (unsigned nFrameUs);

	// Display
	void SetupDisplay (void);
	void Blit (bool bForce);

	// Input (the USB handler runs in interrupt context: it only queues)
	void UpdateUSB (void);
	void DrainInput (void);
	unsigned CountKeyboards (void) const;
	static void KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6], void *pArg);
	static void KeyRemovedHandler (CDevice *pDevice, void *pContext);

private:
	CUSBHCIDevice	m_USBHCI;
	CEMMCDevice	m_EMMC;
	FATFS		m_FileSystem;
	CConsole	m_SerialConsole;	// stdout of the core (e.g. "Unhandled opcode") -> UART
	CLogTee		m_LogTee;

	// Per-board file names, so several boards can share one card
	char		m_SpeedFile[64];
	char		m_BootLogFile[64];

	bool		m_bSDOK;
	bool		m_bScreenOK;
	bool		m_bTurbo;		// F2: run flat out
	bool		m_bKeyLog;		// cmdline.txt keylog=1: log every USB key

	CUSBKeyboardDevice * volatile m_pKeyboards[MAX_KEYBOARDS];
	unsigned	m_nUSBLogFlushes;
	volatile bool	m_bUSBRemoved;

	u8		*m_pROM;

	// Display geometry
	u8		*m_pFB;
	unsigned	m_nFBWidth, m_nFBHeight, m_nFBPitch, m_nFBDepth;
	unsigned	m_nScale, m_nOffX, m_nOffY;
	u32		m_Palette[16];		// palette index -> pixel value at this depth

	static CKernel *s_pThis;
};

#endif
