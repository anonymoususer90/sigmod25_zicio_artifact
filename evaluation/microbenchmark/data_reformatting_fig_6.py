import os

RESULTS_PATH=os.getcwd()
LOOP_CNT=0

zicio_elapsed_time = 0
zicio_ingestion_time = 0
zicio_wait_time = 0
zicio_cycles = 0
zicio_instructions = 0

def get_dat_str(lines, buf_size):
    global zicio_elapsed_time, zicio_ingestion_time, zicio_wait_time
    global zicio_cycles, zicio_instructions

    elapsed_time = int(lines[4].split(',')[0])
    normalized_elapsed_time = elapsed_time / zicio_elapsed_time

    ingestion_time = int(lines[0].split(',')[0].split(':')[1])
    normalized_ingestion_time = (normalized_elapsed_time *
                                (ingestion_time / elapsed_time))

    wait_time = int(lines[0].split(',')[1].split(':')[1])
    normalized_wait_time = (normalized_elapsed_time *
                            (wait_time / elapsed_time))

    submit_time = int(lines[0].split(',')[2].split(':')[1])
    normalized_submit_time = (normalized_elapsed_time *
                            (submit_time / elapsed_time))

    cycles = int(lines[2].split(',')[0])
    normalized_cycles = cycles / zicio_cycles
    instructions = int(lines[3].split(',')[0])
    normalized_instructions = instructions / zicio_instructions
    
    return "%d %.3lf %.3lf %.3lf %.3lf %.3lf %.3lf\n"%(buf_size,
            normalized_elapsed_time, normalized_ingestion_time,
            normalized_wait_time, normalized_submit_time, normalized_cycles,
            normalized_instructions)

def reformat():
    global zicio_elapsed_time, zicio_ingestion_time, zicio_wait_time
    global zicio_cycles, zicio_instructions
    
    raw_dir ="%s/seq_raw_results_%d"%(RESULTS_PATH, LOOP_CNT)
    
    if os.path.isdir(raw_dir):
        results_dir ="%s/seq_results_%d"%(RESULTS_PATH, LOOP_CNT)
        os.mkdir(results_dir)

        with open("%s/zicio_%d.dat"%(raw_dir, LOOP_CNT), "r") as f:
            lines = f.readlines()

        zicio_elapsed_time = int(lines[4].split(',')[0])
        zicio_ingestion_time = int(lines[0].split(',')[0].split(':')[1])
        zicio_wait_time = int(lines[0].split(',')[1].split(':')[1])
        zicio_cycles = int(lines[2].split(',')[0])
        zicio_instructions = int(lines[3].split(',')[0])

        with open("%s/zicio_%d.dat"%(results_dir, LOOP_CNT), "w") as f:
            line = "%d %.1lf %.3lf %.3lf %.3lf %.1lf %.1lf\n"%(2097152,
                    zicio_elapsed_time / 1e9, zicio_ingestion_time / zicio_elapsed_time,
                    zicio_wait_time / zicio_elapsed_time, 0.000,
                    zicio_cycles / 1e9, zicio_instructions / 1e9)
            for i in range(5):
                f.write(line)
        
        uring_types = ["O_O", "X_O", "O_X", "X_X"]
        pread_types = ["O", "X"]

        files = []
        for ut in uring_types:
            files.append(open("%s/uring_%s_%d.dat"%
                                (results_dir, ut, LOOP_CNT), "w"))
        for pt in pread_types:
            files.append(open("%s/pread_%s_%d.dat"%
                                (results_dir, pt, LOOP_CNT), "w"))

        for order in [12, 13, 14, 16, 18]:
            buf_size = 2**order
            for i in range(4):
                with open("%s/uring_%d_%s_0.dat"%
                            (raw_dir, buf_size, uring_types[i]), "r") as f:
                    lines = f.readlines()

                files[i].write(get_dat_str(lines, buf_size))

            for i in range(4, 6):
                with open("%s/pread_%d_%s_0.dat"%
                            (raw_dir, buf_size, pread_types[i-4]), "r") as f:
                    lines = f.readlines()

                files[i].write(get_dat_str(lines, buf_size))

        for f in files:
            f.close()
    
if __name__ == "__main__":
    reformat()