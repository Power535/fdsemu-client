#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "version.h"
#include "Device.h"
#include "Flash.h"
#include "System.h"
#include "DiskImage.h"
#include "build.h"
#include "crc32.h"
#include "diskutil.h"
#include "diskrw.h"
#include "flashrw.h"
#include "fwupdate.h"

#if defined WIN32
char host[] = "Win32";
#elif defined WIN64
char host[] = "Win64";
#elif defined __linux__
char host[] = "Linux";
#elif defined(__APPLE__)
char host[] = "OSX";
#else
char host[] = "Unknown";
#endif

CDevice dev;

enum {

	ACTION_GROUP_FLASH = 100,
	ACTION_GROUP_DISK = 200,
	ACTION_GROUP_FIRMWARE = 300,
	ACTION_GROUP_MISC = 400,

	ACTION_INVALID = -1,
	ACTION_NONE = 0,
	ACTION_HELP,
	ACTION_CONVERT,

	ACTION_LIST = ACTION_GROUP_FLASH,
	ACTION_READFLASH,
	ACTION_READFLASHRAW,
	ACTION_WRITEFLASH,
	ACTION_WRITEDOCTORFLASH,
	ACTION_ERASEFLASH,

	ACTION_READDISK = ACTION_GROUP_DISK,
	ACTION_READDISKRAW,
	ACTION_READDISKBIN,
	ACTION_READDISKDOCTOR,
	ACTION_WRITEDISK,

	ACTION_UPDATEFIRMWARE = ACTION_GROUP_FIRMWARE,
	ACTION_UPDATEFIRMWARE2,
	ACTION_UPDATEBOOTLOADER,
	ACTION_UPDATELOADER,

	ACTION_CHIPERASE = ACTION_GROUP_MISC,
	ACTION_SELFTEST,
	ACTION_VERIFY,
};

typedef struct arg_s {
	int action;
	char cshort;
	char clong[32];
	int numparams;
	int *flagptr;
	char description[256];
} arg_t;

#define ARG_ACTION(ac,sh,lo,np,de)		{ac,sh,lo,np,0,de}
#define ARG_FLAG(va,sh,lo,de)			{-1,sh,lo,0,va,de}
#define ARG_END()						{0,0,"",0,0,""}

int action = ACTION_NONE;
int verbose = 0;
int force = 0;

arg_t args[] = {
	ARG_ACTION(ACTION_HELP,				'h',	"--help",					0,	"Display this message"),
	ARG_ACTION(ACTION_LIST,				'l',	"--list",					0,	"List all disks stored in flash"),
	ARG_ACTION(ACTION_SELFTEST,			0,		"--self-test",				0,	"Perform FDSemu self-test"),
	ARG_ACTION(ACTION_CHIPERASE,		0,		"--chip-erase",				0,	"Erase entire flash chip"),
	ARG_ACTION(ACTION_UPDATEFIRMWARE,	0,		"--update-firmware",		1,	"Update firmware from file (using sram)"),
	ARG_ACTION(ACTION_UPDATEFIRMWARE2,	0,		"--update-firmware-flash",	1,	"Update firmware from file (using flash)"),
	ARG_ACTION(ACTION_UPDATEBOOTLOADER,	0,		"--update-bootloader",		1,	"Update bootloader from file"),

	ARG_ACTION(ACTION_WRITEDISK,		'w',	"--write-disk",				-1,	"Write disk from disk image"),
	ARG_ACTION(ACTION_READDISK,			'r',	"--read-disk",				1,	"Read disk to fwNES format"),
	ARG_ACTION(ACTION_READDISKBIN,		0,		"--read-disk-bin",			1,	"Read disk to bin format"),
	ARG_ACTION(ACTION_READDISKRAW,		0,		"--read-disk-raw",			1,	"Read disk to raw format"),
	ARG_ACTION(ACTION_READDISKDOCTOR,	0,		"--read-disk-doctor",		1,	"Read disk to game doctor format"),

	ARG_ACTION(ACTION_ERASEFLASH,		'e',	"--erase",					-1,	"Erase list of slots"),
	ARG_ACTION(ACTION_WRITEFLASH,		'f',	"--write-flash",			-1,	"Write list of disks to flash"),

	ARG_FLAG(&force,	0,	"--force",		"Force an operation (depreciated)"),
	ARG_FLAG(&verbose,	0,	"--verbose",	"More verbose output"),

	ARG_END()
};

//allocate buffer and read whole file
bool loadfile(char *filename, uint8_t **buf, int *filesize)
{
	FILE *fp;
	int size;
	bool result = false;

	//check if the pointers are ok
	if (buf == 0 || filesize == 0) {
		return(false);
	}

	//open file
	if ((fp = fopen(filename, "rb")) == 0) {
		return(false);
	}

	//get file size
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	//allocate buffer
	*buf = new uint8_t[size];

	//read in file
	*filesize = fread(*buf, 1, size, fp);

	//close file and return
	fclose(fp);
	return(true);
}

#if !defined(WIN32) && !defined(WIN64)
#include <unistd.h>
#else
#include <io.h>
#ifndef F_OK
#define F_OK 00
#define R_OK 04
#endif
#endif

bool file_exists(char *fn)
{
	if (access(fn, F_OK) != -1) {
		return(true);
	}
	return(false);
}

static void usage(char *argv0)
{
	char usagestr[] =
		"  Flash operations:\n"
		"    -f file.fds [1..n]            write image to flash, optional to specify slot\n"
		"    -F file.A                     write doctor images to flash (only specify first disk file)\n"
		"    -s file.fds [1..n]            save image from flash slot [1..n]\n"
		"    -e [1..n] | all               erase slot [1..n] or erase all slots\n"
		"    -l                            list images stored on flash\n"
		"\n"
		"  Disk operations:\n"
		"    -R file.fds                   read disk to fwNES format disk image\n"
		"    -r file.raw [file.bin]        read disk to raw file (and optional bin file)\n"
		"    -w file.fds                   write disk from fwNES format disk image\n"
		"\n"
		"  Other operations:\n"
		"    -U firmware.bin               update firmware from firmware image\n"
		"\n"
		"  Options:\n"
		"    -v                            more verbose output\n"
		"";
/*	char usagestr[] =
		"Commands:\n\n"
		" Flash operations:\n\n"
		"  -f, --write-flash [files...]     write image(s) to flash\n"
		"  -F, --write-doctor [files...]    write doctor disk image sides to flash\n"
		"  -s, --save-flash [file] [1..n]   save image from flash slot [1..n]\n"
		"  -e, --erase-slot [1..n]          erase flash disk image in slot [1..n]\n"
		"  -d, --dump [file] [addr] [size]  dump flash from starting addr, size bytes\n"
		"  -W, --write-dump [file] [addr]   write raw flash data to flash\n"
		"\n"
		" Disk operations:\n\n"
		"  -r [file]                        read disk to file, type must be specified\n"
		"  -w [file]                        write file to disk\n"
		"\n"
		" Conversion operations:\n\n"
		"  -c, --convert [infile] [outfile] convert disk image, type must be specified\n"
		"\n"
		" Update operations:\n\n"
		"  -u, --update-loader [file]       update loader from fwFDS loader image\n"
		"  -U, --update-firmware [file]     update firmware from binary image\n"
		"\n"
		"Options:\n\n"
		"  -v, --verbose                    more verbose output\n"
		"      --force                      force operation\n"
		"  -t, --type [type]                specify file type, valid types: fds bin raw\n"
		"\n";*/

	printf("\n  usage: %s <options> <command> <file(s)>\n\n%s", "fdsemu-cli", usagestr);
}

uint8_t *find_string(uint8_t *str, uint8_t *buf, int len)
{
	int identlen = strlen((char*)str);
	uint8_t *ptr = buf;
	int i;
	uint8_t *ret = 0;

	for (i = 0; i < (len - identlen); i++, buf++) {

		//first byte is a match, continue checking
		if (*buf == (uint8_t)str[0]) {

			//check for match
			if (memcmp(buf, str, identlen) == 0) {
				ret = buf;
				break;
			}
		}
	}
	return(ret);
}

uint8_t ident_bootloader[] = "*BOOT2*";

bool bootloader_is_valid(uint8_t *fw, int len)
{
	uint8_t *buf;

	if (len > 4096) {
		return(false);
	}
	buf = find_string(ident_bootloader, fw, len);
	return(buf == 0 ? false : true);
}

int GetDiskSide(int ownerid, int childid)
{
	TFlashHeader *headers = dev.FlashUtil->GetHeaders();
	TFlashHeader *header;
	int ret = 1;
	int slot = ownerid;

	while (slot != 0xFFFF) {
		header = &headers[slot];
		ret++;
		slot = header->nextid;
		if (slot == childid) {
			break;
		}
	}

	return(ret);
}

bool fds_list(int verbose)
{
	TFlashHeader *headers = dev.FlashUtil->GetHeaders();
	uint32_t i;
	int side = 0, empty = 0;

	if (headers == 0) {
		return(false);
	}

	printf("Listing disks stored in flash:\n");

	for (i=0; i < dev.Slots; i++) {
		TFlashHeader *header = &headers[i];

		uint8_t *buf = headers[i].filename;

		//verbose listing
		if (verbose) {

			//empty slot
			if (buf[0] == 0xFF) {
				printf("%d:\n", i);
				side = 0;
				empty++;
				continue;
			}

			//this disk image has valid ownerid/nextid
			if (header->flags & 0x20) {
				if (header->ownerid == i) {
					printf("%d: %s\n", i, buf);
				}
				else if ((header->flags & 3) == 3) {
					printf("%d:    Game Doctor save disk for slot %d\n", i, header->ownerid);
				}
				else {
					side = GetDiskSide(header->ownerid,i);
					printf("%d:    Child of slot %d (Side %c)\n", i, header->ownerid,'A' + (side - 1));
				}
				side = 1;
			}

			//old format flash image
			else {
				if (buf[0] != 0) {      //filename present
					printf("%d: %s\n", i, buf);
					side = 1;
				}
				else if (!side) {          //first side is missing
					printf("%d: ?\n", i);
				}
				else {                    //next side
					printf("%d:    Side %d\n", i, ++side);
				}
			}
		}

		//short listing
		else {
			//empty slot
			if (buf[0] == 0xFF) {
				empty++;
				continue;
			}

			//this disk image has valid ownerid/nextid
			if (header->flags & 0x20) {
				if (header->ownerid == i) {
					printf("%d: %s\n", i, buf);
				}
			}

			else {
				if (buf[0] == 0xFF) {          //empty
					empty++;
				}

				//filename is here
				else if (buf[0] != 0) {
					printf("%d: %s\n", i, buf);
				}
			}
		}
	}
	printf("\nEmpty slots: %d\n", empty);
	return(true);
}

bool convert_image(char *infile, char *outfile, char *type)
{
//	CDiskImage image;

//	image.Load(infile);
	return(true);
}

bool chip_erase()
{
	uint8_t cmd[] = { CMD_CHIPERASE,0,0,0 };

	if (!dev.Flash->WriteEnable())
		return false;
	if (!dev.FlashWrite(cmd, 1, 1, 0))
		return false;
	return(dev.Flash->WaitBusy(600 * 1000));
}

bool verify_device()
{
	uint8_t *buf, *buf2;
	int i;

	bool ok = false;

	buf = new uint8_t[0x10000];
	buf2 = new uint8_t[0x10000];

	do {
		printf("Testing flash read/write...");

		//read data first (to preserve data)
		dev.Flash->Read(buf, 0, 0x10000);

		//generate simple test pattern
		for (i = 0; i < 0x10000; i++) {
			buf2[i] = (uint8_t)i;
		}

		//write simple test pattern, then read it back
		dev.Flash->Write(buf2, 0, 0x10000);
		dev.Flash->Read(buf2, 0, 0x10000);

		//verify flash data
		for (i = 0; i < 0x10000; i++) {
			if (buf2[i] != (uint8_t)i) {
				printf("error\n");
				break;
			}
		}

		//write old data back
		dev.Flash->Write(buf, 0, 0x10000);
		printf("ok\n");

		printf("Testing sram read/write...");
		//write test pattern to sram, then read it back
		dev.Sram->Write(buf2, 0, 0x10000);
		dev.Sram->Read(buf2, 0, 0x10000);

		//verify sram data
		for (i = 0; i < 0x10000; i++) {
			if (buf2[i] != (uint8_t)i) {
				printf("error\n");
				break;
			}
		}
		printf("ok\n");

		printf("Asking device to do a self-test...\n");
		ok = dev.Selftest();
		printf("If FDSemu LED indicator is RED, the self-test has failed.\n");

	} while (0);

	delete[] buf;
	delete[] buf2;
	return(ok);
}

bool erase_slot(int slot)
{
	TFlashHeader *headers = dev.FlashUtil->GetHeaders();
	uint8_t *buf;

	if (dev.Flash->Erase(slot * SLOTSIZE, SLOTSIZE) == false) {
		return(false);
	}
	for (uint32_t i = (slot + 1); i<dev.Slots; i++) {
		buf = headers[i].filename;
		if (buf[0] == 0xff) {          //empty
			break;
		}
		else if (buf[0] != 0) {      //filename present
			break;
		}
		else {                    //next side
			if (dev.Flash->Erase(i * SLOTSIZE, SLOTSIZE) == false) {
				return(false);
			}
			printf(", %d", i);
		}
	}
	return(true);
}

extern unsigned char firmware[];
extern unsigned char bootloader[];
extern int firmware_length;
extern int bootloader_length;

int main(int argc, char *argv[])
{
	int i, slot;
	bool success = false;
	char *param = 0;
	char *param2 = 0;
	char *param3 = 0;
	char *params[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
	int numparams = 0;
	int required_build = -1;
	int required_version = -1;
	uint32_t required_crc32 = 0;

	crc32_gentab();

	printf("fdsemu-cli v%d.%d.%d (%s) by James Holodnak, based on code by loopy\n", VERSION / 100, VERSION % 100, BUILDNUM, host);

	required_build = detect_firmware_build((uint8_t*)firmware, firmware_length);
	required_version = detect_bootloader_version((uint8_t*)bootloader, bootloader_length);
	required_crc32 = bootloader_get_crc32((uint8_t*)bootloader, bootloader_length);

	if (argc < 2) {
		usage(argv[0]);
		return(1);
	}

	//parse command line
	for (i = 1; i < argc; i++) {

		//get program usage
		if (strcmp(argv[i], "-?") == 0 || strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return(1);
		}

		//use more verbose output
		else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			verbose = 1;
		}

		//force the operation, whatever it may be
		else if (/*strcmp(argv[i], "-f") == 0 ||*/ strcmp(argv[i], "--force") == 0) {
			force = 1;
		}

		//erase entire chip
		else if (/*strcmp(argv[i], "-f") == 0 ||*/ strcmp(argv[i], "--chip-erase") == 0) {
			action = ACTION_CHIPERASE;
		}

		//list disk images
		else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
			action = ACTION_LIST;
		}

		//read disk image to disk
		else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--read-disk") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename.\n");
				return(1);
			}
			action = ACTION_READDISK;
			param = argv[++i];
			if (argv[i] && argv[i][0] != '-') {
				param2 = argv[++i];
			}
		}

		//read disk image to disk
		else if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--read-disk-fds") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename.\n");
				return(1);
			}
			action = ACTION_READDISK;
			param3 = argv[++i];
		}

		//write disk image to disk
		else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--write-disk") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename.\n");
				return(1);
			}
			action = ACTION_WRITEDISK;
			param = argv[++i];
		}

		//write disk image to flash
		else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--write-flash") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename.\n");
				return(1);
			}
			action = ACTION_WRITEFLASH;
			param = argv[++i];
			if (argv[i] && argv[i][0] != '-') {
				param2 = argv[++i];
			}
		}

		//write doctor disk image to flash
		else if (strcmp(argv[i], "-F") == 0 || strcmp(argv[i], "--write-doctor") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename.\n");
				return(1);
			}
			action = ACTION_WRITEDOCTORFLASH;
			param = argv[++i];
		}

		//read disk image from flash
		else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--save-flash") == 0) {
			if ((i + 2) >= argc) {
				printf("\nPlease specify a filename and slot number.\n");
				return(1);
			}
			action = ACTION_READFLASH;
			param = argv[++i];
			if (argv[i][0] != '-') {
				param2 = argv[++i];
			}
		}

		//read disk image from flash
		else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--save-flash-raw") == 0) {
			if ((i + 2) >= argc) {
				printf("\nPlease specify a filename and slot number.\n");
				return(1);
			}
			action = ACTION_READFLASHRAW;
			param = argv[++i];
			if (argv[i][0] != '-') {
				param2 = argv[++i];
			}
		}

		//erase disk image from flash
		else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--erase-flash") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a slot number (use --list --verbose to see flash contents).\n");
				return(1);
			}
			action = ACTION_ERASEFLASH;
			while ((i + 1) < argc) {
				params[numparams++] = argv[++i];
			}
		}

		//verify that the loader is in place and sram/flash is ok
		else if (strcmp(argv[i], "--verify") == 0) {
			action = ACTION_VERIFY;
		}

		//update loader
		else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--update-loader") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename for the loader to update with.\n");
				return(1);
			}
			action = ACTION_UPDATELOADER;
			param = argv[++i];
		}

		//update firmware
		else if (strcmp(argv[i], "-U") == 0 || strcmp(argv[i], "--update-firmware") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename for the firmware to update with.\n");
				return(1);
			}
			action = ACTION_UPDATEFIRMWARE;
			param = argv[++i];
		}

		//update firmware
		else if (strcmp(argv[i], "--update-firmware-flash") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename for the firmware to update with.\n");
				return(1);
			}
			action = ACTION_UPDATEFIRMWARE2;
			param = argv[++i];
		}

		//update firmware
		else if (strcmp(argv[i], "--update-bootloader") == 0) {
			if ((i + 1) >= argc) {
				printf("\nPlease specify a filename for the bootloader to update with.\n");
				return(1);
			}
			action = ACTION_UPDATEBOOTLOADER;
			param = argv[++i];
		}

		//self test
		else if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--self-test") == 0) {
			action = ACTION_SELFTEST;
		}
	}

	if (dev.Open() == false) {
		printf("Error opening device.\n");
		return(2);
	}
	printf(" Device: %s, %dMB flash (firmware build %d, flashID %06X)\n", dev.DeviceName, dev.FlashSize / 0x100000, dev.Version, dev.FlashID);
	printf("\n");
	
	if (dev.IsV2 == 0) {
		if (dev.Version < required_build && action != ACTION_UPDATEFIRMWARE && action != ACTION_UPDATEFIRMWARE2) {
			char ch;

			printf("Firmware is outdated, the required minimum version is %d\n\n", required_build);
			printf("Press 'y' to upgrade, any other key cancel: \n");
			ch = readKb();
			if (ch == 'Y' || ch == 'y') {
				success = upload_firmware(firmware, firmware_length, 0);
			}
			action = ACTION_INVALID;
		}

		if (dev.VerifyBootloader() != required_crc32 && action != ACTION_UPDATEBOOTLOADER && action != ACTION_UPDATEFIRMWARE && action != ACTION_UPDATEFIRMWARE2) {
			char ch;

			printf("Bootloader is outdated.\n\n");
			printf("Press 'y' to upgrade, any other key cancel: \n");
			ch = readKb();
			if (ch == 'Y' || ch == 'y') {
				success = upload_bootloader(bootloader, bootloader_length);
			}
			action = ACTION_INVALID;
		}
	}

	switch (action) {

	default:
		break;

	case ACTION_NONE:
		printf("No operation specified.\n");
		success = true;
		break;

	case ACTION_LIST:
		success = fds_list(verbose);
		break;

	case ACTION_UPDATEFIRMWARE:
		success = firmware_update(param, 0);
		break;

	case ACTION_UPDATEFIRMWARE2:
		printf("Updating firmware by flash\n");
		success = firmware_update(param, 1);
		break;

	case ACTION_UPDATEBOOTLOADER:
		success = bootloader_update(param);
		break;

	case ACTION_WRITEFLASH:
		if (is_gamedoctor(param)) {
			printf("Detected Game Doctor image.\n");
			success = write_doctor(param);
		}
		else {
			success = write_flash(param, (param2 == 0) ? -1 : atoi(param2));
		}
		break;

	case ACTION_WRITEDOCTORFLASH:
		success = write_doctor(param);
		break;

	case ACTION_READFLASH:
		dev.FlashUtil->ReadHeaders();
		success = read_flash(param, atoi(param2));
		break;

	case ACTION_READFLASHRAW:
		dev.FlashUtil->ReadHeaders();
		success = read_flash_raw(param, atoi(param2));
		break;

	case ACTION_ERASEFLASH:
		dev.FlashUtil->ReadHeaders();
		//erase all slots
		if (params[0] && strcmp(params[0], "all") == 0) {
			printf("Erasing all slots from flash...\n");
			success = dev.Flash->Erase(0, dev.FlashSize);
		}

		//erase one slot
		else {
			printf("Erase disk image from flash...\n");
			for (i = 0; i < numparams; i++) {
				slot = atoi(params[i]);
				printf("   Slot %d", slot);
				success = erase_slot(slot);
				printf("\n");
//				success = dev.Flash->Erase(slot * SLOTSIZE, SLOTSIZE);
				if (success == false) {
					break;
				}
			}
		}
		break;

	case ACTION_WRITEDISK:
		printf("Writing disk from file...\n");
		success = writeDisk(param);
		break;

	case ACTION_READDISK:
		printf("Reading disk to file...\n");
		success = FDS_readDisk(param, param2, param3);
		break;

	case ACTION_CHIPERASE:
		printf("Erasing entire flash chip...\n");
		success = chip_erase();
		break;

	case ACTION_SELFTEST:
		printf("Self test...\n");
		success = dev.Selftest();
		break;

	case ACTION_VERIFY:
		printf("Verify integrity of device...\n");
		success = verify_device();
		break;
	}

	dev.Close();

	if (success) {
		printf("Operation completed successfully.\n");
	}
	else {
		printf("Operation failed.\n");
	}

	return(0);
}
