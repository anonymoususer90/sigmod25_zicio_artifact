#!/bin/bash
set -eux -o pipefail

# Enable source
printf "Installing dependencies...\n"
sudo cp /etc/apt/sources.list /etc/apt/sources.list~
sudo sed -Ei 's/^# deb-src /deb-src /' /etc/apt/sources.list
sudo apt-get update

# Install build dependencies
sudo apt-get build-dep linux linux-image-$(uname -r) -y || true
sudo apt-get install -y libncurses-dev flex bison openssl libssl-dev dkms zstd \
    libelf-dev libudev-dev libpci-dev libiberty-dev \
    autoconf fakeroot bc cpio

# Install BTF dependency
wget -O /tmp/dwarves_1.17-1_amd64.deb http://old-releases.ubuntu.com/ubuntu/pool/universe/d/dwarves-dfsg/dwarves_1.17-1_amd64.deb
sudo dpkg -i /tmp/dwarves_1.17-1_amd64.deb


SCRIPT_PATH=`realpath $0`
BASE_DIR=`dirname $SCRIPT_PATH`
LINUX_PATH="$BASE_DIR/linux"

pushd $LINUX_PATH

# Cleanup the previous build
rm -f ../linux-* 2> /dev/null
make distclean

sed -Ei 's/^EXTRAVERSION =$/EXTRAVERSION =-zicio/' Makefile
# Configure kernel
printf "Configuring kernel...\n"
(yes "" || true) | make localmodconfig
./scripts/config -e CONFIG_DEBUG_INFO
./scripts/config -e CONFIG_GDB_SCRIPTS
./scripts/config -e CONFIG_FRAME_POINTER
./scripts/config -e CONFIG_BPF_SYSCALL
./scripts/config -e CONFIG_DEBUG_INFO_BTF
./scripts/config -e CONFIG_UIO
./scripts/config -e CONFIG_UIO_PCI_GENERIC
./scripts/config -e CONFIG_KGDB
./scripts/config -e CONFIG_KGDB_KDB
./scripts/config -e CONFIG_KGDB_SERIAL_CONSOLE
./scripts/config -e CONFIG_EXT4_FS
./scripts/config -e CONFIG_ZICIO
./scripts/config -e CONFIG_ZICIO_MEM
./scripts/config -e CONFIG_BLK_DEV_NVME
./scripts/config -e CONFIG_E1000
./scripts/config -d CONFIG_DEBUG_INFO_REDUCED
./scripts/config -d CONFIG_RANDOMIZE_BASE
./scripts/config -d SYSTEM_TRUSTED_KEYS
./scripts/config -d SYSTEM_REVOCATION_KEYS

make olddefconfig
if [ -z "$(cat .config | grep CONFIG_DEBUG_INFO_BTF)" ]; then
    printf "Cannot find CONFIG_DEBUG_INFO_BTF in .config file. Please enable it manually by 'make nconfig'.\n"
    exit 1
fi
if [ -z "$(cat .config | grep CONFIG_UIO_PCI_GENERIC)" ]; then
    printf "Cannot find CONFIG_UIO_PCI_GENERIC in .config file. Please enable it manually by 'make nconfig'.\n"
    exit 1
fi
if [ -z "$(cat .config | grep CONFIG_ZICIO)" ]; then
    printf "Cannot find CONFIG_ZICIO in .config file. Please enable it manually by 'make nconfig'.\n"
    exit 1
fi


# Compile kernel
#printf "Compiling kernel...\n"
#make deb-pkg -j $(nproc)
#popd

# Install kernel
#printf "Installing kernel...\n"
#pushd $BASE_DIR
#sudo dpkg -i linux-*.deb
#popd

#if [ -z "$(awk -F\' '/menuentry / {print $2}' /boot/grub/grub.cfg | grep -m 1 'Ubuntu, with Linux 5.15.0-zicio+')" ]; then
#    printf "Cannot find ZICIO kernel. Please install the kernel manually.\n"
#    exit 1
#fi

#printf "ZICIO kernel is installed. To boot into SANTFACE kernel, please run:\n"
#printf "    sudo grub-reboot \"Advanced options for Ubuntu>Ubuntu, with Linux 5.15.0-zicio+\"\n"
#printf "    sudo reboot\n"
