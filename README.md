# Rapid Data Ingestion through DB-OS Co-design

Efficient data retrieval is critical yet resource-intensive in data warehouse systems, where full or range scans consume significant CPU cycles within DBMSs and OSs. *zicIO* offloads data access control from DBMSs to a specially co-designed OS component, reducing redundant data fetching and alleviating all known data access latencies. Key components include precise timing information from DBMSs, automated I/O control by the OS, and seamless coordination between the DBMS and OS through shared memory. In this paper, we present the following core concepts in *zicIO*:

**Core Concepts Underpinning *zicIO***

- **DB-OS Co-design**: We propose *zicIO*, the first DB-OS co-design that frees DBMSs from data access control, allowing them to focus on data ingestion while mitigating all known data access latencies.
- **Automated I/O Control**: Our co-design enables automated I/O control that prepares data just before DBMS workers access it, thereby minimizing CPU usage.
- **Page Sharing Mechanism**: Our co-design for page sharing allows DBMS workers to ingest data from the same table with reduced I/O redundancy.

This document serves as a comprehensive guide to understanding *zicIO*'s underlying logic and its implementation at the code level. It also provides detailed instructions on how to reproduce evaluations using the prepared scripts.

# Automated I/O Control

In the operation of *zicIO*, I/O occurs at three points: when the user calls the **zicio_open** system call, within the NVMe interrupt handler, and in the softirq process. 

### **Automated I/O control in the NVMe interrupt handler**

Most NVMe commands are submitted by the interrupt handler. This pseudocode provides a breakdown of this process, corresponding to the operations of *KZicIO* as depicted in Figure 3 of the paper.

```
/*
 * linux/drivers/host/pci.c
 * Assume that we use zicio_notify_ranges() as a default.
 */
nvme_irq()
|
-- nvme_process_cq()
    |
    -- nvme_handle_cqe()
        |
        -- zicio_notify_complete_command()
        |   |
        |   -- zicio_notify_update_flow_ctrl() /* adjust reward request */
        |   |
        |   -- zicio_complete_firehose_command() /* update status bits in the MEMSB when the I/O is completed */
        |   |
        |   -- zicio_prepare_next_huge_page_cmds() /* prepare next NVMe commands, put them into the timer wheel */
        |   |
        |   -- zicio_notify_do_softtimer_irq_cycle()
        |       |
        |       -- zicio_prepare_resubmit() /* get the next NVMe command from the timer wheel */
        |
        -- zicio_nvme_setup_read_cmd() /* set DMA address of the new NVMe command */
        |
        -- nvme_submit_cmd() /* put the new NVMe command into the submission queue */
```

### **Initializing resources and triggering the first I/O**

Typically, I/O is triggered by the interrupt handler. However, the initial I/O must be triggered manually. The pseudocode below illustrates the process where a user initiates the first I/O operation using **zicio_open**:

```
/* linux/zicio/zicio.c */
SYSCALL_DEFINE1(zicio_u_open, struct zicio_args __user*, zicio_user_args)
|
-- sys_zicio_k_open()
|   |
|   -- zicio_initialize_resources()
|       |
|       -- zicio_allocate_and_initialize_mem() /* alocate data buffer and MEMSB */
|       |
|       -- zicio_mmap_buffers() /* map them to the user's page table */
|
-- zicio_init_read_trigger() /* trigger the first I/O */
```

### **Retriggering by the softIRQ process**

If there is no space in the user’s buffer for additional I/O, resubmission stops and no further NVMe interrupts occur. In this case, *zicIO* calculates when buffer space will become available based on the user’s consumption rate and wakes up the softIRQ process. Then the process triggers the I/O, causing a new NVMe interrupt to occur.

```
/* linux/zicio/zicio_data_buffer_descriptor.c */
zicio_notify_complete_command()
|
-- ...
|
-- next_huge_page_status = zicio_prepare_next_huge_page_cmds() 
|
-- if next_huge_page_status == ZICIO_NO_SPACE_FOR_NEW_IO
    |
    -- io_timing = zicio_get_data_buffer_io_timing()
    |
    -- wake up softIRQ daemon at the @io_timing

/* linux/kernel/smpboot.c */
smpboot_thread_fn() /* softIRQ daemon process' main function */
|
-- while (true)
    |
    -- __do_softirq() /* original softIRQ operation */
    |
    -- zicio_do_softtimer_jobs() /* I/O retriggering of zicIO */
```

# OS-level Page Sharing

The OS-level page sharing mechanism in *zicIO* is designed to optimize data access by allowing multiple DBMS workers to share OS pages without redundancy. 

### **Page Sharing through Pre-mapping**

Pages from a shared pool are pre-mapped into the user's page table. This involves checking if the page exists in the shared pool, accessing it through a hash table, and ensuring it is not expired. The reference counter for the page is incremented to indicate its use. The code flow for this process is provided below.
```
/* linux/zicio/zicio_shared_pool.c */
zicio_adjust_mapping_and_reclaim_pages()
|
-- zicio_premap_pages_from_shared_pool()
    |
    -- for each huge pages in the sliding-window cache
        |
        -- if (zicio_check_bitmaps_to_premap() == true) /* does this page exist in the shared pool? */
            |
            -- zicio_check_spcb_to_premap() /* "spcb" means shared-page-control-block */
            |   |
            |   -- zicio_rcu_hash_find() /* access the spcb of this page through the hash table */
            |   |
            |   -- atomic_inc(&spcb->ref_count) /* increase reference counter */
            |   |
            |   -- if (spcb->expiration_jiffies < current_jiffies) /* check expiration timeout of this page */
            |   |   |
            |   |   -- atomic_dec(&spcb->ref_count)
            |   |   |
            |   |   -- return NULL
            |   |
            |   -- return spcb
            |
            -- if (spcb != NULL)
                |
                -- zicio_do_premap_page_from_shared_pool(spcb) /* pre-map the huge page into the user's page table */
```

 ### **Unmapping from the User's Page Table**

Pages that have been consumed or whose expiration timeout has passed are unmapped from the user's page table. This process involves clearing the page table entry and flushing the TLB (Translation Lookaside Buffer) to ensure consistency. The reference counter is decremented to indicate the page is no longer in use. The code flow for this process is provided below.
```
/* linux/zicio/zicio_shared_pool.c */
zicio_adjust_mapping_and_reclaim_pages()
|
-- zicio_unmap_pages_from_local_page_table()
    |
    -- for pages that the user has consumed or the expiration timeout has passed
        |
        -- zicio_ghost_unmapping() /* "ghost" means virtual memory that is not mapped to a physical memory */
        |   |
        |   -- pmd_clear() /* clear user's page table entry */
        |   |
        |   -- flush_tlb()
        |
        -- atomic_dec(&spcb->ref_count)
```

If you want to see the implementation details, we recommend starting with the provided code flow.

# Implementations on Legacy Databases

In this paper, we modified legacy databases, including MySQL, PostgreSQL, and Citus, to improve their data access mechanism by replacing a specific function call and evaluate our approach.  
We use our new **zicio_get_page** function exclusively for sequential and disjoint range scans in query plans. Our approach has limitations; it is not designed for mixed workloads, such as those involving updates. Currently, we leave it to support update operations.  
Detailed descriptions of these modifications are provided below.

## MySQL

To adapt *zicIO* to MySQL, we chose MyISAM as the storage engine rather than InnoDB, the default engine of MySQL, and modified the data file layout for these reasons:  
1. MyISAM: InnoDB, which has a clustered index, cannot notify *zicIO* where to read in the data file in advance, despite running a full scan.  
2. Paginated files: To make *zicIO* work well with requesting I/O in 2 MiB chunks, we paginated MyISAM's data in 2 MiB chunks.  
  
After that, we replaced **mysql_file_read**, which calls the **read** system call, with **zicio_get_page** and **memcpy** as shown below:  

```{r, tidy=FALSE, eval=FALSE, highlight=FALSE}
/* Original code flow */
Query_expression::ExecuteIteratorQuery()
    |
    -- TableScanIterator::Init()
    |   |
    |   -- mi_scan_init()
    |
    -- TableScanIterator::Read()
        |
        -- _mi_read_rnd_static_record()
        |   |
        |   -- mysql_file_read()  /* to record buffer of MySQL */
        |
       (or)
        |
        -- _mi_read_rnd_dynamic_record
            |
            -- mysql_file_read()  /* to record buffer of MySQL */


/* Modified code flow for the zicIO */
Query_expression::ExecuteIteratorQuery()
    |
    -- TableScanIterator::Init()
    |   |
    |   -- mi_scan_init()
    |       |
    |       -- zicio_open() /* notify the entire data file */
    |
    -- TableScanIterator::Read()
        |
        -- _mi_read_rnd_static_record()
        |   |
        |   -- zicio_get_page() /* get readable page */
        |   |
        |   -- memcpy() /* to record buffer of MySQL */
        |
       (or)
        |
        -- _mi_read_rnd_dynamic_record
            |
            -- zicio_get_page() /* get readable page */
            |
            -- memcpy() /* to record buffer of MySQL */
```
  
When we first init the scan, we call **zicio_open** without **zicio_notify_ranges** to inform **KzicIO** that the entire data file will be read. Afterwards, instead of calling **mysql_file_read**, we call **zicio_get_page** to get the address of **UzicIO**'s buffer memory where the record exist.  
To make the adaptation of **zicIO** user-friendly, we let the query execution layer copies record from **UzicIO** rather than using the memory address directly.  

## PostgreSQL

We substituted the standard **ReadBuffer** function, which handles reading data from disk into the buffer, with our custom
**zicio_get_page** function. This change is intended to replace PostgreSQL's existing data access path with our new data access path, which is like the call path below.

```{r, tidy=FALSE, eval=FALSE, highlight=FALSE}
/* src/backend/access/heap/heapam.c */

/* Original code flow */
heapgettup_pagemode()
    |
    -- heapgetpage()
        |
        -- ReadBufferExtended()


/* Modified code flow for the zicIO */
initscan()
    |
    -- zicio_notify_ranges() /* notify needed pages */

heapgettup_pagemode()
    |
    -- zicio_heapgetpage()
        |
        -- zicio_get_page() /* get readable page */
```

## Citus

The Citus database supports a columnar engine. We utilize only the columnar function, not the distributed function. The Citus columnar engine uses hierarchical storage layouts comprising a set of stripes (record groups), with each stripe consisting of chunks (column groups). These features are well-suited for analytical jobs. In Citus, before starting a scan on a stripe, the system checks whether it is necessary to read that area. If the area is deemed unnecessary, it is skipped.
 
During the checking phase, we call the **zicio_notify_ranges** function to deliver the needed page to the **KzicIO**. After that, we replaced ReadBuffer with **zicio_get_page**, similar to our modifications in PostgreSQL. The code flow below shows our modifications to the Citus code.

```{r, tidy=FALSE, eval=FALSE, highlight=FALSE}
/* src/backend/columnar/columnar_reader.c */

/* Original code flow */
LoadFilteredStripeBuffers()
    |
    -- LoadColumnBuffers()
        |
        -- ColumnarStorageRead()
            |
            -- ReadFromBlock()
                |
                -- ReadBuffer()

/* Modified code flow for the zicIO */
LoadFilteredStripeBuffers()
    |
    -- NotifyPagesInChunks()
    |   |
    |   -- zicio_notify_ranges() /* notify needed pages */
    |
    -- LoadColumnBuffers()
        |
        -- ColumnarStorageRead()
            |
            -- ReadFromBlock()
                |
                -- zicio_get_page() /* get readable page */
```

# How to Reproduce Evaluations with Prepared Scripts

Here, we provide scripts for the evaluations described in our paper. The following descriptions outline the settings and steps to reproduce specific evaluations.

## Information

- Evaluation setup of *zicIO* paper (server configuration):  
    - CPU: two AMD EPYC 7742 (128 cores)
    - RAM: 512 GiB
    - Device: PCIe 4.0 NVMe SSD (delivering 7 GiB/s for sequential reads)
- Evaluations:
    - Microbenchmark
    - Legacy DBMSs
- Hierarchy:
```
 sigmod25_zicio_artifact
 ├── build_and_install_zicio_kernel.sh
 ├── linux
 ├── libzicio
 └── evaluation
     ├── run.py: evaluation with legacy DBMSs
     └── microbenchmark: evaluation with microbenchmark
```

## QEMU Setting

We need to install the kernel to use zicIO. If you want to install the kernel directly in your environment without using QEMU, you can skip this section.

1. Create ubuntu image
``` bash
$ qemu-img create -f qcow2 ubuntu.img 64G

$ wget https://releases.ubuntu.com/20.04/ubuntu-20.04.6-live-server-amd64.iso

$ qemu-system-x86_64 \
-m 4G \
-drive file=ubuntu.img,if=virtio,cache=writethrough \
-cdrom ubuntu-20.04.6-live-server-amd64.iso \
-boot d
```

2. Create NVMe image
``` bash
$ qemu-img create -f qcow2 nvme.img 128G
```

3. Run the QEMU
``` bash
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
``` bash
$ ssh -p 12345 username@localhost
```

## Build and Install zicIO Kernel

1. Clone this repository.
``` bash
$ git clone https://github.com/anonymoususer90/sigmod25_zicio_artifact.git

$ cd sigmod25_zicio_artifact
```

2. Compile and install zicIO kernel.
``` bash
$ ./build_and_install_zicio_kernel.sh

$ sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 5.15.0-zicio+"

$ sudo reboot
```

3. Compile and install libzicio.
``` bash
$ cd sigmod25_zicio_artifact/libzicio

$ make test
```

## Preparation for evaluation

1. Fix build_and_install_zicio_kernel.sh.
``` bash
$ vim build_and_install_zicio_kernel.sh

./scripts/config -e CONFIG_ZICIO_STAT
./scripts/config --set-val CONFIG_ZICIO_PREAD_BREAKDOWN_LEVEL 1
```

2. Rebuild and install zicIO kernel.
``` bash
$ ./build_and_install_zicio_kernel.sh

$ sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 5.15.0-zicio+"

$ sudo reboot
```

## Run evaluation with microbenchmark

Workloads:  
1. Data ingestion with sequential scans (figure 6)  
2. Data ingestion with disjoint range scans (figure 7)  
3. Data ingestion with concurrent clients (figure 10)  
``` bash
$ cd evaluation/microbenchmark
$ ./microbenchmark.sh --device-path=${your_device_path} --data-dir=${your_data_path}
```

## Run evaluation with legacy DBMSs

Workloads (TPC-H queries 1~22, figure 8):  
1. Small, cold cache w/o *zicIO*  
2. Large, warm cache w/o *zicIO*  
3. Small, cold cache w/ *zicIO*
``` bash
$ cd evaluation

# build table for evaluation on mysql
$ python3 run.py --database=mysql --compile --init --create-table

# run evaluation of single process on mysql w/ zicIO
$ python3 run.py --database=mysql --device-path=${your_device_path} --vanilla --eval1-small

# run evaluation of single process on mysql w/o zicIO
$ python3 run.py --database=mysql --device-path=${your_device_path} --zicio --eval1-small --eval1-large



# build table for evaluation on postgres
$ python3 run.py --database=postgres --compile --init --create-table

# run evaluation of single process on postgres w/ zicIO
$ python3 run.py --database=postgres --device-path=${your_device_path} --zicio --eval1-small

# run evaluation of single process on postgres w/o zicIO
$ python3 run.py --database=postgres --device-path=${your_device_path} --vanilla --eval1-small --eval1-large --test-hot



# build table for evaluation on citus
$ python3 run.py --database=citus --compile --init --create-table

# run evaluation of single process on citus w/ zicIO
$ python3 run.py --database=citus --device-path=${your_device_path} --zicio --eval1-small --compile

# run evaluation of single process on citus w/o zicIO
$ python3 run.py --database=citus --device-path=${your_device_path} --vanilla --eval1-small --eval1-large --test-hot --compile
```
