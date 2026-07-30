/*****************************************************************
Ultimate 64/II+ Command Library - DOS functions

Based on Ultimate II Dos Lib
Scott Hutter, Francesco Sblendorio
https://github.com/xlar54/ultimateii-dos-lib

Based on ultimate_dos-1.2.docx and command interface.docx
https://github.com/markusC64/1541ultimate2/tree/master/doc

Disclaimer:  Because of the nature of DOS commands, use this code
solely at your own risk.

Patches and pull requests are welcome
******************************************************************/

#include <string.h>
#include <petscii.h>
#include "ultimate_common_lib.h"

// Switching code generation to bank 0 common routine section
#pragma code(code)
#pragma data(data)

char uii_status[STATUS_QUEUE_SZ + 1];
char uii_data[DATA_QUEUE_SZ + 1];
char temp_string_onechar[2];
unsigned uii_data_index;
unsigned uii_data_len;

char uii_target = TARGET_DOS1;
struct DevInfo uii_devinfo[4];

// Core functions

void uii_logtext(const char *text)
// uii_logtext — Log text for debugging; a no-op unless DEBUG is defined.
// Input:  text — the text to log.
// Output: none.
// Syntax: uii_logtext("\nwaiting...");
{
#ifdef DEBUG
	printf("%s", text);
#else
	text = NULL;
#endif
}

void uii_logstatusreg(void)
// uii_logstatusreg — Log the UCI status register value for debugging; a no-op unless DEBUG is defined.
// Output: none.
// Syntax: uii_logstatusreg();
{
#ifdef DEBUG
	printf("\nstatus reg %4x = %2x", &uii_reg_read.status, uii_reg_read.status);
#endif
}

char uii_detect(void)
// uii_detect — Detect presence of the UCI via the ID register ($DF1D); value should be $C9.
// Output: 1 = detected (UCI is also reset via uii_abort()), 0 = not detected.
// Syntax: if (!uii_detect()) { /* no Ultimate cartridge present */ }
{
	if (uii_reg_read.id == 0xc9)
	{
		// Reset UCI
		uii_abort();

		// Return 1 for detected = true
		return 1;
	}
	else
	{
		// Return 0 for detected = false
		return 0;
	}
}

void uii_settarget(char id)
// uii_settarget — Set the target subsystem for the next command.
// Input:  id — the target ID: TARGET_DOS1 (1), TARGET_DOS2 (2), TARGET_NETWORK (3), TARGET_CONTROL (4).
// Output: none.
// Syntax: uii_settarget(TARGET_CONTROL);
{
	uii_target = id;
}

void uii_freeze(void)
// uii_freeze — Trigger a freeze (cartridge freeze-button equivalent) via the control interface.
// Output: none.
// Syntax: uii_freeze();
{
	char cmd[] = {0x00, 0x05};

	uii_settarget(TARGET_CONTROL);

	uii_sendcommand(cmd, 2);
	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_identify(void)
// uii_identify — Identify the UCI.
// The "Identify" command sends back an identification string, such as "ULTIMATE-II DOS V1.0". The
// user software can use this function to query which targets exist, or to obtain version information.
// The status channel will report "00,OK", as this command cannot fail.
// Output: identification string in uii_data[]; status is always "00,OK".
// Syntax: uii_identify();
{
	char cmd[] = {0x00, DOS_CMD_IDENTIFY};
	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);
	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_echo(void)
// uii_echo — Echo the command back as data (test command).
// This command will simply echo the command back as a data packet. The status channel will return
// "00,OK", as this command cannot fail.
// Output: echo of the sent command in uii_data[]; status is always "00,OK".
// Syntax: uii_echo();
{
	char cmd[] = {0x00, DOS_CMD_ECHO};
	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_getinterfacecount(void)
// uii_getinterfacecount — Get the number of network interfaces on the Ultimate device.
// Output: interface count in uii_data[0]; the previously active target is saved and restored.
// Syntax: uii_getinterfacecount();
{
	char tempTarget = uii_target;
	char cmd[] = {0x00, NET_CMD_GET_INTERFACE_COUNT};

	uii_settarget(TARGET_NETWORK);
	uii_sendcommand(cmd, 0x02);

	uii_readdata();
	uii_readstatus();
	uii_accept();

	uii_target = tempTarget;
}

void uii_sendcommand(char *bytes, unsigned count)
// uii_sendcommand — Send a raw command packet to the UCI (lowest-level send primitive); waits for the UCI to be idle and retries if the error bit is set.
// Input:  bytes — command buffer; bytes[0] is overwritten with the current uii_target, bytes[1] is the opcode, remaining bytes are arguments.
// Input:  count — total number of bytes to send, including the target and opcode bytes.
// Output: none.
// Syntax: char cmd[] = {0x00, DOS_CMD_IDENTIFY}; uii_sendcommand(cmd, 2);
{
	unsigned x = 0;
	char success = 0;

	bytes[0] = uii_target;

	while (success == 0)
	{
		// Wait for idle state
		uii_logtext("\nwaiting for cmd-busy to clear...");
		uii_logstatusreg();

		while (!(((uii_reg_read.status & 32) == 0) && ((uii_reg_read.status & 16) == 0)))
		{
			uii_logtext("\nwaiting...");
			uii_logstatusreg();
		};

		// Write char by char to data register
		uii_logtext("\nwriting command...");
		while (x < count)
			uii_reg_write.cmddata = bytes[x++];

		// Send PUSH_CMD
		uii_logtext("\npushing command...");
		uii_reg_write.control |= 0x01;

		uii_logstatusreg();

		// check ERROR bit.  If set, clear it via ctrl reg, and try again
		if ((uii_reg_read.status & 4) == 4)
		{
			uii_logtext("\nerror was set. trying again");
			uii_reg_write.control |= 0x08;
		}
		else
		{
			uii_logstatusreg();

			// check for cmd busy
			while (((uii_reg_read.status & 32) == 0) && ((uii_reg_read.status & 16) == 16))
			{
				uii_logtext("\nstate is busy");
			}
			success = 1;
		}
	}

	uii_logstatusreg();
	uii_logtext("\ncommand sent");
}

void uii_accept(void)
// uii_accept — Acknowledge completion of a UCI response and release the UCI for the next command.
// Output: none; must be called after draining all response data and status.
// Syntax: uii_readdata(); uii_readstatus(); uii_accept();
{
	uii_logstatusreg();
	uii_logtext("\nsending ack");
	uii_reg_write.control |= 0x02;
	while (!(uii_reg_read.status & 2) == 0)
	{
		uii_logtext("\nwaiting for ack...");
		uii_logstatusreg();
	};
}

char uii_isdataavailable(void)
// uii_isdataavailable — Check whether the UCI data FIFO has bytes to read (status bit 7).
// Output: 1 if data is available, 0 if not.
// Syntax: while (uii_isdataavailable()) { uii_readdata(); uii_accept(); }
{
	if (((uii_reg_read.status & 128) == 128))
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

char uii_ismoredataavailable(void)
// uii_ismoredataavailable — Check whether more packets follow in a multi-packet transfer (status bits 4 and 5 both set).
// Output: 1 if more packets follow, 0 if this is the last packet.
// Syntax: while (uii_isdataavailable() || uii_ismoredataavailable()) { uii_readdata(); uii_accept(); }
{
	if (((uii_reg_read.status & 48) == 48))
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

char uii_isstatusdataavailable(void)
// uii_isstatusdataavailable — Check whether the UCI status FIFO has bytes to read (status bit 6).
// Output: 1 if status data is available, 0 if not.
// Syntax: while (uii_isstatusdataavailable()) { uii_readstatus(); }
{
	if (((uii_reg_read.status & 64) == 64))
		return 1;
	else
		return 0;
}

void uii_abort(void)
// uii_abort — Abort the current UCI operation by setting the ABORT bit in the control register.
// Output: none; does not wait for confirmation.
// Syntax: uii_abort();
{
	uii_logstatusreg();
	uii_logtext("\nsending abort");
	uii_reg_write.control |= 0x04;
}

unsigned uii_readdata(void)
// uii_readdata — Drain the UCI response data FIFO into uii_data[].
// Output: number of bytes read (0 if no data available); uii_data[] is always null-terminated and capped at DATA_QUEUE_SZ bytes.
// Syntax: unsigned n = uii_readdata();
{
	unsigned count = 0;
	uii_data[0] = 0;
	uii_logtext("\n\nreading data...");
	uii_logstatusreg();

	// If there is data to read
	while (uii_isdataavailable())
	{
		if (count < DATA_QUEUE_SZ)
		{
			uii_data[count++] = uii_reg_read.respdata;
		}
		else
		{
			// Data buffer full, abort reading
			uii_logtext("\ndata buffer full, aborting read");
			break;
		}
	}
	uii_data[count] = 0;
	return count;
}

unsigned uii_readstatus(void)
// uii_readstatus — Drain the UCI status FIFO into uii_status[].
// Output: number of status bytes read; uii_status[] is always null-terminated and capped at STATUS_QUEUE_SZ bytes. UII_SUCCESS is valid after this call.
// Syntax: uii_readstatus(); if (UII_SUCCESS) { ... }
{
	unsigned count = 0;
	uii_status[0] = 0;

	uii_logtext("\n\nreading status...");
	uii_logstatusreg();

	while (uii_isstatusdataavailable())
	{
		if (count < STATUS_QUEUE_SZ)
		{
			uii_status[count++] = uii_reg_read.statusdata;
		}
		else
		{
			// Status buffer full, abort reading
			uii_logtext("\nstatus buffer full, aborting read");
			break;
		}
	}

	uii_status[count] = 0;
	return count;
}
