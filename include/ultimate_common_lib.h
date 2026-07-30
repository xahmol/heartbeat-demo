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

#ifndef _ULTIMATE_COMMON_LIB_H_
#define _ULTIMATE_COMMON_LIB_H_

#include <stdlib.h>
#include <stdio.h>

// Defines for UCI registers
struct UII_READ
// Read only registers
{
	volatile char status;
	volatile char id;
	volatile char respdata;
	volatile char statusdata;
};

struct UII_WRITE
// Write only registers
{
	volatile char control;
	volatile char cmddata;
};

// Mapping the UCI registers
#define uii_reg_read	(*((struct UII_READ *)0xdf1c))
#define uii_reg_write	(*((struct UII_WRITE *)0xdf1c))

// Length of data queues
// The sizes of these queues are important to note, since they define the maximum transfer size per command.
// The UCI command queue size is 896 bytes ($380), the response data queue is also 896 bytes ($380),
// and the status queue is 256 bytes ($100).
// Buffer size is chosen smaller here to lower memory usage, increase if needed (for example for networking)
#define DATA_QUEUE_SZ 512
#define STATUS_QUEUE_SZ 256

// UCI target IDs
#define TARGET_DOS1 0x01
#define TARGET_DOS2 0x02
#define TARGET_NETWORK 0x03
#define TARGET_CONTROL 0x04

// UCI command IDs
// DOS layer commands
#define DOS_CMD_IDENTIFY 0x01
#define DOS_CMD_OPEN_FILE 0x02
#define DOS_CMD_CLOSE_FILE 0x03
#define DOS_CMD_READ_DATA 0x04
#define DOS_CMD_WRITE_DATA 0x05
#define DOS_CMD_FILE_SEEK 0x06
#define DOS_CMD_FILE_INFO 0x07
#define DOS_CMD_FILE_STAT 0x08
#define DOS_CMD_DELETE_FILE 0x09
#define DOS_CMD_RENAME_FILE 0x0a
#define DOS_CMD_COPY_FILE 0x0b
#define DOS_CMD_CHANGE_DIR 0x11
#define DOS_CMD_GET_PATH 0x12
#define DOS_CMD_OPEN_DIR 0x13
#define DOS_CMD_READ_DIR 0x14
#define DOS_CMD_COPY_UI_PATH 0x15
#define DOS_CMD_CREATE_DIR 0x16
#define DOS_CMD_COPY_HOME_PATH 0x17
#define DOS_CMD_LOAD_REU 0x21
#define DOS_CMD_SAVE_REU 0x22
#define DOS_CMD_MOUNT_DISK 0x23
#define DOS_CMD_UMOUNT_DISK 0x24
#define DOS_CMD_SWAP_DISK 0x25
#define DOS_CMD_GET_TIME 0x26
#define DOS_CMD_SET_TIME 0x27
#define DOS_CMD_LOAD_INTO_RAMDISK 0x41
#define DOS_CMD_SAVE_RAMDISK 0x42
#define DOS_CMD_ECHO 0xf0

// Control layer commands (names match firmware control_target.h)
#define CTRL_CMD_IDENTIFY      0x01
#define CTRL_CMD_READ_RTC      0x02
#define CTRL_CMD_FINISH_CAPTURE 0x03
#define CTRL_CMD_FREEZE        0x05
#define CTRL_CMD_REBOOT        0x06
#define CTRL_CMD_U64_SAVEMEM   0x0F
#define CTRL_CMD_DECODE_TRACK  0x11
#define CTRL_CMD_ENCODE_TRACK  0x12
#define CTRL_CMD_EASYFLASH     0x20
#define CTRL_CMD_GET_HWINFO    0x28
#define CTRL_CMD_GET_DRVINFO   0x29
#define CTRL_CMD_DEVICE_INFO   CTRL_CMD_GET_DRVINFO  // backward-compat alias
#define CTRL_CMD_ENABLE_DISK_A  0x30
#define CTRL_CMD_DISABLE_DISK_A 0x31
#define CTRL_CMD_ENABLE_DISK_B  0x32
#define CTRL_CMD_DISABLE_DISK_B 0x33
#define CTRL_CMD_DRIVE_A_POWER  0x34
#define CTRL_CMD_DRIVE_B_POWER  0x35
#define CTRL_CMD_GET_RAMDISK_INFO 0x40

// Network layer commands
#define NET_CMD_GET_INTERFACE_COUNT 0x02
#define NET_CMD_GET_IP_ADDRESS 0x05
#define NET_CMD_TCP_SOCKET_CONNECT 0x07
#define NET_CMD_UDP_SOCKET_CONNECT 0x08
#define NET_CMD_SOCKET_CLOSE 0x09
#define NET_CMD_SOCKET_READ 0x10
#define NET_CMD_SOCKET_WRITE 0x11
#define NET_CMD_TCP_LISTENER_START 0x12
#define NET_CMD_TCP_LISTENER_STOP 0x13
#define NET_CMD_GET_LISTENER_STATE 0x14
#define NET_CMD_GET_LISTENER_SOCKET 0x15
#define NET_LISTENER_STATE_NOT_LISTENING  0x00
#define NET_LISTENER_STATE_NOT_LISTENEING 0x00  // kept for backward compat; was a typo
#define NET_LISTENER_STATE_LISTENING 0x01
#define NET_LISTENER_STATE_CONNECTED 0x02
#define NET_LISTENER_STATE_BIND_ERROR 0x03
#define NET_LISTENER_STATE_PORT_IN_USE 0x04

// Uncomment for debug output
//#define DEBUG

// Macro for checking if last command was successful
#define UII_SUCCESS (uii_status[0] == '0' && uii_status[1] == '0')

// Global variables
extern char uii_status[STATUS_QUEUE_SZ + 1];
extern char uii_data[DATA_QUEUE_SZ  + 1];
extern char temp_string_onechar[2];
extern unsigned uii_data_index;
extern unsigned uii_data_len;
extern char uii_target;
struct DevInfo
{
	// Structure to parse drive info
	char exist;
	// Does it exist? 0 = no 1 = yes
	char type;
	// Type (hex):
	// 00 = 1541
	// 01 - 1571
	// 02 = 1581
	// 0F = SoftIEC
	// 50 = Printer
	char power;
	// Power 0 = off 1 = on
	char id;
	// IEC ID
};
extern struct DevInfo uii_devinfo[4];
// Array of info per device
// 0 = drive A
// 1 = drive B
// 2 = SoftIEC
// 3 = Printerindo

// prototypes

// uii_detect — Check whether the UCI hardware is present at $DF1D.
// Output: 1 if detected (id register reads 0xC9; UCI is also reset via uii_abort()), 0 if not present.
// Syntax: if (!uii_detect()) { /* no Ultimate cartridge present */ }
char uii_detect(void);

// uii_settarget — Set the target subsystem (DOS1/DOS2/NETWORK/CONTROL) for the next command.
// Input:  id — target ID: TARGET_DOS1, TARGET_DOS2, TARGET_NETWORK, or TARGET_CONTROL.
// Output: none.
// Syntax: uii_settarget(TARGET_CONTROL);
void uii_settarget(char id);

// uii_freeze — Trigger a freeze (cartridge freeze-button equivalent) via the control interface.
// Output: none.
// Syntax: uii_freeze();
void uii_freeze(void);

// uii_identify — Query the UCI firmware identification string (e.g. "ULTIMATE-II DOS V1.4").
// Output: identification string in uii_data[]; status is always "00,OK".
// Syntax: uii_identify();
void uii_identify(void);

// uii_echo — Test command: sends the command packet and receives it back as data.
// Output: echo of the sent command in uii_data[]; status is always "00,OK".
// Syntax: uii_echo();
void uii_echo(void);

// uii_getinterfacecount — Get the number of network interfaces on the Ultimate device.
// Output: interface count in uii_data[0]; the previously active target is saved and restored.
// Syntax: uii_getinterfacecount();
void uii_getinterfacecount(void);

// uii_sendcommand — Send a raw command packet to the UCI (lowest-level send primitive); waits for the UCI to be idle and retries if the error bit is set.
// Input:  bytes — command buffer; bytes[0] is overwritten with the current uii_target, bytes[1] is the opcode, remaining bytes are arguments.
// Input:  count — total number of bytes to send, including the target and opcode bytes.
// Output: none.
// Syntax: char cmd[] = {0x00, DOS_CMD_IDENTIFY}; uii_sendcommand(cmd, 2);
void uii_sendcommand(char *bytes, unsigned count);

// uii_accept — Acknowledge completion of a UCI response and release the UCI for the next command.
// Output: none; must be called after draining all response data and status.
// Syntax: uii_readdata(); uii_readstatus(); uii_accept();
void uii_accept(void);

// uii_isdataavailable — Check whether the UCI data FIFO has bytes to read (status bit 7).
// Output: 1 if data is available, 0 if not.
// Syntax: while (uii_isdataavailable()) { uii_readdata(); uii_accept(); }
char uii_isdataavailable(void);

// uii_ismoredataavailable — Check whether more packets follow in a multi-packet transfer (status bits 4 and 5 both set).
// Output: 1 if more packets follow, 0 if this is the last packet.
// Syntax: while (uii_isdataavailable() || uii_ismoredataavailable()) { uii_readdata(); uii_accept(); }
char uii_ismoredataavailable(void);

// uii_isstatusdataavailable — Check whether the UCI status FIFO has bytes to read (status bit 6).
// Output: 1 if status data is available, 0 if not.
// Syntax: while (uii_isstatusdataavailable()) { uii_readstatus(); }
char uii_isstatusdataavailable(void);

// uii_abort — Abort the current UCI operation by setting the ABORT bit in the control register.
// Output: none; does not wait for confirmation.
// Syntax: uii_abort();
void uii_abort(void);

// uii_readdata — Drain the UCI response data FIFO into uii_data[].
// Output: number of bytes read (0 if no data available); uii_data[] is always null-terminated.
// Syntax: unsigned n = uii_readdata();
unsigned uii_readdata(void);

// uii_readstatus — Drain the UCI status FIFO into uii_status[].
// Output: number of status bytes read; uii_status[] is always null-terminated. UII_SUCCESS is valid after this call.
// Syntax: uii_readstatus(); if (UII_SUCCESS) { ... }
unsigned uii_readstatus(void);

#pragma compile("ultimate_common_lib.c")

#endif
