# ZicIO: Rapid Data Ingestion through DB-OS Co-design

## How to Reproduce Key Results

### QEMU Setting

We need to modify the kernel to use ZicIO. If you are not using QEMU, you can skip this chapter.

1. Create ubuntu image
```
$ qemu-img create -f qcow2 ubuntu.img 64G

$ wget https://releases.ubuntu.com/20.04/ubuntu-20.04.6-live-server-amd64.iso

$ qemu-system-x86_64 \
-m 4G \
-drive file=ubuntu.img,if=virtio,cache=writethrough \
-cdrom ubuntu-20.04.6-live-server-amd64.iso \
-boot d
```

2. Create NVMe image
```
$ qemu-img create -f qcow2 nvme.img 128G
```

3. Run the QEMU
```
$ sudo qemu-system-x86_64 \
-cpu Cascadelake-Server \
-smp 32 \
-m 128G \
-enable-kvm \
-drive file=ubuntu.img \
-drive file=nvme.img,if=none,id=nvm \
-device nvme,serial=deadbeef,drive=nvm \
-net user,hostfwd=tcp::12345-:22 \
-net nic \
-nographic
```

4. Connect to the QEMU
```
$ ssh -p 12345 username@localhost
```

### Install ZicIO

1. Clone this repository
```
$ git clone https://github.com/anonymoususer90/sigmod25_zicio_artifact.git

$ cd sigmod25_zicio_artifact
```

2. Compile and install ZicIO kernel
```
$ ./build_and_install_zicio_kernel.sh

$ sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 5.15.0-zicio+"

$ sudo reboot
```

3. Compile and install libzicio
```
$ cd sigmod25_zicio_artifact/libzicio

$ make test
```
