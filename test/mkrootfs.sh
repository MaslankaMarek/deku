#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

echo "Download rootfs image"
curl https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/ubuntu-base-22.04-base-amd64.tar.gz -o ubuntu-base.tar.gz

echo "Make image"
dd if=/dev/zero of=rootfs.img bs=512 count=1M
mkfs.ext4 -F -L dekutestroot rootfs.img
mkdir rootfs

echo "Mount rootfs"
sudo mount -o loop rootfs.img rootfs

echo "Prepare rootfs"
sudo tar zxvf ubuntu-base.tar.gz -C rootfs

sudo cp -rf rootfs-overlay/* rootfs/

sudo mount -t proc /proc rootfs/proc
sudo mount -t sysfs /sys rootfs/sys
sudo mount -o bind /dev rootfs/dev
sudo mount -o bind /dev/pts rootfs/dev/pts

echo "Enter to chroot"
sudo chroot rootfs /bin/bash /root/install.sh

echo "Exit from chroot"
cp rootfs/root/.ssh/testing_rsa .
cat /home/$SUDO_USER/.ssh/id_rsa.pub >> rootfs/root/.ssh/authorized_keys

echo "Unmount"
sudo umount rootfs/proc
sudo umount rootfs/sys
sudo umount rootfs/dev/pts
sudo umount rootfs/dev

sudo umount rootfs

e2fsck -p -f rootfs.img
resize2fs -M rootfs.img

echo "Fix ownership"
chown $SUDO_USER testing_rsa
chown $SUDO_USER rootfs.img

echo "Cleanup"
rm -rf rootfs
rm ubuntu-base.tar.gz
