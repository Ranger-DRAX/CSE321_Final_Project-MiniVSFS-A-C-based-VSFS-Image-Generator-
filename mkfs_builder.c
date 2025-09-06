// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

#define BS 4096u // define the  block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

// ================= Structures =================
#pragma pack(push,1)
typedef struct {
    uint32_t magic;               // 0x4D565346
    uint32_t version;             // 1
    uint32_t block_size;          // 4096
    uint64_t total_blocks;        // computed
    uint64_t inode_count;         // CLI

    uint64_t inode_bitmap_start;  // 1
    uint64_t inode_bitmap_blocks; // 1
    uint64_t data_bitmap_start;   // 2
    uint64_t data_bitmap_blocks;  // 1

    uint64_t inode_table_start;   // 3
    uint64_t inode_table_blocks;  // computed
    uint64_t data_region_start;   // computed
    uint64_t data_region_blocks;  // computed

    uint64_t root_inode;          // 1
    uint64_t mtime_epoch;         // time(NULL)

    uint32_t flags;               // 0
    uint32_t checksum;            // crc (must be last)
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;

    uint32_t direct[DIRECT_MAX];

    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;

    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t  type;
    char     name[58];
    uint8_t  checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

// ================= CRC Helpers =================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

// ================= Bitmap helpers =================
static inline void bm_set(uint8_t *bm, uint32_t idx){
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}

// ================= Dirent helper =================
static void make_dirent(dirent64_t *de, uint32_t ino, uint8_t type, const char *name){
    memset(de, 0, sizeof(*de));
    de->inode_no = ino;
    de->type = type;
    strncpy(de->name, name, 58);
    dirent_checksum_finalize(de);
}

// ================= Main =================
int main(int argc, char *argv[]) {
    crc32_init();
    if (argc != 7){
        fprintf(stderr,"Usage: %s --image out.img --size-kib N --inodes M\n", argv[0]);
        return 1;
    }

    char *image_name=NULL;
    uint64_t size_kib=0, inode_count=0;

    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--image")) image_name=argv[++i];
        else if (!strcmp(argv[i],"--size-kib")) size_kib=strtoull(argv[++i],NULL,10);
        else if (!strcmp(argv[i],"--inodes")) inode_count=strtoull(argv[++i],NULL,10);
    }
    if (!image_name || size_kib<180 || size_kib>4096 || (size_kib%4)!=0 || inode_count<128 || inode_count>512){
        fprintf(stderr,"Invalid parameters\n");
        return 1;
    }

    uint64_t total_blocks = (size_kib*1024)/BS;
    uint64_t inode_table_blocks = (inode_count*INODE_SIZE + BS-1)/BS;
    uint64_t inode_bitmap_start = 1;
    uint64_t data_bitmap_start  = 2;
    uint64_t inode_table_start  = 3;
    uint64_t data_region_start  = inode_table_start + inode_table_blocks;

    // prepare superblock
    uint8_t sb_block[BS]; memset(sb_block,0,BS);
    superblock_t *sb = (superblock_t*)sb_block;
    sb->magic=0x4D565346;
    sb->version=1;
    sb->block_size=BS;
    sb->total_blocks=total_blocks;
    sb->inode_count=inode_count;
    sb->inode_bitmap_start=inode_bitmap_start;
    sb->inode_bitmap_blocks=1;
    sb->data_bitmap_start=data_bitmap_start;
    sb->data_bitmap_blocks=1;
    sb->inode_table_start=inode_table_start;
    sb->inode_table_blocks=inode_table_blocks;
    sb->data_region_start=data_region_start;
    sb->data_region_blocks=total_blocks-data_region_start;
    sb->root_inode=1;
    sb->mtime_epoch=time(NULL);
    sb->flags=0;
    superblock_crc_finalize(sb);

    // inode bitmap
    uint8_t ibm[BS]; memset(ibm,0,BS);
    bm_set(ibm,0); // root inode allocated

    // data bitmap
    uint8_t dbm[BS]; memset(dbm,0,BS);
    bm_set(dbm,0); // first data block for root dir

    // inode table
    uint8_t *inode_table = calloc(inode_table_blocks,BS);
    inode_t *root = (inode_t*)inode_table;
    memset(root,0,sizeof(*root));
    root->mode=0040000; // dir
    root->links=2;
    root->size_bytes=64*2; // "." and ".."
    root->atime=root->mtime=root->ctime=sb->mtime_epoch;
    root->direct[0]=data_region_start; // absolute block #
    inode_crc_finalize(root);

    // root directory data
    uint8_t *rootblk = calloc(1,BS);
    dirent64_t *d1 = (dirent64_t*)rootblk;
    make_dirent(d1,1,2,".");
    dirent64_t *d2 = (dirent64_t*)(rootblk+64);
    make_dirent(d2,1,2,"..");

    // write everything
    FILE *f=fopen(image_name,"wb");
    if(!f){perror("fopen"); return 1;}
    fwrite(sb_block,BS,1,f);
    fwrite(ibm,BS,1,f);
    fwrite(dbm,BS,1,f);
    fwrite(inode_table,BS,inode_table_blocks,f);
    fwrite(rootblk,BS,1,f);

    // pad the rest
    uint64_t written = 1+1+1+inode_table_blocks+1;
    uint64_t remain = total_blocks - written;
    uint8_t *zero = calloc(1,BS);
    for(uint64_t i=0;i<remain;i++) fwrite(zero,BS,1,f);
    fclose(f);

    printf("Created FS image '%s' with %llu blocks, %llu inodes\n",
           image_name,(unsigned long long)total_blocks,(unsigned long long)inode_count);
    return 0;
}
