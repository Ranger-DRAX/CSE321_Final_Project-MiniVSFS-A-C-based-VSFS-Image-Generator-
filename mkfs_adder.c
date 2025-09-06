// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

// ================= Structures =================
#pragma pack(push,1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;

    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;

    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;

    uint64_t root_inode;
    uint64_t mtime_epoch;

    uint32_t flags;
    uint32_t checksum; // must be last
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
static inline int bm_test(uint8_t *bm, uint32_t idx){
    return (bm[idx >> 3] >> (idx & 7u)) & 1u;
}
static uint32_t bm_find_first_zero(uint8_t *bm, uint32_t limit){
    for(uint32_t i=0;i<limit;i++) if(!bm_test(bm,i)) return i;
    return UINT32_MAX;
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
        fprintf(stderr,"Usage: %s --input in.img --output out.img --file filename\n", argv[0]);
        return 1;
    }

    char *input=NULL,*output=NULL,*filename=NULL;
    for(int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--input")) input=argv[++i];
        else if (!strcmp(argv[i],"--output")) output=argv[++i];
        else if (!strcmp(argv[i],"--file")) filename=argv[++i];
    }
    if (!input || !output || !filename){
        fprintf(stderr,"Invalid parameters\n");
        return 1;
    }

    // open FS image
    FILE *fin=fopen(input,"rb");
    if(!fin){perror("open input"); return 1;}

    fseek(fin,0,SEEK_END);
    size_t fsize=ftell(fin);
    rewind(fin);
    uint8_t *img=malloc(fsize);
    fread(img,1,fsize,fin);
    fclose(fin);

    superblock_t *sb=(superblock_t*)(img);
    uint8_t *ibm = img + sb->inode_bitmap_start*BS;
    uint8_t *dbm = img + sb->data_bitmap_start*BS;
    inode_t *itable = (inode_t*)(img + sb->inode_table_start*BS);
    uint8_t *data_region = img + sb->data_region_start*BS;

    // load host file
    FILE *fh=fopen(filename,"rb");
    if(!fh){perror("open file"); return 1;}
    fseek(fh,0,SEEK_END);
    size_t fsz=ftell(fh);
    rewind(fh);
    uint8_t *buf=malloc(fsz);
    fread(buf,1,fsz,fh);
    fclose(fh);

    // check file size
    size_t max_size = DIRECT_MAX*BS;
    if (fsz > max_size){
        fprintf(stderr,"File too big for MiniVSFS (max %zu bytes)\n", max_size);
        free(buf); free(img);
        return 1;
    }

    // allocate inode
    uint32_t ino_idx = bm_find_first_zero(ibm,sb->inode_count);
    if(ino_idx==UINT32_MAX){fprintf(stderr,"No free inode\n"); return 1;}
    bm_set(ibm,ino_idx);
    inode_t *ino = itable + ino_idx;

    // allocate blocks
    uint32_t need_blocks = (fsz+BS-1)/BS;
    uint32_t blocks[DIRECT_MAX]; memset(blocks,0,sizeof(blocks));
    for(uint32_t i=0;i<need_blocks;i++){
        uint32_t b = bm_find_first_zero(dbm,sb->data_region_blocks);
        if(b==UINT32_MAX){fprintf(stderr,"No free data block\n"); return 1;}
        bm_set(dbm,b);
        blocks[i]=sb->data_region_start+b;
        memcpy(img + blocks[i]*BS, buf+i*BS, (i==need_blocks-1)?(fsz - i*BS):BS);
    }
    free(buf);

    // fill inode
    memset(ino,0,sizeof(*ino));
    ino->mode=0100000; // file
    ino->links=1;
    ino->size_bytes=fsz;
    ino->atime=ino->mtime=ino->ctime=time(NULL);
    for(uint32_t i=0;i<need_blocks;i++) ino->direct[i]=blocks[i];
    inode_crc_finalize(ino);

    // update root dir
    inode_t *root = itable; // inode #1 at index 0
    uint32_t rootblkno = root->direct[0];
    dirent64_t *ents = (dirent64_t*)(img + rootblkno*BS);
    int placed=0;
    for(int i=0;i<BS/64;i++){
        if(ents[i].inode_no==0){
            make_dirent(&ents[i], ino_idx+1, 1, filename);
            placed=1; break;
        }
    }
    if(!placed){
        fprintf(stderr,"Root dir full\n"); return 1;
    }
    root->size_bytes += 64;
    inode_crc_finalize(root);

    // update superblock mtime
    sb->mtime_epoch=time(NULL);
    superblock_crc_finalize(sb);

    // write out
    FILE *fout=fopen(output,"wb");
    if(!fout){perror("open output"); return 1;}
    fwrite(img,1,fsize,fout);
    fclose(fout);
    free(img);

    printf("Added '%s' (size %zu bytes) as inode %u -> %s\n",
           filename,fsz,ino_idx+1,output);
    return 0;
}
