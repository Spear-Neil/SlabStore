import subprocess
import os
import re
import shutil
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FormatStrFormatter
from matplotlib.gridspec import GridSpec

# global parameters
log_dir = "./logs/"
pm_script = "./pm-script.py"


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


def figure_core_ops():  # fixed-size record (8-byte key, 8-byte value)
    operation_count = 200000000  # total operation count in run phase
    additional_count = 1000000
    latency_sampling = 0.01

    threads = [1, 2, 4, 8, 16, 24, 32, 40, 48]
    workloads = ["Insert", "Lookup", "Remove", "Mixed"]
    insert_ratio = [1.0, 0, 0, 0.5]
    lookup_ratio = [0, 1.0, 0, 0.5]
    remove_ratio = [0, 0, 1.0, 0]
    objects = ["pmemkv", "SlabStore", "Plush(fixed-size)", "Viper(fixed-size)", "Dash", "FAST+FAIR", "FPTree", "uTree"]
    libs = ["pmemkv", "slabstore", "plush", "viper", "dash", "fastfair", "fptree", "utree"]
    colors = ['blue', 'red', 'orange', 'green', 'purple', 'gray', 'steelblue', 'brown']
    markers = ["o", "d", "s", "^", "X", "P", "v", "*"]

    for wid in range(len(workloads)):  # workloads: core kvs operations
        if workloads[wid] == "Insert":
            record_count = additional_count
        elif workloads[wid] == "Remove":
            record_count = operation_count + additional_count
        else:  # Lookup / Mixed
            record_count = operation_count

        assert len(objects) == len(libs)
        for oid in range(len(objects)):  # indexes and stores
            input_lib = "./build/test/libpibench-" + libs[oid] + ".so"
            for nthd in threads:
                pibench = ["./build/test/PiBench", input_lib, "-n", str(record_count), "-p",
                           str(operation_count), "-i", str(insert_ratio[wid]), "-r", str(lookup_ratio[wid]), "-d",
                           str(remove_ratio[wid]), "--latency_sampling", str(latency_sampling), "-t", str(nthd),
                           "--script", str(pm_script)]
                log_name = ('pibench-' + workloads[wid] + '-' + libs[oid] + '-' + str(nthd) + '.log')
                log_path = os.path.join(log_dir, log_name)

                if not os.path.exists(log_path):
                    remove_path("/mnt/pmem0/pibench")
                    os.mkdir("/mnt/pmem0/pibench")
                    result = subprocess.run(pibench, capture_output=True, text=True).stdout
                    with open(log_path, "w") as log:
                        log.write(str(pibench) + "\n" + result)
    remove_path("/mnt/pmem0/pibench")

    # figure throughput
    fig = plt.figure(figsize=(10, 7.5))
    row, col = 2, 2
    for rid in range(row):
        for cid in range(col):
            wid = rid * col + cid
            plt.subplot(row, col, wid + 1)
            for oid in range(len(objects)):  # indexes and stores
                perf = []
                for nthd in threads:
                    log_name = ('pibench-' + workloads[wid] + '-' + libs[oid] + '-' + str(nthd) + '.log')
                    log_path = os.path.join(log_dir, log_name)
                    with open(log_path) as log:
                        result = log.read()
                        if not result:
                            exit("Unknown error: " + log_path)
                        match = re.search(r"Completed:\s*(\d+\.\d+)", result)
                        if workloads[wid] == "Remove":  # some indexes failed to remove a lot of records
                            match = re.search(r"Succeeded:\s*(\d+\.\d+)", result)
                        if not match:
                            exit("unknown error, match failed")
                        perf.append(float(match.group(1)) / 1000000)
                plt.plot(threads, perf, label=objects[oid], marker=markers[oid], color=colors[oid],
                         linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
            plt.xticks(threads[1:])
            plt.xlim(0, threads[-1] + 1)
            plt.axvspan(threads[-1] / 2, threads[-1] + 1, color='lightgrey', alpha=0.8)
            plt.grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.4)
            plt.title(workloads[wid], x=0.5, y=1.02, fontsize=15)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    fig.legend(lines, labels, loc='upper center', ncol=len(objects) / 2, bbox_to_anchor=(0.5, 1.11), fontsize=15)
    fig.text(-0.03, 0.5, 'Million Operations per Second', va='center', rotation='vertical', fontsize=15)
    fig.text(0.485, -0.02, "Threads", va='center', fontsize=15)
    fig.savefig("core-ops-tpt.pdf", bbox_inches='tight')
    fig.show()

    loads = ["Insert", "Lookup"]
    nthd = 24

    # figure tail latency
    pattern = ["min", "50%", "90%", "99%", "99.9%"]
    fig = plt.figure(figsize=(10, 3.8))
    row, col = 1, 2
    assert (col == len(loads))
    for lid in range(len(loads)):
        plt.subplot(row, col, lid + 1)
        for oid in range(len(objects)):
            latency = []
            log_name = ('pibench-' + loads[lid] + '-' + libs[oid] + '-' + str(nthd) + '.log')
            log_path = os.path.join(log_dir, log_name)
            with open(log_path) as log:
                result = log.read()
                if not result:
                    exit("Unknown error: " + log_path)
                for pid in range(len(pattern)):
                    escaped_pattern = re.escape(pattern[pid])
                    match = re.search(rf"{escaped_pattern}:\s*(\d+)", result)
                    if not match:
                        exit("unknown error, match failed")
                    latency.append(float(match.group(1)) / 1000)  # us
            plt.plot(pattern, latency, label=objects[oid], marker=markers[oid], color=colors[oid],
                     linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
        plt.title(loads[lid], y=-0.2, fontsize=15)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    # fig.legend(lines, labels, loc='upper center', ncol=len(objects) / 2, bbox_to_anchor=(0.5, 1.22), fontsize=15)
    fig.text(-0.03, 0.5, 'Latency [us]', va='center', rotation='vertical', fontsize=15)
    fig.savefig("core-ops-latency.pdf", bbox_inches='tight')
    fig.show()

    # figure DRAM/PMEM access
    types = ["DRAM Reads", "DRAM Writes", "PMEM Reads", "PMEM Writes", "PMEM Media Reads", "PMEM Media Writes"]
    patterns = ["DRAM Reads (bytes)", "DRAM Writes (bytes)", "NVM Reads (bytes)",
                "NVM Writes (bytes)", "TotalMediaReads (bytes)", "TotalMediaWrites (bytes)"]
    colors = ['lightsalmon', 'darkorange', 'lightblue', 'steelblue', 'lightgreen', 'forestgreen']
    hatches = ['/', '\\', '/', '\\', '/', '\\']

    fig = plt.figure(figsize=(30, 6))
    row, col = len(loads), len(objects)
    y_max = [[0, 0], [0, 0]]
    wide, narrow = -0.1, -0.24
    gs = GridSpec(row, col * 2 - 1, figure=fig,
                  width_ratios=[1, wide, 1, narrow, 1, narrow, 1, wide,
                                1, narrow, 1, narrow, 1, narrow, 1])
    for lid in range(len(loads)):  # two row, insert/lookup
        for oid in range(len(objects)):  # indexes and stores
            ax = fig.add_subplot(gs[lid, oid * 2])
            access_bytes = []
            log_name = ('pibench-' + loads[lid] + '-' + libs[oid] + '-' + str(nthd) + '.log')
            log_path = os.path.join(log_dir, log_name)
            with open(log_path) as log:
                result = log.read()
                if not result:
                    exit("Unknown error: " + log_path)
                for tid in range(len(types)):  # access types
                    escaped_pattern = re.escape(patterns[tid])
                    match = re.search(rf"{escaped_pattern}:\s*(\d+)", result)
                    if not match:
                        exit("unknown error, match failed")
                    access_bytes.append(float(match.group(1)) / (operation_count * 1000))

            x = np.arange(len(types))
            ax.bar(x, access_bytes, color=colors, hatch=hatches, label=types, alpha=1)
            ax.set_xticks([])
            if lid == len(loads) - 1:
                ax.set_title(objects[oid], y=-0.2, fontsize=15)

            if libs.index("slabstore") <= oid <= libs.index("viper"):
                cur_max = max(access_bytes)
                y_max[lid][0] = cur_max if cur_max > y_max[lid][0] else y_max[lid][0]
            if libs.index("dash") <= oid <= libs.index("utree"):
                cur_max = max(access_bytes)
                y_max[lid][1] = cur_max if cur_max > y_max[lid][1] else y_max[lid][1]

    # adjust ylim
    scale = 1.05
    for lid in range(len(loads)):  # two row, insert/lookup
        for oid in range(len(objects)):  # indexes and stores
            subplot = fig.axes[lid * col + oid]
            subplot.yaxis.set_major_formatter(FormatStrFormatter('%.1f'))
            if libs.index("slabstore") <= oid <= libs.index("viper"):
                subplot.set_ylim(0, y_max[lid][0] * scale)
                if oid != libs.index("slabstore"):
                    subplot.tick_params(axis='y', length=0)
                    subplot.set_yticklabels([])
            if libs.index("dash") <= oid <= libs.index("utree"):
                subplot.set_ylim(0, y_max[lid][1] * scale)
                if oid != libs.index("dash"):
                    subplot.tick_params(axis='y', length=0)
                    subplot.set_yticklabels([])
            subplot.grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.5)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    fig.legend(lines, labels, loc='upper center', ncol=len(types), bbox_to_anchor=(0.5, 1.1), fontsize=15)
    fig.text(-0.01, 0.5, 'Kilobytes per Operation', va='center', rotation='vertical', fontsize=15)
    fig.text(1.005, 0.75, loads[0], va='center', rotation=270, fontsize=15)
    fig.text(1.005, 0.25, loads[1], va='center', rotation=270, fontsize=15)
    fig.savefig("core-ops-access.pdf", bbox_inches='tight')
    fig.show()


def figure_scalability(key_size, val_size, only_unif=False):
    store_path = "/mnt/pmem0/ycsb-store"
    store_size = 128
    records_num = 200000000
    run_duration = 60
    enable_pcm = 1

    # the order can not be changed
    stores = ["pmemkv", "BasicSlabStore", "SlabStore", "Plush", "Viper", "RocksDB"]
    colors = ['blue', 'steelblue', 'red', 'orange', 'green', 'purple', 'gray', 'brown']
    markers = ["o", "X", "d", "s", "^", "v", "P", "*"]

    threads = [1, 2, 4, 8, 16, 24, 32, 40, 48]
    read_ratios = [100, 75, 50, 25, 0]
    workloads = ["Read-Only", "Read-Heavy", "Balanced", "Write-Heavy", "Write-Only"]
    distributions = ["unif", "zipf"]
    height = 6
    if only_unif: distributions, height = ["unif"], 3

    fig = plt.figure(figsize=(20, height))
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
                                 str(read_ratios[cid]), str(run_duration), str(enable_pcm), str(rid),
                                 str(48), str(pm_script)]
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
                marker_size = 12
                if cid == 0 and sid == 1: marker_size = 14
                plt.plot(threads, perf, label=stores[sid], marker=markers[sid], color=colors[sid],
                         linewidth=3, markersize=marker_size, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
            plt.xticks(threads[1:])
            plt.xlim(0, threads[-1] + 1)
            plt.axvspan(threads[-1] / 2, threads[-1] + 1, color='lightgrey', alpha=0.8)
            plt.grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.4)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    if not only_unif:
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
    fig_name = 'scale-result-' + str(key_size) + '-' + str(val_size) + '.pdf'
    fig.savefig(fig_name, bbox_inches='tight')
    fig.show()


def figure_ycsb_insert():
    store_path = "/mnt/pmem0/ycsb-store"
    store_size = 128
    records_num = 200000000

    # the order can not be changed
    stores = ["pmemkv", "BasicSlabStore", "SlabStore", "Plush", "Viper", "RocksDB"]
    colors = ['blue', 'steelblue', 'red', 'orange', 'green', 'purple', 'gray', 'brown']
    markers = ["o", "X", "d", "s", "^", "v", "P", "*"]
    threads = [1, 2, 4, 8, 16, 24, 32, 40, 48]
    records_size = [[8, 32], [32, 200]]

    for rid in range(len(records_size)):
        for sid in range(len(stores)):
            for nthd in threads:
                key_size, val_size = records_size[rid]
                ycsb_insert = ["./build/test/test-ycsb-test", store_path, str(store_size),
                               str(sid), str(nthd), str(records_num), str(key_size), str(val_size),
                               str(0), str(0), str(0), str(0), str(nthd), str(pm_script)]
                log_name = ("ycsb-insert-" + str(rid) + "-" + str(sid) + "-" + str(nthd) + ".log")
                log_path = os.path.join(log_dir, log_name)
                if not os.path.exists(log_path):
                    remove_path(store_path)
                    result = subprocess.run(ycsb_insert, capture_output=True, text=True).stdout
                    with open(log_path, 'w') as log:
                        log.write(str(ycsb_insert) + "\n" + result + "\n\n")
    remove_path(store_path)

    fig, axes = plt.subplots(1, len(records_size), figsize=(10, 3.8))
    for rid in range(len(records_size)):
        for sid in range(len(stores)):
            perf = []
            for nthd in threads:
                log_name = ("ycsb-insert-" + str(rid) + "-" + str(sid) + "-" + str(nthd) + ".log")
                log_path = os.path.join(log_dir, log_name)
                with open(log_path) as log:
                    result = log.read()
                    if not result: exit("unknown error, " + log_name)
                    match = re.search(r"load phase.*?throughput:\s*([\d.]+)", result)
                    if not match: exit("unknown error, match failed")
                    perf.append(float(match.group(1)))
            axes[rid].plot(threads, perf, label=stores[sid], marker=markers[sid], color=colors[sid],
                           linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
        axes[rid].set_xticks(threads[1:])
        axes[rid].set_xlim(0, threads[-1] + 1)
        axes[rid].axvspan(threads[-1] / 2, threads[-1] + 1, color='lightgrey', alpha=0.8)
        axes[rid].grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.4)
        axes[rid].set_title(str(records_size[rid]), x=0.5, y=1.02, fontsize=15)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    fig.legend(lines, labels, loc='upper center', ncol=len(stores), bbox_to_anchor=(0.5, 1.14), fontsize=15)
    fig.text(-0.03, 0.50, 'Million Operations per Second', va='center', rotation='vertical', fontsize=15)
    fig.text(0.485, -0.02, "Threads", va='center', fontsize=15)
    fig.savefig("ycsb-insert.pdf", bbox_inches='tight')
    fig.show()


if __name__ == "__main__":
    build_project()

    plt.rcParams['axes.linewidth'] = 2
    plt.rcParams['xtick.major.width'] = 2
    plt.rcParams['ytick.major.width'] = 2

    plt.rcParams['pdf.fonttype'] = 42
    plt.rcParams['ps.fonttype'] = 42
    plt.rcParams['font.weight'] = 'medium'

    figure_core_ops()

    figure_scalability(8, 32, False)
    figure_scalability(32, 200, True)

    figure_ycsb_insert()
