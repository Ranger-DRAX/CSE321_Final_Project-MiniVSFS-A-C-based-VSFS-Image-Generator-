#!/bin/bash
# MiniVSFS Testing Script
# Usage: ./test_fs.sh fs.img

IMG=$1

if [ -z "$IMG" ]; then
  echo "Usage: $0 <imagefile>"
  exit 1
fi

echo "==== SUPERBLOCK (first 128 bytes) ===="
hexdump -C -n 128 "$IMG" | head -20
echo

echo "==== INODE BITMAP (block 1) ===="
hexdump -C -s $((1*4096)) -n 32 "$IMG"
echo

echo "==== DATA BITMAP (block 2) ===="
hexdump -C -s $((2*4096)) -n 32 "$IMG"
echo

echo "==== ROOT INODE (#1, first inode table entry) ===="
hexdump -C -s $((3*4096)) -n 128 "$IMG"
echo

echo "==== ROOT DIRECTORY BLOCK (first data block) ===="
# assumes data region starts at block 4 for small FS
hexdump -C -s $((4*4096)) -n 256 "$IMG"
echo

echo "==== INODE #2 (if a file has been added) ===="
hexdump -C -s $((3*4096 + 128)) -n 128 "$IMG"
echo

echo "==== DATA BLOCK OF INODE #2 (if direct[0]=5) ===="
hexdump -C -s $((5*4096)) -n 128 "$IMG"
echo
