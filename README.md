# ZicIO: Rapid Data Ingestion through DB-OS Co-design

## Instructions to Build ZicIO Kernel

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

1. Clone this repository.
```
$ git clone https://github.com/anonymoususer90/sigmod25_zicio_artifact.git

$ cd sigmod25_zicio_artifact
```

2. Compile and install ZicIO kernel.
```
$ ./build_and_install_zicio_kernel.sh

$ sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 5.15.0-zicio+"

$ sudo reboot
```

3. Compile and install libzicio.
```
$ cd sigmod25_zicio_artifact/libzicio

$ make test
```
## Usage

1. Include libzicio header file.
```
#include <libzicio.h>
```

2. Initialize and open the ZicIO channel.
```
struct zicio zicio_data;

zicio_init(&zicio_data);

/*
 * Set the size of the data to be obtained by zicio_get_page().
 * In this example, we use 4KB.
 */ 
zicio_data.read_page_size = 4096;

for (int i = 0; i < NUM_FILES; i++) {
    fd[i] = open(path[i], O_RDONLY | O_DIRECT);
    zicio_notify_ranges(&zicio_data, fd[i], start_offset, end_offset);
}

/*
 * After completing the above steps, we can call the zicio_open().
 * This system-call triggers the first I/O of the ZicIO channel.
 */
zicio_open(&zicio_data);
```

3. Use it for data ingestion.
```
do_data_ingestion(struct zicio *zicio_data, ...)
{
    do {
        while (zicio_data->get_status != ZICIO_GET_PAGE_SUCCESS)
            zicio_get_page(zicio_data);

        do_something(zicio_data->page_addr);

        zicio_put_page(zicio_data);
    } while (zicio_data->put_status != ZICIO_PUT_PAGE_EOF);
}
```

4. Close the ZicIO channel.
```
/*
 * Release the resources allocated for ZicIO.
 * Note that this function does not close files.
 */
zicio_close(&zicio_data);

for (int i = 0; i < NUM_FILES; i++) {
    close(fd[i]);
}
```
