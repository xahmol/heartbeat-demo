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
#include "ultimate_common_lib.h"
#include "ultimate_dos_lib.h"

// Switching code generation to bank 0 common routine section
#pragma code(code)
#pragma data(data)

void uii_get_path(void)
// uii_get_path — Get the current path.
// The "Get Path" command will return the current path in the file system, starting from the root. The
// path string is returned as a data packet. The status channel reports "00,OK", as this command can
// never fail.
// Output: current path string in uii_data[]; status always "00,OK".
// Syntax: uii_get_path();
{
	char cmd[] = {0x00, DOS_CMD_GET_PATH};
	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);
	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_open_dir(void)
// uii_open_dir — Open a directory for streaming via uii_get_dir().
// The "Open Directory" command will attempt to start reading the current directory. The command will
// not return any data, but it will return status information: "00,OK", "01,DIRECTORY EMPTY", or, if
// there was an error: "86,CAN'T READ DIRECTORY".
// Output: none; status as described above.
// Syntax: uii_open_dir(); if (UII_SUCCESS) uii_get_dir();
{
	char cmd[] = {0x00, DOS_CMD_OPEN_DIR};
	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);
	uii_readstatus();
	uii_accept();
}

void uii_get_dir(void)
// uii_get_dir — Read a directory: stream its contents to the data channel.
// The "Read Directory" command will return the contents of the directory to the data channel. Each
// entry of the directory is transmitted as a data packet. The format is simple: The first char gives the
// attribute of the directory entry, followed by the file name. The attribute has the following fields:
// Bit 7	Bit 6	Bit 5	Bit 4	Bit 3	Bit 2	Bit 1	Bit 0
// 				ARCHIVE	DIR		VOLUME	SYSTEM	HIDDEN	READONLY
// These fields are taken from the attribute char as it exists in FAT directories, and is reused for other
// non-FAT directories.
// Read this in a loop, and _accept() the data
// in order to get the next packet
//
// Each data packet is 512 bytes each
// Output: none directly; drain with uii_isdataavailable()/uii_readdata()/uii_accept() in a loop — uii_data[0] is the attribute byte, uii_data+1 is the entry name.
// Syntax: uii_get_dir(); while (uii_isdataavailable()) { uii_readdata(); uii_accept(); }
{
	char cmd[] = {0x00, DOS_CMD_READ_DIR};
	unsigned count = 0;
	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);
}

void uii_change_dir(char *directory)
// uii_change_dir — Change the current directory.
// The 'Change Directory" command is used to let the DOS enter a sub directory. When the DOS starts, the
// current directory will be the root of the SdCard. The parameter given is the name of the directory to
// enter. Like Windows, Linux and MacOS, the names "." and ".." have special meaning: current and parent
// directory.
// With this command, it is also possible to enter files that have sub-entries, such as ".D64" files. These
// files are treated as sub file systems, and therefore commands as "File Open" will also work on files
// within.
// This command does never return any data. The status channel will tell whether the operation was
// successful. The two possible responses are: "00,OK", or "83,NO SUCH DIRECTORY".
// Input:  directory — subdirectory name, ".." for parent, "/" for root, or a sub-filesystem file (e.g. ".D64") to enter it.
// Output: none; status "00,OK" or "83,NO SUCH DIRECTORY".
// Syntax: uii_change_dir("mygame");
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(directory) + 2);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_CHANGE_DIR;

	for (x = 0; x < strlen(directory); x++)
		fullcmd[x + 2] = directory[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(directory) + 2);

	free(fullcmd);

	uii_readstatus();
	uii_accept();
}

void uii_create_dir(char *directory)
// uii_create_dir — Create a new directory.
// The "Create Dir" command creates the specified directory in the current path.
// This command does not return any data. The status channel will either read "00,OK" or it will contain
// the appropriate filesystem error message.
// Input:  directory — name of the new directory to create.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_create_dir("saves");
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(directory) + 2);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_CREATE_DIR;

	for (x = 0; x < strlen(directory); x++)
		fullcmd[x + 2] = directory[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(directory) + 2);

	free(fullcmd);

	uii_readstatus();
	uii_accept();
}

void uii_change_dir_home(void)
// uii_change_dir_home — Change to the home directory.
// The "Copy Home Path" command changes into the user defined home directory specified in the "Home
// Directory" setting under "User Interface Settings". If the directory does not exist, the appropriate
// filesystem error message is reported on the status channel.
// The command is executed and then falls through to the "Get Path" command; thus it will return the
// current path which the file browser is at.
// Output: none directly; current path ends up in uii_data[] via the fall-through Get Path. Status "00,OK" or a filesystem error.
// Syntax: uii_change_dir_home();
{
	char cmd[] = {0x00, DOS_CMD_COPY_HOME_PATH};
	unsigned count = 0;

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);
	uii_readstatus();
	uii_accept();
}

void uii_mount_disk(char id, char *filename)
// uii_mount_disk — Mount a disk image.
// The "Mount Disk" command mounts the disk image specified by the <filename> argument on the
// drive using the IEC-ID specified by the single char argument <id>.
// If there is no drive using the specified id, then the drive last mounted on will be used. If there is no
// such drive, the status channel reports "90,DRIVE NOT PRESENT".
// If the file denoted by <filename> is not a disk image, the status channel reports "89,NOT A DISK
// IMAGE".
// On successful mount the status channel reports "00,OK". This command never returns any data.
// Input:  id — IEC device ID of the target drive.
// Input:  filename — name of the disk image file (must be in the current UCI path).
// Output: none; status "00,OK", "89,NOT A DISK IMAGE", or "90,DRIVE NOT PRESENT".
// Syntax: uii_mount_disk(8, "game.d64");
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(filename) + 3);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_MOUNT_DISK;
	fullcmd[2] = id;

	for (x = 0; x < strlen(filename); x++)
		fullcmd[x + 3] = filename[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(filename) + 3);

	free(fullcmd);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_unmount_disk(char id)
// uii_unmount_disk — Unmount a disk image.
// The "Umount Disk" command unmounts the disk currently mounted on the drive using the IEC-ID
// specified by the single char argument <id>.
// If there is no drive using the specified id, then the drive last mounted on will be used. If there is no
// such drive, the status channel reports "90,DRIVE NOT PRESENT".
// On successful unmount the status channel reports "00,OK". The command is considered successful
// even if no disk was mounted beforehand. This command never returns any data.
// Input:  id — IEC device ID of the drive to unmount.
// Output: none; status "00,OK" (also on no disk mounted) or "90,DRIVE NOT PRESENT".
// Syntax: uii_unmount_disk(8);
{
	char cmd[] = {0x00, DOS_CMD_UMOUNT_DISK, 0x00};

	cmd[2] = id;

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 3);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_open_file(char attrib, char *filename)
// uii_open_file — Open a file.
//
// Attrib will be:
// 0x01 = Read
// 0x02 = Write
// 0x06 = Create new file
// 0x0E = Create (overwriting an existing file)
//
// Full explanation:
// The "Open File" command takes two arguments: an attribute byte; directly followed by the filename to
// be opened. The attribute char contains flags that tell the file system how, in which mode, to open the
// file. The following table shows which flags are applicable:
// Attribute Value
// 		FA_READ				$01
// 		FA_WRITE			$02
// 		FA_CREATE_NEW		$04
// 		FA_CREATE_ALWAYS	$08
// To open a file in read mode, only just use $01. To open a file in write mode, use $02 if the file you write
// to already exists. This mode will not clear the file. Add $04 if you would like to create a new file to
// write; this mode will clear the file to 0 bytes first. Add $08 if the file you open may overwrite a file that
// already exists. (So, in order to open a file for writing that may always overwrite an existing file, use
// $0E.)
// The filename does not need to be null-terminated, as the length of the command determines the length
// of the file name string.
//
// The command will never return data. Status will either be "00,OK", or a status message from the file
// system.
// Input:  attrib — open mode flags (FA_READ=0x01, FA_WRITE=0x02, FA_CREATE_NEW=0x04, FA_CREATE_ALWAYS=0x08; e.g. 0x06=create new, 0x0E=write-or-overwrite).
// Input:  filename — name of the file to open (null-terminated C string).
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_open_file(0x01, "song.hbt");
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(filename) + 3);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_OPEN_FILE;
	fullcmd[2] = attrib;

	for (x = 0; x < strlen(filename); x++)
		fullcmd[x + 3] = filename[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(filename) + 3);

	free(fullcmd);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_close_file(void)
// uii_close_file — Close the currently opened file.
// The "Close File" command closes the file that was last opened. It does not take any arguments, neither
// will this command return any data. The status channel will read:
// "00,OK" or "84,NO FILE TO CLOSE"
// Output: none; status "00,OK" or "84,NO FILE TO CLOSE".
// Syntax: uii_close_file();
{
	char cmd[] = {0x00, DOS_CMD_CLOSE_FILE};

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_write_file(char *data, unsigned length)
// uii_write_file — Write data to the currently opened file.
// The "Write Data" command will write to the file that is currently open. If there is no file open, the
// status channel will read "85,NO FILE OPEN". If the file is not opened for writing, the file system will
// return "ACCESS DENIED" onto the status channel. The command will never return data.
// Input:  data — pointer to the buffer to write.
// Input:  length — number of bytes to write; must not exceed DATA_QUEUE_SZ - 4 (508) bytes per call — split larger writes into a loop.
// Output: none; status "00,OK", "85,NO FILE OPEN", or "ACCESS DENIED".
// Syntax: uii_write_file(buf, 256);
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(length + 4);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_WRITE_DATA;
	fullcmd[2] = 0x00;
	fullcmd[3] = 0x00;

	for (x = 0; x < length; x++)
		fullcmd[x + 4] = data[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, length + 4);

	free(fullcmd);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_read_file(unsigned length)
// uii_read_file — Read data from the currently opened file.
// The "Read Data" command will start a read transfer from the opened file. If there is no file open, the
// reply will be an empty data packet, and the status channel will read "85,NO FILE OPEN".
// When something goes wrong, this will be reported through the status channel. When everything is
// okay, the status channel will stay quiet.
// As with _get_dir(), read this in a loop, and _accept() the data
// in order to get the next packet
//
// Each data packet is 512 bytes each
// Input:  length — maximum number of bytes to request.
// Output: none directly; drain the response in a loop with uii_isdataavailable()/uii_ismoredataavailable(), uii_readdata(), and uii_accept().
// Syntax: uii_read_file(512); while (uii_isdataavailable() || uii_ismoredataavailable()) { uii_readdata(); uii_accept(); }
{
	char cmd[] = {0x00, DOS_CMD_READ_DATA, 0x00, 0x00};

	cmd[2] = length & 0xFF;
	cmd[3] = length >> 8;

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 4);
}

void uii_seek_file(char posL, char posML, char posMH, char posH)
// uii_seek_file — Seek to a position in the currently opened file.
// The "File Seek" command places the pointer into the currently opened file at a user-defined position.
// The command takes one argument: a 32 bit value, which is transferred LSB first.
// The command never returns any data. When the seek is successful, status returns "00,OK", or else a
// message from the file system. If there is no file open, the status channel will read "85,NO FILE OPEN"
// Input:  posL — bits 7-0 of the 32-bit file offset.
// Input:  posML — bits 15-8 of the offset.
// Input:  posMH — bits 23-16 of the offset.
// Input:  posH — bits 31-24 of the offset.
// Output: none; status "00,OK", "85,NO FILE OPEN", or a filesystem error.
// Syntax: uii_seek_file(0x00, 0x04, 0x00, 0x00); // seek to byte 1024
{
	char cmd[] = {0x00, DOS_CMD_FILE_SEEK, posL, posML, posMH, posH};

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 6);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_file_info()
// uii_file_info — Get information about the currently open file.
// The "File Info" command returns a data packet with information about the currently open file. In fact,
// this command executes a file stat command. The format of the data packet is as follows:
//  DWORD size; /* File size */
//  WORD date; /* Last modified date */
//  WORD time; /* Last modified time */
//  char extension[3];
//  char attrib; /* Attribute */
//  char filename[ ];
// Ultimate DOS Command Summary
// Version 1.2, December 16th, 2017 6
// The status response could either be:
// "00,OK", "85,NO FILE OPEN", or "88,NO INFORMATION AVAILABLE"
// Output: data packet described above in uii_data[]; status "00,OK", "85,NO FILE OPEN", or "88,NO INFORMATION AVAILABLE".
// Syntax: uii_file_info();
{
	char cmd[] = {0x00, DOS_CMD_FILE_INFO};

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

unsigned long uii_file_size()
// uii_file_size — Get the size of the currently open file.
// The "File Size" command returns the size of the currently open file in bytes.
// Output: file size as a 32-bit unsigned integer. Calls uii_file_info() internally, which overwrites uii_data[].
// Syntax: unsigned long sz = uii_file_size();
{
	// First call uii_file_info to populate uii_data with the file info data packet
	uii_file_info();

	// The file size is the first 4 bytes of the data packet, so we can read it from uii_data
	unsigned long size = 0;
	size |= (unsigned char)uii_data[0];
	size |= (unsigned char)uii_data[1] << 8;
	size |= (unsigned char)uii_data[2] << 16;
	size |= (unsigned char)uii_data[3] << 24;

	// Return the file size
	return size;
}

void uii_file_stat(char *filename)
// uii_file_stat — Get information about a file.
// The "File Info" command returns a data packet with information about a file, specified by the 'filename'
// parameter. The format of the data packet is the same as for DOS_CMD_FILE_INFO (0x07).
// The status response could either be:
// "00,OK", or "88,FILE NOT FOUND"
// Input:  filename — name of the file to query.
// Output: same data packet layout as uii_file_info() in uii_data[]; status "00,OK" or "88,FILE NOT FOUND".
// Syntax: uii_file_stat("song.hbt");
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(filename) + 2);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_FILE_STAT;

	for (x = 0; x < strlen(filename); x++)
		fullcmd[x + 2] = filename[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(filename) + 2);

	free(fullcmd);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_delete_file(char *filename)
// uii_delete_file — Delete a file.
// The "Delete File" command deletes the specified file.
// This command does not return any data. The status channel will either read "00,OK" or it will contain
// the appropriate filesystem error message.
// Input:  filename — name of the file to delete.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_delete_file("old.dat");
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(filename) + 2);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_DELETE_FILE;

	for (x = 0; x < strlen(filename); x++)
		fullcmd[x + 2] = filename[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(filename) + 2);

	free(fullcmd);

	uii_readstatus();
	uii_accept();
}

void uii_rename_file(char *oldname, char *newname)
// uii_rename_file — Rename a file.
// The "Rename File" command renames the specified file.
// This command does not return any data. The status channel will either read "00,OK" or it will contain
// the appropriate filesystem error message.
// Input:  oldname — current filename.
// Input:  newname — new filename.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_rename_file("temp.dat", "final.dat");
{
	unsigned x = 0;
	unsigned count = 0;
	char *fullcmd = (char *)malloc(strlen(oldname) + strlen(newname) + 3);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_RENAME_FILE;

	count = 2;

	for (x = 0; x < strlen(oldname); x++)
		fullcmd[count++] = oldname[x];

	fullcmd[count++] = 0x00;

	for (x = 0; x < strlen(newname); x++)
		fullcmd[count++] = newname[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, count);

	free(fullcmd);

	uii_readstatus();
	uii_accept();
}

void uii_copy_file(char *source, char *destination)
// uii_copy_file — Copy a file.
// The "Copy File" command copies the file specified by <source> to the file specified by
// <destination>.
// This command does not return any data. The status channel will either read "00,OK" or it will contain
// the appropriate filesystem error message.
// Input:  source — path of the file to copy.
// Input:  destination — path of the new file.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_copy_file("song.hbt", "backup/song.hbt");
{
	unsigned x = 0;
	unsigned count = 0;
	char *fullcmd = (char *)malloc(strlen(source) + strlen(destination) + 3);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_COPY_FILE;

	count = 2;

	for (x = 0; x < strlen(source); x++)
		fullcmd[count++] = source[x];

	fullcmd[count++] = 0x00;

	for (x = 0; x < strlen(destination); x++)
		fullcmd[count++] = destination[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, count);

	free(fullcmd);

	uii_readstatus();
	uii_accept();
}

void uii_get_ramdisk_info(void)
// uii_get_ramdisk_info — Get information about GEOS RAM disks in REU.
// Output: RAM disk information: 8 bytes with 2 bytes for drive 8-10: drive ID and type
// Syntax: uii_get_ramdisk_info();
{
	char cmd[] = {0x00, CTRL_CMD_GET_RAMDISK_INFO};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_loadIntoRamDisk(char id, char *filename, char whatif)
// uii_loadIntoRamDisk — Load a file into the RAM disk.
// Input:  id — RAM disk ID (drive number, e.g. 8).
// Input:  filename — name of the file to load.
// Input:  whatif — 0 = perform the load; 1 = trial run only (checks size/type, no actual load).
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_loadIntoRamDisk(8, "disk.g64", 0);
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(filename) + 3);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_LOAD_INTO_RAMDISK;
	fullcmd[2] = id + (128 * whatif);

	for (x = 0; x < strlen(filename); x++)
		fullcmd[x + 3] = filename[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(filename) + 3);

	free(fullcmd);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_saveRamDisk(char id, char *filename)
// uii_saveRamDisk — Save a file from the RAM disk.
// Input:  id — RAM disk ID.
// Input:  filename — destination filename.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_saveRamDisk(8, "disk.g64");
{
	unsigned x = 0;
	char *fullcmd = (char *)malloc(strlen(filename) + 3);
	if (!fullcmd)
		return;
	fullcmd[0] = 0x00;
	fullcmd[1] = DOS_CMD_SAVE_RAMDISK;
	fullcmd[2] = id;

	for (x = 0; x < strlen(filename); x++)
		fullcmd[x + 3] = filename[x];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(fullcmd, strlen(filename) + 3);

	free(fullcmd);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_save_reu(unsigned long reu_addr, unsigned long size)
// uii_save_reu — Save REU memory to a file (file must already be open for writing).
// The "Save REU" command can be used to write data to the currently opened file from the REU
// memory. The command takes two 32-bit parameters. The first argument is the REU
// address from which the data is saved; the second gives the total number of bytes that shall be written.
// The save function does not wrap around; it is truncated when the start address plus the length exceeds
// the end address of the REU memory. The upper bytes of both the address as well as the length are
// masked out, thus effectively these bytes are dummy bytes.
// Note: This function assumes a 16 MB REU configuration.
// The status message is either "00,OK", "02,REQUEST TRUNCATED", or a message directly from the file
// system.
// The data that is returned is a more detailed string, indicating the number of bytes written from which
// address, such as: "$008000 BYTES SAVED FROM REU $852000".
// Input:  reu_addr — starting address in REU memory to save from.
// Input:  size — number of bytes to save.
// Output: transfer summary string in uii_data[]; status as described above.
// Syntax: uii_save_reu(0x852000UL, 0x8000UL);
{
	char cmd[10];
	cmd[0] = 0x00;
	cmd[1] = DOS_CMD_SAVE_REU;
	cmd[2] = (char)(reu_addr & 0xff);
	cmd[3] = (char)((reu_addr >> 8) & 0xff);
	cmd[4] = (char)((reu_addr >> 16) & 0xff);
	cmd[5] = (char)((reu_addr >> 24) & 0xff);
	cmd[6] = (char)(size & 0xff);
	cmd[7] = (char)((size >> 8) & 0xff);
	cmd[8] = (char)((size >> 16) & 0xff);
	cmd[9] = (char)((size >> 24) & 0xff);

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 10);
	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_load_reu(unsigned long reu_addr, unsigned long size)
// uii_load_reu — Load a file to REU memory.
// The "Load REU" command can be used to read data from the currently opened file into the REU
// memory. The command takes two 32-bit parameters, both LSB first. The first argument is the REU
// address at which the data is loaded, the second gives the total number of bytes that shall be read. The
// load function does not wrap around; the load is truncated when the start address plus the length
// exceeds the end address of the REU memory. The upper bytes of both the address as well as the length
// are masked out, thus effectively these bytes are dummy bytes.
// Note: This function assumes a 16 MB REU configuration.
// The status message is either "00,OK", "02,REQUEST TRUNCATED", or a message directly from the file
// system.
// Input:  reu_addr — destination address in REU memory to load to.
// Input:  size — number of bytes to load.
// Output: transfer summary string in uii_data[]; status as described above.
// Syntax: uii_load_reu(0x126800UL, 0x3000UL);
{
	char cmd[10];
	cmd[0] = 0x00;
	cmd[1] = DOS_CMD_LOAD_REU;
	cmd[2] = (char)(reu_addr & 0xff);
	cmd[3] = (char)((reu_addr >> 8) & 0xff);
	cmd[4] = (char)((reu_addr >> 16) & 0xff);
	cmd[5] = (char)((reu_addr >> 24) & 0xff);
	cmd[6] = (char)(size & 0xff);
	cmd[7] = (char)((size >> 8) & 0xff);
	cmd[8] = (char)((size >> 16) & 0xff);
	cmd[9] = (char)((size >> 24) & 0xff);

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 10);
	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_save_reu_image(char size)
// uii_save_reu_image — Save the whole REU (from address 0) to a file, sized by index rather than byte count.
// The "Save REU" command can be used to write data to the currently opened file from the REU
// memory.
// The status message is either "00,OK", "02,REQUEST TRUNCATED", or a message directly from the file
// system.
// The data that is returned is a more detailed string, indicating the number of bytes written from which
// address, such as: "$008000 BYTES SAVED FROM REU $852000".
// Input:  size — the size of the REU to save: 0=128KB, 1=256KB, 2=512KB, 3=1MB, 4=2MB, 5=4MB, 6=8MB, 7=16MB.
// Output: transfer summary string in uii_data[]; status as described above.
// Syntax: uii_save_reu_image(7); // save full 16 MB REU
{
	char cmd[] = {0x00, DOS_CMD_SAVE_REU, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x01};
	char sizes[8] = {0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff};

	cmd[8] = sizes[size];
	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 10);
	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_load_reu_image(char size)
// uii_load_reu_image — Load the whole REU (into address 0) from a file, sized by index rather than byte count.
// The "Load REU" command can be used to read data from the currently opened file into the REU
// memory.
// The status message is either "00,OK", "02,REQUEST TRUNCATED", or a message directly from the file
// system.
// The data that is returned is a more detailed string, indicating the number of bytes read at which
// address, such as: "$003000 BYTES LOADED TO REU $126800"
// Input:  size — the size of the REU to load: 0=128KB, 1=256KB, 2=512KB, 3=1MB, 4=2MB, 5=4MB, 6=8MB, 7=16MB.
// Output: transfer summary string in uii_data[]; status as described above.
// Syntax: uii_load_reu_image(7); // load full 16 MB REU
{
	char cmd[] = {0x00, DOS_CMD_LOAD_REU, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x01};
	char sizes[8] = {0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff};

	cmd[8] = sizes[size];

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 10);
	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_enable_drive_a(void)
// uii_enable_drive_a — Enable drive A.
// Output: none; status "00,OK". Allow the drive a moment to spin up before mounting or accessing it.
// Syntax: uii_enable_drive_a();
{
	char cmd[] = {0x00, CTRL_CMD_ENABLE_DISK_A};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_disable_drive_a(void)
// uii_disable_drive_a — Disable drive A.
// Output: none; status "00,OK".
// Syntax: uii_disable_drive_a();
{
	char cmd[] = {0x00, CTRL_CMD_DISABLE_DISK_A};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_enable_drive_b(void)
// uii_enable_drive_b — Enable drive B.
// Output: none; status "00,OK". Allow the drive a moment to spin up before mounting or accessing it.
// Syntax: uii_enable_drive_b();
{
	char cmd[] = {0x00, CTRL_CMD_ENABLE_DISK_B};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_disable_drive_b(void)
// uii_disable_drive_b — Disable drive B.
// Output: none; status "00,OK".
// Syntax: uii_disable_drive_b();
{
	char cmd[] = {0x00, CTRL_CMD_DISABLE_DISK_B};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_get_drive_a_power(void)
// uii_get_drive_a_power — Get the power status of drive A.
// Output: none directly; uii_data[0] = 0 (off) or 1 (on).
// Syntax: uii_get_drive_a_power();
{
	char cmd[] = {0x00, CTRL_CMD_DRIVE_A_POWER};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_get_drive_b_power(void)
// uii_get_drive_b_power — Get the power status of drive B.
// Output: none directly; uii_data[0] = 0 (off) or 1 (on).
// Syntax: uii_get_drive_b_power();
{
	char cmd[] = {0x00, CTRL_CMD_DRIVE_B_POWER};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_get_deviceinfo(void)
// uii_get_deviceinfo — Get device information.
// Output: raw byte sequence describing connected drives in uii_data[]; parse with uii_parse_deviceinfo().
// Syntax: uii_get_deviceinfo();
{
	char cmd[] = {0x00, CTRL_CMD_GET_DRVINFO};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

char uii_parse_deviceinfo(void)
// uii_parse_deviceinfo — Parse the device information into uii_devinfo[].
// Output: 1 on success (uii_devinfo[] populated for drive A, drive B, SoftIEC, and soft printer as present), 0 on failure (UCI error or no devices found).
// Syntax: if (uii_parse_deviceinfo()) { /* uii_devinfo[0..3] now valid */ }
{
	char devicecount, count, temp;

	// Execute UCI 29 : CTRL_CMD_GET_DRVINFO
	uii_get_deviceinfo();

	// Return with success code = 0 if no success
	if (!UII_SUCCESS)
	{
		return 0;
	}

	// Get number of devices to parse
	devicecount = uii_data[0];
	if (!devicecount)
	{
		return 0;
	}

	// Parse first type
	count = 1;
	temp = uii_data[count++];

	// Parse drive A
	if (temp < 0x0f)
	{
		// Drive A found
		uii_devinfo[0].exist = 1;
		uii_devinfo[0].type = temp;
		uii_devinfo[0].id = uii_data[count++];
		uii_devinfo[0].power = uii_data[count++];
		temp = uii_data[count++];
	}

	// Parse drive B
	if (temp < 0x0f)
	{
		// Drive B found
		uii_devinfo[1].exist = 1;
		uii_devinfo[1].type = temp;
		uii_devinfo[1].id = uii_data[count++];
		uii_devinfo[1].power = uii_data[count++];
		temp = uii_data[count++];
	}

	// Parse SoftIEC
	if (temp == 0x0f)
	{
		// SoftIEC
		uii_devinfo[2].exist = 1;
		uii_devinfo[2].type = temp;
		uii_devinfo[2].id = uii_data[count++];
		uii_devinfo[2].power = uii_data[count++];
		temp = uii_data[count++];
	}

	// Parse soft printer
	if (temp == 0x50)
	{
		// SoftPrinter
		uii_devinfo[3].exist = 1;
		uii_devinfo[3].type = temp;
		uii_devinfo[3].id = uii_data[count++];
		uii_devinfo[3].power = uii_data[count];
	}

	return 1;
}

char *uii_device_type(char typeval)
// uii_device_type — Convert device type value to string.
// Input:  typeval — type value from uii_devinfo[n].type.
// Output: pointer to a string literal: "1541", "1571", "1581", "SoftIEC", "Printer", or "" if unknown.
// Syntax: char *name = uii_device_type(uii_devinfo[0].type);
{
	switch (typeval)
	{
	case 0x00:
		return "1541";
	case 0x01:
		return "1571";
	case 0x02:
		return "1581";
	case 0x0F:
		return "SoftIEC";
	case 0x50:
		return "Printer";
	default:
		return "";
	}
}

void uii_swap_disk(void)
// uii_swap_disk — Swap the disk mounted on drive A with drive B.
// The "Swap Disk" command swaps the disk images mounted on drive A and drive B.
// This command does not return any data.
// Output: none; status "00,OK".
// Syntax: uii_swap_disk();
{
	char cmd[] = {0x00, DOS_CMD_SWAP_DISK, 0x00};

	uii_settarget(TARGET_DOS1);
	uii_sendcommand(cmd, 3);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

void uii_reboot(void)
// uii_reboot — Reboot the C64 via the cartridge.
// Triggers a clean C64 reset through the Ultimate control interface.
// This command does not return — the machine resets immediately.
// Output: none; the call does not return.
// Syntax: uii_reboot();
{
	char cmd[] = {0x00, CTRL_CMD_REBOOT};

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 2);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}

// Reusable path construction buffer — module-level static to avoid heap pressure.
// heapsize is only 256 bytes; uii_change_dir already mallocs the same string internally.
static char _uii_fp[128];
static char _uii_root[] = "/";   // non-const for uii_change_dir

static char _uii_lc(char c)
// _uii_lc — Lowercase an ASCII letter; other characters pass through unchanged.
// Input:  c — character to lowercase.
// Output: lowercase equivalent of c if c is 'A'-'Z', otherwise c unchanged.
// Syntax: char lower = _uii_lc('S'); // 's'
{
    return (c >= 'A' && c <= 'Z') ? (char)(c | 0x20) : c;
}

char uii_scan_media(char drives[UII_MAX_DRIVES][UII_DRIVE_PATH_LEN], char *count)
// uii_scan_media — Scan UCI root "/" for user-accessible storage: directories whose name starts with
// "sd" or "usb" (case-insensitive, FAT DIR attribute bit 4 set).
// Fills drives[0..n-1] with lowercase slash-delimited paths e.g. "/usb0/", "/sd/".
// Sets *count to number found. Leaves CWD at "/".
// Input:  drives — 2-D array to receive found paths; dimensioned [UII_MAX_DRIVES][UII_DRIVE_PATH_LEN].
// Input:  count — pointer to receive the number of drives found.
// Output: 1 on success, 0 if root directory could not be opened.
// Syntax: char drives[UII_MAX_DRIVES][UII_DRIVE_PATH_LEN]; char n; uii_scan_media(drives, &n);
{
    char n0, n1, n2;
    char j;
    char *name;

    *count = 0;

    uii_change_dir(_uii_root);
    uii_open_dir();
    if (!UII_SUCCESS)
        return 0;

    uii_get_dir();
    while (uii_isdataavailable())
    {
        uii_readdata();
        uii_accept();

        if (!(uii_data[0] & 0x10))  // bit 4 = directory
            continue;

        name = uii_data + 1;        // null-terminated ASCII name
        if (!name[0])
            continue;

        n0 = _uii_lc(name[0]);
        n1 = _uii_lc(name[1]);
        n2 = _uii_lc(name[2]);

        if (!((n0 == 's' && n1 == 'd') ||
              (n0 == 'u' && n1 == 's' && n2 == 'b')))
            continue;

        if (*count >= UII_MAX_DRIVES)
            continue;

        // Build "/lowercasename/"
        drives[*count][0] = '/';
        for (j = 0; name[j] && j < UII_DRIVE_PATH_LEN - 3; j++)
            drives[*count][j + 1] = _uii_lc(name[j]);
        drives[*count][j + 1] = '/';
        drives[*count][j + 2] = 0;
        (*count)++;
    }
    return 1;
}

char uii_find_media_path(char drives[UII_MAX_DRIVES][UII_DRIVE_PATH_LEN], char drv_count,
                          char *subpath, char *result)
// uii_find_media_path — Search subpath under each drive in drives[0..drv_count-1].
// On first match: fills result[] with full path, leaves CWD there, returns 1.
// Returns 0 if not found on any drive; sets result[0]=0.
// Input:  drives — drive array populated by uii_scan_media().
// Input:  drv_count — number of valid entries in drives.
// Input:  subpath — relative subpath to search for, e.g. "mygame/data/".
// Input:  result — buffer to receive the full found path; must hold at least UII_DRIVE_PATH_LEN + strlen(subpath) + 1 bytes.
// Output: 1 if found (CWD is now at the found path), 0 if not found on any drive.
// Syntax: char result[32]; if (uii_find_media_path(drives, n, "mygame/data/", result)) { ... }
{
    char i;
    unsigned char dlen, slen;

    result[0] = 0;
    slen = (unsigned char)strlen(subpath);

    for (i = 0; i < drv_count; i++)
    {
        dlen = (unsigned char)strlen(drives[i]);
        if ((unsigned char)(dlen + slen) >= 127)
            continue;               // path too long for buffer; skip

        memcpy(_uii_fp, drives[i], dlen);
        memcpy(_uii_fp + dlen, subpath, (unsigned)(slen + 1));  // includes null

        uii_change_dir(_uii_fp);
        if (UII_SUCCESS)
        {
            strcpy(result, _uii_fp);
            return 1;
        }
    }
    return 0;
}

void uii_get_hwinfo(char device)
// uii_get_hwinfo — Get hardware information from the Ultimate cartridge.
// Input:  device — 0 = product identification string (e.g. "Ultimate 64", "1541 Ultimate II+");
//                  1 = SID chip configuration (byte 0: count; per SID: addr_lo, addr_hi, bits, rsvd, rsvd).
// Output: none directly; result returned in uii_data[].
// Syntax: uii_get_hwinfo(0);
{
	char cmd[] = {0x00, CTRL_CMD_GET_HWINFO, 0x00};

	cmd[2] = device;

	uii_settarget(TARGET_CONTROL);
	uii_sendcommand(cmd, 3);

	uii_readdata();
	uii_readstatus();
	uii_accept();
}
