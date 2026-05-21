# SimpleFS implementation

## 1. Build module

  ```bash
  make KDIR=/path/to/kernel/source/dir
  ```

## 2. Load module

  ```bash
  sudo insmod simplefs.ko
  ```

## 3. Crate virtual disk and Format the disk

```bash
dd if=/dev/zero of=simplefs_disk.img bs=512 count=20000
printf "\xEF\xBE\xAD\xDE\x00\x02\x00\x00\x64\x00\x00\x00\x64\x00\x00\x00\xB4\xD0\xDB\x72" | dd of=simplefs_disk.img bs=1 seek=0 conv=notrunc
printf "\xEF\xBE\xAD\xDE\x00\x02\x00\x00\x64\x00\x00\x00\x64\x00\x00\x00\xB4\xD0\xDB\x72" | dd of=simplefs_disk.img bs=512 seek=100 conv=notrunc
```

## 4. Mount filesystem

```bash
sudo mount -t simplefs /dev/loop0 /mnt/simplefs
```

## 5. *Build tests

```bash
gcc test_simplefs.c -o test_simplefs
```

## 6. *Run tests

```bash
sudo ./test_simplefs /mnt/simplefs stress
```

## *Qemu setup

qemu-system-x86_64 \
    -kernel /home/artemiy/fstests/linux-6.12.90/arch/x86/boot/bzImage \
    -initrd ./my_initrd.cpio.gz \
    -drive file=./simplefs_disk.img,format=raw,index=0,media=disk \
    -virtfs local,path=/home/artemiy/fstests/kernel-modules-project,mount_tag=hostshare,security_model=none,id=hostshare \
    -append "console=ttyS0 quiet" \
    -nographic
