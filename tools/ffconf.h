/* ffconf.h -- FatFs R0.15 configuration for Adafruit_ColecoJam */
#define FFCONF_DEF	80286

#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND	0
#define FF_USE_MKFS	0
#define FF_USE_FASTSEEK	1
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	1
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

#define FF_CODE_PAGE	437
/* LFN buffer: 1 = static working buffer in BSS.
   Do NOT use 3 (heap) unless ffsystem.c is in the build -- it is what
   supplies ff_memalloc/ff_memfree, and without it the link fails.
   A static buffer is also the better fit here: no fragmentation, and
   FatFs is only touched from core 0. Costs ~512 bytes of BSS. */
#define FF_USE_LFN	1
#define FF_MAX_LFN	255
#define FF_LFN_UNICODE	0
#define FF_LFN_BUF	255
#define FF_SFN_BUF	12
#define FF_FS_RPATH	0

#define FF_VOLUMES	1
#define FF_STR_VOLUME_ID	0
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS	512
#define FF_MAX_SS	512
#define FF_LBA64	0
#define FF_MIN_GPT	0x10000000
#define FF_USE_TRIM	0

#define FF_FS_TINY	0
#define FF_FS_EXFAT	0
#define FF_FS_NORTC	1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK	0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
