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

#ifndef _ULTIMATE_DOS_LIB_H_
#define _ULTIMATE_DOS_LIB_H_

// prototypes

// uii_get_path — Read the current UCI filesystem path into uii_data[], starting from the root.
// Output: current path string in uii_data[]; status is always "00,OK".
// Syntax: uii_get_path();
void uii_get_path(void);

// uii_open_dir — Start reading the current directory for streaming via uii_get_dir().
// Output: none (no data); status "00,OK", "01,DIRECTORY EMPTY", or "86,CAN'T READ DIRECTORY".
// Syntax: uii_open_dir(); if (UII_SUCCESS) uii_get_dir();
void uii_open_dir(void);

// uii_get_dir — Stream the contents of the currently open directory to the data channel, one entry per packet.
// Output: none directly; drain with uii_readdata()/uii_accept() in a loop — uii_data[0] is the FAT attribute byte (bit 4 = directory), uii_data+1 is the null-terminated entry name.
// Syntax: uii_get_dir(); while (uii_isdataavailable()) { uii_readdata(); uii_accept(); }
void uii_get_dir(void);

// uii_change_dir — Change the current directory; also enters disk image sub-filesystems (e.g. ".D64" files).
// Input:  directory — subdirectory name, ".." for parent, "/" for root, or a disk image filename to enter it as a sub-filesystem.
// Output: none; status "00,OK" or "83,NO SUCH DIRECTORY".
// Syntax: uii_change_dir("mygame");
void uii_change_dir(char *directory);

// uii_create_dir — Create a new directory in the current path.
// Input:  directory — name of the directory to create.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_create_dir("saves");
void uii_create_dir(char *directory);

// uii_change_dir_home — Navigate to the UCI home directory configured in the Ultimate's User Interface Settings.
// Output: none; falls through to "Get Path" so the current path ends up in uii_data[]. Status "00,OK" or a filesystem error if the home directory does not exist.
// Syntax: uii_change_dir_home();
void uii_change_dir_home(void);

// uii_mount_disk — Mount a disk image file on an emulated IEC drive.
// Input:  id — IEC device ID of the target drive (e.g. 8).
// Input:  filename — name of the disk image file in the current UCI path.
// Output: none; status "00,OK", "89,NOT A DISK IMAGE", or "90,DRIVE NOT PRESENT".
// Syntax: uii_mount_disk(8, "game.d64");
void uii_mount_disk(char id, char *filename);

// uii_unmount_disk — Unmount the disk image from the drive with the given IEC ID.
// Input:  id — IEC device ID of the drive to unmount.
// Output: none; status "00,OK" (also reported if no disk was mounted) or "90,DRIVE NOT PRESENT".
// Syntax: uii_unmount_disk(8);
void uii_unmount_disk(char id);

// uii_swap_disk — Swap the disk images mounted on drive A and drive B.
// Output: none; status "00,OK".
// Syntax: uii_swap_disk();
void uii_swap_disk(void);

// uii_open_file — Open a file on the current path in the given mode.
// Input:  attrib — open mode flags: 0x01=read, 0x02=write existing, 0x04=create new (truncate), 0x08=allow overwrite (0x06=create new for writing, 0x0E=write, create or overwrite).
// Input:  filename — name of the file to open.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_open_file(0x01, "song.hbt");
void uii_open_file(char attrib, char *filename);

// uii_close_file — Close the currently open file.
// Output: none; status "00,OK" or "84,NO FILE TO CLOSE".
// Syntax: uii_close_file();
void uii_close_file(void);

// uii_write_file — Write bytes to the currently open file.
// Input:  data — pointer to the buffer to write.
// Input:  length — number of bytes to write; must not exceed DATA_QUEUE_SZ - 4 (508) bytes per call.
// Output: none; status "00,OK", "85,NO FILE OPEN", or "ACCESS DENIED".
// Syntax: uii_write_file(buf, 256);
void uii_write_file(char *data, unsigned length);

// uii_read_file — Request up to length bytes from the currently open file; does not read the data itself.
// Input:  length — maximum number of bytes to request.
// Output: none directly; drain the response in a loop with uii_isdataavailable()/uii_ismoredataavailable(), uii_readdata(), and uii_accept().
// Syntax: uii_read_file(512); while (uii_isdataavailable() || uii_ismoredataavailable()) { uii_readdata(); uii_accept(); }
void uii_read_file(unsigned length);

// uii_seek_file — Move the read/write pointer to a position in the currently open file.
// Input:  posL — bits 7-0 of the 32-bit file offset.
// Input:  posML — bits 15-8 of the offset.
// Input:  posMH — bits 23-16 of the offset.
// Input:  posH — bits 31-24 of the offset.
// Output: none; status "00,OK", "85,NO FILE OPEN", or a filesystem error.
// Syntax: uii_seek_file(0x00, 0x04, 0x00, 0x00); // seek to byte 1024
void uii_seek_file(char posL, char posML, char posMH, char posH); // Seek to 4-byte file position

// uii_file_info — Get metadata (size, date, time, extension, attributes, name) for the currently open file.
// Output: none directly; data packet in uii_data[] (see uii_file_size() for the size field layout). Status "00,OK", "85,NO FILE OPEN", or "88,NO INFORMATION AVAILABLE".
// Syntax: uii_file_info();
void uii_file_info();

// uii_file_size — Get the byte size of the currently open file.
// Output: file size as a 32-bit unsigned integer (0 if no file is open). Calls uii_file_info() internally, which overwrites uii_data[].
// Syntax: unsigned long sz = uii_file_size();
unsigned long uii_file_size();

// uii_file_stat — Get metadata about a named file without opening it.
// Input:  filename — name of the file to query.
// Output: none directly; same data packet layout as uii_file_info(). Status "00,OK" or "88,FILE NOT FOUND".
// Syntax: uii_file_stat("song.hbt");
void uii_file_stat(char *filename);

// uii_delete_file — Delete the named file from the current directory.
// Input:  filename — name of the file to delete.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_delete_file("old.dat");
void uii_delete_file(char *filename);

// uii_rename_file — Rename a file in the current directory.
// Input:  oldname — current filename.
// Input:  newname — new filename.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_rename_file("temp.dat", "final.dat");
void uii_rename_file(char *oldname, char *newname);

// uii_copy_file — Copy a file to a new name or path.
// Input:  source — source file path.
// Input:  destination — destination file path.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_copy_file("song.hbt", "backup/song.hbt");
void uii_copy_file(char *source, char *destination);

// uii_get_ramdisk_info — Get information about GEOS RAM disks stored in REU.
// Output: 8 bytes in uii_data[] — 2 bytes per drive (IDs 8-11): drive ID and type.
// Syntax: uii_get_ramdisk_info();
void uii_get_ramdisk_info(void);

// uii_loadIntoRamDisk — Load a file into a GEOS RAM disk in REU.
// Input:  id — RAM disk ID (drive number, e.g. 8).
// Input:  filename — name of the file to load.
// Input:  whatif — 0 = perform the load; 1 = trial run only (checks size/type, no actual load).
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_loadIntoRamDisk(8, "disk.g64", 0);
void uii_loadIntoRamDisk(char id, char *filename, char whatif);

// uii_saveRamDisk — Save a GEOS RAM disk from REU to a file.
// Input:  id — RAM disk ID.
// Input:  filename — destination filename.
// Output: none; status "00,OK" or a filesystem error.
// Syntax: uii_saveRamDisk(8, "disk.g64");
void uii_saveRamDisk(char id, char *filename);

// uii_save_reu — Save a region of REU memory to the currently open file (file must be open for writing).
// Input:  reu_addr — starting address in REU memory (assumes a 16 MB REU configuration).
// Input:  size — number of bytes to save; truncated if reu_addr + size exceeds the end of REU.
// Output: none directly; transfer summary string in uii_data[] (e.g. "$008000 BYTES SAVED FROM REU $852000"). Status "00,OK", "02,REQUEST TRUNCATED", or a filesystem error.
// Syntax: uii_save_reu(0x852000UL, 0x8000UL);
void uii_save_reu(unsigned long reu_addr, unsigned long size);

// uii_load_reu — Load a region from the currently open file into REU memory.
// Input:  reu_addr — destination address in REU memory (assumes a 16 MB REU configuration).
// Input:  size — number of bytes to load; truncated if reu_addr + size exceeds the end of REU.
// Output: none directly; transfer summary string in uii_data[] (e.g. "$003000 BYTES LOADED TO REU $126800"). Status "00,OK", "02,REQUEST TRUNCATED", or a filesystem error.
// Syntax: uii_load_reu(0x126800UL, 0x3000UL);
void uii_load_reu(unsigned long reu_addr, unsigned long size);

// uii_save_reu_image — Save a whole REU image (from address 0) to the currently open file.
// Input:  size — REU size index: 0=128KB, 1=256KB, 2=512KB, 3=1MB, 4=2MB, 5=4MB, 6=8MB, 7=16MB.
// Output: none directly; transfer summary string in uii_data[]. Status "00,OK", "02,REQUEST TRUNCATED", or a filesystem error.
// Syntax: uii_save_reu_image(7); // save full 16 MB REU
void uii_save_reu_image(char size);

// uii_load_reu_image — Load a whole REU image from the currently open file into REU memory starting at address 0.
// Input:  size — REU size index: 0=128KB, 1=256KB, 2=512KB, 3=1MB, 4=2MB, 5=4MB, 6=8MB, 7=16MB.
// Output: none directly; transfer summary string in uii_data[]. Status "00,OK", "02,REQUEST TRUNCATED", or a filesystem error.
// Syntax: uii_load_reu_image(7); // load full 16 MB REU
void uii_load_reu_image(char size);

// uii_get_deviceinfo — Retrieve raw drive information from the Ultimate control interface.
// Output: raw byte sequence describing connected drives in uii_data[]; parse with uii_parse_deviceinfo().
// Syntax: uii_get_deviceinfo();
void uii_get_deviceinfo(void);

// uii_parse_deviceinfo — Call uii_get_deviceinfo() and parse the result into the uii_devinfo[] array.
// Output: 1 on success (uii_devinfo[] populated), 0 on failure (UCI error or no devices found).
// Syntax: if (uii_parse_deviceinfo()) { /* uii_devinfo[0..3] now valid */ }
char uii_parse_deviceinfo(void);

// uii_device_type — Convert a drive type byte to a human-readable string.
// Input:  typeval — type value from uii_devinfo[n].type.
// Output: pointer to a string literal: "1541", "1571", "1581", "SoftIEC", "Printer", or "" if unknown.
// Syntax: char *name = uii_device_type(uii_devinfo[0].type);
char *uii_device_type(char typeval);

// uii_reboot — Trigger a clean C64 reset through the Ultimate control interface.
// Output: none; the machine resets immediately and this call does not return.
// Syntax: uii_reboot();
void uii_reboot(void);

// uii_get_hwinfo — Retrieve hardware information from the Ultimate cartridge.
// Input:  device — 0 = product identification string (e.g. "Ultimate 64"); 1 = SID chip configuration (uii_data[0]=count, then per SID: addr_lo, addr_hi, bits, rsvd, rsvd).
// Output: none directly; result in uii_data[].
// Syntax: uii_get_hwinfo(0);
void uii_get_hwinfo(char device);

// uii_enable_drive_a — Power on the Ultimate emulated drive A.
// Output: none; status "00,OK". Allow the drive a moment to spin up before mounting or accessing it.
// Syntax: uii_enable_drive_a();
void uii_enable_drive_a(void);

// uii_disable_drive_a — Power off the Ultimate emulated drive A.
// Output: none; status "00,OK".
// Syntax: uii_disable_drive_a();
void uii_disable_drive_a(void);

// uii_enable_drive_b — Power on the Ultimate emulated drive B.
// Output: none; status "00,OK". Allow the drive a moment to spin up before mounting or accessing it.
// Syntax: uii_enable_drive_b();
void uii_enable_drive_b(void);

// uii_disable_drive_b — Power off the Ultimate emulated drive B.
// Output: none; status "00,OK".
// Syntax: uii_disable_drive_b();
void uii_disable_drive_b(void);

// uii_get_drive_a_power — Read the current power state of drive A.
// Output: none directly; uii_data[0] = 0 (off) or 1 (on).
// Syntax: uii_get_drive_a_power();
void uii_get_drive_a_power(void);

// uii_get_drive_b_power — Read the current power state of drive B.
// Output: none directly; uii_data[0] = 0 (off) or 1 (on).
// Syntax: uii_get_drive_b_power();
void uii_get_drive_b_power(void);

// Media drive enumeration
// UII_MAX_DRIVES:     maximum number of SD/USB drives to track (override with -dUII_MAX_DRIVES=N)
// UII_DRIVE_PATH_LEN: bytes per stored drive root path, e.g. "/usb0/" = 7 bytes; 16 gives headroom
#ifndef UII_MAX_DRIVES
#define UII_MAX_DRIVES      5
#endif
#ifndef UII_DRIVE_PATH_LEN
#define UII_DRIVE_PATH_LEN  16
#endif

// uii_scan_media — Scan the UCI root "/" for user-accessible storage (directories named sd* or usb*, case-insensitive).
// Fills drives[0..n-1] with lowercase slash-delimited paths, e.g. "/usb0/", "/sd/".
// Sets *count to the number found (0..UII_MAX_DRIVES). Leaves CWD at "/".
// Input:  drives — 2-D array to receive found paths; dimensioned [UII_MAX_DRIVES][UII_DRIVE_PATH_LEN].
// Input:  count — pointer to receive the number of drives found.
// Output: 1 if root was opened successfully, 0 on error.
// Syntax: char drives[UII_MAX_DRIVES][UII_DRIVE_PATH_LEN]; char n; uii_scan_media(drives, &n);
char uii_scan_media(char drives[UII_MAX_DRIVES][UII_DRIVE_PATH_LEN], char *count);

// uii_find_media_path — Search for subpath under each drive in drives[0..drv_count-1] (as populated by uii_scan_media()).
// On the first match: copies the full found path into result[], leaves CWD there
// (ready for immediate file operations), and returns 1.
// On failure: sets result[0]=0 and returns 0.
// Input:  drives — drive array populated by uii_scan_media().
// Input:  drv_count — number of valid entries in drives.
// Input:  subpath — relative subpath to search for, e.g. "mygame/data/".
// Input:  result — buffer to receive the full found path; must hold at least UII_DRIVE_PATH_LEN + strlen(subpath) + 1 bytes.
// Output: 1 if found (CWD is now at the found path), 0 if not found on any drive.
// Syntax: char result[32]; if (uii_find_media_path(drives, n, "mygame/data/", result)) { ... }
char uii_find_media_path(char drives[UII_MAX_DRIVES][UII_DRIVE_PATH_LEN], char drv_count,
                          char *subpath, char *result);

#pragma compile("ultimate_dos_lib.c")

#endif
