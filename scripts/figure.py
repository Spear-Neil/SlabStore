import subprocess
import os
import re
import shutil
import matplotlib.pyplot as plt

# global parameters
log_dir = "./logs/"
store_path = "/mnt/pmem0/ycsb-store"
store_size = 128
key_size, val_size = 8, 32
records_num = 100000000
run_duration = 60
enable_pcm = 1

stores = ["pmemkv", "BasicSlabStore", "SlabStore", "Plush", "Viper", "RocksDB"]
# stores = ["pmemkv", "BasicSlabStore"]
colors = ['blue', 'steelblue', 'red', 'orange', 'green', 'purple', 'gray']
markers = ["o", "X", "d", "s", "^", "v", "P"]


def remove_path(path):
    if os.path.exists(path):
        if os.path.isdir(path):
            shutil.rmtree(path)
        else:
            os.remove(path)


def build_project():
    if not os.path.exists("./build"):
        res = subprocess.run(["cmake", "-DCMAKE_BUILD_TYPE=Release", "-B ./build", ".."])
        print("cmake ./build/\n", res.stdout)
        res = subprocess.run(["cmake", "--build", "./build", "-j 32"])
        print("cmake --build ./build/\n", res.stdout)

    if not os.path.exists(log_dir):
        os.mkdir(log_dir)


def figure_scalability():
    threads = [1, 2, 4, 8, 16, 24, 32, 40, 48]
    read_ratios = [100, 75, 50, 25, 0]
    workloads = ["Read-Only", "Read-Heavy", "Balanced", "Write-Heavy", "Write-Only"]
    distributions = ["unif", "zipf"]

    fig = plt.figure(figsize=(20, 6))
    row, col = len(distributions), len(read_ratios)

    for rid in range(len(distributions)):  # distribution [unif, zipf]
        for cid in range(len(read_ratios)):  # read ratio
            plt.subplot(row, col, rid * col + cid + 1)
            for sid in range(len(stores)):  # kv store types
                perf = []
                for nthd in threads:  # worker threads number in run phase
                    remove_path(store_path)
                    ycsb_test = ["./build/test/test-ycsb-test", store_path, str(store_size),
                                 str(sid), str(nthd), str(records_num), str(key_size), str(val_size),
                                 str(read_ratios[cid]), str(run_duration), str(enable_pcm), str(rid)]
                    log_name = ('scale-' + str(store_size) + '-' + str(sid) + '-' + str(nthd) + '-' + str(records_num)
                                + '-' + str(key_size) + '-' + str(val_size) + '-' + str(read_ratios[cid])
                                + '-' + str(run_duration) + '-' + str(enable_pcm) + '-' + str(rid) + '.log')
                    log_path = os.path.join(log_dir, log_name)

                    result = ""

                    if os.path.exists(log_path):  # read existing logs
                        with open(log_path, "r") as log:
                            result = log.read()
                        if not re.search(r"run phase.*?throughput:\s*([\d.]+)", result):
                            result = ""  # existing logs are broken
                            remove_path(log_path)

                    if not result:
                        result = subprocess.run(ycsb_test, capture_output=True, text=True).stdout
                        with open(log_path, 'a+') as log:
                            log.write(str(ycsb_test) + "\n" + result + "\n\n")

                    match = re.search(r"run phase.*?throughput:\s*([\d.]+)", result)
                    if not match:
                        exit("unknown error, match failed")
                    perf.append(float(match.group(1)))
                plt.plot(threads, perf, label=stores[sid], marker=markers[sid], color=colors[sid],
                         linewidth=2, markersize=10, markeredgewidth=0.4, markeredgecolor='black', alpha=0.95)
            plt.xticks(threads[1:])
            plt.xlim(0, threads[-1] + 1)
            plt.grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.4)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    fig.legend(lines, labels, loc='upper center', ncol=len(stores), bbox_to_anchor=(0.5, 1.14), fontsize=15)
    fig.text(-0.03, 0.5, 'Million Operations per Second', va='center', rotation='vertical', fontsize=15)
    fig.text(0.485, -0.02, "Threads", va='center', fontsize=15)

    vertical_begin, ver_step = 1 - 1.0 / row / 2, 1.0 / row
    horizontal_begin, hor_step = 1.0 / col / 2, 1.0 / col
    for rid in range(0, row):
        fig.text(-0.007, vertical_begin - rid * ver_step, distributions[rid], va='center', ha='center',
                 rotation='vertical', fontsize=15)
    for cid in range(0, col):
        fig.text(horizontal_begin + cid * hor_step, 1.01, workloads[cid], va='center', ha='center', fontsize=15)
    fig.savefig('scale-result.pdf', bbox_inches='tight')
    fig.show()


if __name__ == "__main__":
    build_project()

    plt.rcParams['axes.linewidth'] = 2
    plt.rcParams['xtick.major.width'] = 2
    plt.rcParams['ytick.major.width'] = 2

    plt.rcParams['pdf.fonttype'] = 42
    plt.rcParams['ps.fonttype'] = 42
    plt.rcParams['font.weight'] = 'medium'

    figure_scalability()
