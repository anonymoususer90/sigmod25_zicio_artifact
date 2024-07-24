import os

RESULTS_PATH=os.getcwd()
LOOP_CNT=0

def get_dat_str(lines, nr_procs, is_pread):
    elapsed_sum = 0
    elapsed_min = 1000000000000000
    elapsed_max = 0
    wait_sum = 0
    switch_sum = 0
    copy_sum = 0
    storage_sum = 0
    io_sum = 0

    for i in range(nr_procs):
        line = lines[i]
            
        if is_pread:
            mode_switch_latency = int(line.split(',')[3].split(':')[1])
            data_copy_latency = int(line.split(',')[4].split(':')[1])
            storage_latency = int(line.split(',')[5].split(':')[1])
            io_latency = int(line.split(',')[6].split(':')[1])
            wait_latency =(mode_switch_latency + data_copy_latency +
                           storage_latency + io_latency)
            
            switch_sum += mode_switch_latency
            copy_sum += data_copy_latency
            storage_sum += storage_latency
            io_sum += io_latency
        else:
            wait_latency = int(line.split(',')[2].split(':')[1])

        ingestion_latency = int(line.split(',')[1].split(':')[1])
        elapsed_latency = ingestion_latency + wait_latency
        
        elapsed_sum += elapsed_latency
        if elapsed_min > elapsed_latency:
            elapsed_min = elapsed_latency
        if elapsed_max < elapsed_latency:
            elapsed_max = elapsed_latency
            
        wait_sum += wait_latency
    
    return ("%2d %6.1lf %6.1lf %6.1lf %6.1lf %6.1lf %6.1lf %6.1lf %6.1lf\n"%
            (nr_procs,
             elapsed_sum / (nr_procs * 1e9),
             elapsed_min / 1e9,
             elapsed_max / 1e9,
             wait_sum / (nr_procs * 1e9),
             switch_sum / (nr_procs * 1e9),
             copy_sum / (nr_procs * 1e9),
             storage_sum / (nr_procs * 1e9),
             io_sum / (nr_procs * 1e9)))

def reformat(memory_size):
    raw_dir ="%s/multi_clients_%d_%s"%(RESULTS_PATH, LOOP_CNT, memory_size)
    
    if os.path.isdir(raw_dir):
        results_dir ="%s/results_%d_%s"%(RESULTS_PATH, LOOP_CNT, memory_size)
        os.mkdir(results_dir)
        
        files = []
        
        files.append(["pread_X_%d_4096"%(LOOP_CNT), True])
        files.append(["pread_O_%d_4096"%(LOOP_CNT), True])
        files.append(["pread_X_%d_8192"%(LOOP_CNT), True])
        files.append(["pread_O_%d_8192"%(LOOP_CNT), True])
        files.append(["pread_X_%d_16384"%(LOOP_CNT), True])
        files.append(["pread_O_%d_16384"%(LOOP_CNT), True])
        files.append(["zicio_X_%d"%(LOOP_CNT), False])
        files.append(["zicio_O_%d"%(LOOP_CNT), False])

        for f in files:
            f_name = f[0]
            is_pread = f[1]

            result_file = open("%s/%s.dat"%(results_dir, f_name), "w")

            for order in range(5):
                if order == 1:
                    continue

                nr_procs = 2**order

                with open("%s/%s_%d.dat"%(raw_dir, f_name, nr_procs), "r") as f:
                    lines = f.readlines()

                result_file.write(get_dat_str(lines, nr_procs, is_pread))
                    
            result_file.close()
    
if __name__ == "__main__":
    reformat("small_16")
    reformat("large_256")