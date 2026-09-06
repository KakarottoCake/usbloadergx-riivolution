/* Read-only FAT16/FAT32 for the IOS side. See riivo_fat.c for the
   constraints this is shaped by. */
#ifndef RIIVO_FAT_H_
#define RIIVO_FAT_H_

#define RFAT_SECTOR    512
#define RFAT_MAX_NAME  255

#define RFAT_OK        0
#define RFAT_EINVAL   -1
#define RFAT_EIO      -2
#define RFAT_ENOFS    -3
#define RFAT_ENOENT   -4
#define RFAT_EISDIR   -5
#define RFAT_ECORRUPT -6

/* Reads `count` sectors at `lba` into `buf`. Non-zero on success. On the
   console this calls d2x's own block read; in tests it reads an image. */
typedef int (*rfat_read_fn)(void *ctx, unsigned int lba, unsigned int count,
							void *buf);

typedef struct
{
	rfat_read_fn read;
	void *ctx;
	unsigned int part_lba;
	unsigned short bytes_per_sec;
	unsigned char sec_per_clus;
	unsigned char num_fats;
	unsigned short reserved;
	unsigned short root_entries;
	unsigned int fat_sectors;
	unsigned int first_data;
	unsigned int count_clusters;
	unsigned int root_cluster;  /* FAT32 */
	unsigned int root_lba;      /* FAT16 fixed root */
	int is_fat32;
} rfat_vol;

typedef struct
{
	unsigned int cluster;
	unsigned int size;
	int is_dir;
} rfat_dirent;

typedef struct
{
	rfat_vol *vol;
	unsigned int first_cluster;
	unsigned int size;
	unsigned int cur_cluster;
	unsigned int cur_index;
} rfat_file;

/* Find a FAT partition by walking the MBR at LBA 0. d2x keeps only raw
   physical LBAs for the game's fragments - it never records which partition
   they came from - so the partition has to be found here rather than asked
   for. `index` selects among the FAT partitions found, 0 being the first.
   A volume with no MBR at all (superfloppy) reports LBA 0. */
int rfat_find_partition(rfat_read_fn read, void *ctx, int index,
                        unsigned int *lba_out);

int rfat_mount(rfat_vol *v, rfat_read_fn read, void *ctx, unsigned int part_lba);
int rfat_open(rfat_vol *v, const char *path, rfat_file *f);
int rfat_read(rfat_file *f, unsigned int offset, void *buf, unsigned int len,
			  unsigned int *got);
void rfat_drop_cache(void);

#endif
