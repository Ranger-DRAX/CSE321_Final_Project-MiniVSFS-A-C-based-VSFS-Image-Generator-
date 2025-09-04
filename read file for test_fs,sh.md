### 🔹 How to use test_fs.sh

### for making  it executable:
``` chmod +x test_fs.sh```

### Run after building an image:

```  ./test_fs.sh fs.img```

### Run after adding a file:

``` ./test_fs.sh fs2.img ```

### 🔹 What it shows (in order)

Superblock → magic (MVSF), version, block size, etc.

Inode bitmap → root inode allocated.

Data bitmap → root dir block allocated.

Root inode → directory mode (40 00), links = 2, points to block 4.

Root dir block → entries . and .., later new files (file_9.txt, etc.).

Inode #2 → details for first added file.

File data block → shows text contents of file_9.txt.

