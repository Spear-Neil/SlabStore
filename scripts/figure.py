import subprocess
import os
import re
import shutil
import matplotlib.pyplot as plt
import numpy as np
from matplotlib import gridspec
from matplotlib.ticker import FormatStrFormatter
from matplotlib.gridspec import GridSpec
from matplotlib.ticker import MaxNLocator
from matplotlib.patches import Patch

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
    fig = plt.figure(figsize=(9.5, 7.5))
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
            plt.axvspan(threads[-1] / 2, threads[-1] + 1, color='lightgrey', alpha=0.4)
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
            plt.axvspan(threads[-1] / 2, threads[-1] + 1, color='lightgrey', alpha=0.4)
            plt.grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.4)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    if not only_unif:
        fig.legend(lines, labels, loc='upper center', ncol=len(stores), bbox_to_anchor=(0.5, 1.14), fontsize=15)
    text_size = 15 if not only_unif else 14
    fig.text(-0.03, 0.5, 'Million Operations per Second', va='center', rotation='vertical', fontsize=text_size)
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


def figure_ycsb_access(key_size, val_size):
    stores = ["pmemkv", "BasicSlabStore", "SlabStore", "Plush", "Viper", "RocksDB"]
    workloads = ["Read-Only", "Read-Heavy", "Balanced", "Write-Heavy", "Write-Only"]
    read_ratios = [100, 75, 50, 25, 0]
    store_path = "/mnt/pmem0/ycsb-store"
    store_size = 128
    records_num = 200000000
    run_duration = 60
    enable_pcm = 1

    nthd = 24
    sids = [1, 2, 3, 4]
    loads_indexes = [0, 4]  # "Read-Only" & "Write-Only"

    # the scale-*.log pmem access results are unreasonable
    for wid in loads_indexes:
        for sid in sids:
            ycsb_access = ["./build/test/test-ycsb-test", store_path, str(store_size),
                           str(sid), str(nthd), str(records_num), str(key_size), str(val_size),
                           str(read_ratios[wid]), str(run_duration), str(enable_pcm), str(0),
                           str(48), str(pm_script)]
            log_name = ("ycsb-access-" + str(wid) + "-" + str(sid) + "-" +
                        str(key_size) + "-" + str(val_size) + ".log")
            log_path = os.path.join(log_dir, log_name)
            if not os.path.exists(log_path):
                remove_path(store_path)
                result = subprocess.run(ycsb_access, capture_output=True, text=True).stdout
                with open(log_path, 'w') as log:
                    log.write(str(ycsb_access) + "\n" + result + "\n\n")
    remove_path(store_path)

    types = ["DRAM Reads", "DRAM Writes", "PMEM Reads", "PMEM Writes", "PMEM Media Reads", "PMEM Media Writes"]
    patterns = ["Mem Reads:", "Mem Writes:", "TotalReadRequests (bytes):", "TotalWriteRequests (bytes):",
                "TotalMediaReads (bytes):", "TotalMediaWrites (bytes):"]
    colors = ['lightsalmon', 'darkorange', 'lightblue', 'steelblue', 'lightgreen', 'forestgreen']
    hatches = ['/', '\\', '/', '\\', '/', '\\']
    row, col = len(loads_indexes), len(sids)
    fig = plt.figure(figsize=(10, 4))
    wide, narrow = -0.1, -0.08
    width = [1, narrow, 1, narrow, 1, narrow, 1]
    gs = GridSpec(row, col * 2 - 1, figure=fig, width_ratios=width)

    y_max = [0, 0]
    slabstore_media_writes = []
    for rid in range(len(loads_indexes)):
        for cid in range(len(sids)):
            ax = fig.add_subplot(gs[rid, cid * 2])
            wid, sid = loads_indexes[rid], sids[cid]
            log_name = ("ycsb-access-" + str(wid) + "-" + str(sid) + "-" +
                        str(key_size) + "-" + str(val_size) + ".log")
            log_path = os.path.join(log_dir, log_name)
            volumes = []
            with open(log_path) as log:
                result = log.read()
                if not result: exit("unknown error, no result")
                for tid in range(len(types)):
                    escaped_pattern = re.escape(patterns[tid])
                    res = re.findall(rf'{escaped_pattern}\s*([\d.+e]+)', result)
                    count = re.findall(r'total operation count:\s*(\d+)', result)
                    MiB = 1024 * 1024 if tid == 0 or tid == 1 else 1
                    volumes.append(float(res[-1]) * MiB / 1000 / float(count[-1]))

            x = np.arange(len(types))
            ax.bar(x, volumes, color=colors, hatch=hatches, label=types, alpha=1)
            ax.set_xticks([])
            if rid == len(loads_indexes) - 1:
                ax.set_title(stores[sid], y=-0.25, fontsize=12)
                assert (sid - 1 == cid)
                if sid == 1 or sid == 2: slabstore_media_writes.append(volumes[-1])

            cur_max = max(volumes)
            y_max[rid] = cur_max if cur_max > y_max[rid] else y_max[rid]

    # adjust ylim
    scale = 1.05
    for rid in range(len(loads_indexes)):
        for cid in range(len(sids)):
            subplot = fig.axes[rid * col + cid]
            subplot.yaxis.set_major_formatter(FormatStrFormatter('%.1f'))
            subplot.yaxis.set_major_locator(MaxNLocator(nbins=6))
            subplot.set_ylim(0, y_max[rid] * scale)
            if cid != 0:
                subplot.tick_params(axis='y', length=0)
                subplot.set_yticklabels([])
            subplot.grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.5)

            # add highlight rectangle for slabstore and basic-slabstore
            sid = sids[cid]
            if rid == len(loads_indexes) - 1 and (sid == 1 or sid == 2):  # SlabStore
                rect = plt.Rectangle((0.5, -0.0), 0.48, 0.01 + slabstore_media_writes[cid] * scale / y_max[rid],
                                     transform=subplot.transAxes, linewidth=2, edgecolor='red', facecolor='none',
                                     alpha=0.8)
                subplot.add_patch(rect)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    fig.legend(lines, labels, loc='upper center', ncol=len(types), bbox_to_anchor=(0.5, 1.1), fontsize=10)
    fig.text(-0.01, 0.5, 'Kilobytes per Operation', va='center', rotation='vertical', fontsize=12)
    fig.text(1.005, 0.75, workloads[loads_indexes[0]], va='center', rotation=270, fontsize=12)
    fig.text(1.005, 0.25, workloads[loads_indexes[1]], va='center', rotation=270, fontsize=12)
    file_name = "ycsb-access-" + str(key_size) + "-" + str(val_size) + ".pdf"
    fig.savefig(file_name, bbox_inches='tight')
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
        axes[rid].axvspan(threads[-1] / 2, threads[-1] + 1, color='lightgrey', alpha=0.4)
        axes[rid].grid(axis='y', color='darkgray', linestyle=':', linewidth=2, alpha=0.4)
        axes[rid].set_title(str(records_size[rid]), x=0.5, y=1.02, fontsize=15)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    fig.legend(lines, labels, loc='upper center', ncol=len(stores), bbox_to_anchor=(0.5, 1.14), fontsize=13)
    fig.text(-0.03, 0.50, 'Million Operations per Second', va='center', rotation='vertical', fontsize=13)
    fig.text(0.485, -0.02, "Threads", va='center', fontsize=15)
    fig.savefig("ycsb-insert.pdf", bbox_inches='tight')
    fig.show()


def figure_size_sensitivity_and_recovery():
    sizes = [[8, 8], [16, 32], [32, 64], [32, 200], [64, 400], [128, 800]]
    store_path = "/mnt/pmem0/ycsb-store"
    store_size = 484
    records_num = 200000000
    read_ratio = 50  # balanced
    run_drt = 60

    stores = ["pmemkv", "BasicSlabStore", "SlabStore", "Plush", "Viper"]  # , "RocksDB"]
    colors = ['blue', 'steelblue', 'red', 'orange', 'green', 'purple', 'gray', 'brown']
    markers = ["o", "X", "d", "s", "^", "v", "P", "*"]

    nthd = 48
    # size sensitivity
    for sid in range(len(stores)):
        for kv_size in sizes:
            key_size, val_size = kv_size
            ycsb_size = ["./build/test/test-ycsb-test", store_path, str(store_size),
                         str(sid), str(nthd), str(records_num), str(key_size), str(val_size),
                         str(read_ratio), str(run_drt), str(0), str(0), str(nthd), str(pm_script)]
            log_name = ("ycsb-size-" + str(nthd) + "-" + str(sid) + "-" + str(key_size) + "-" + str(val_size) + ".log")
            log_path = os.path.join(log_dir, log_name)
            if not os.path.exists(log_path):
                remove_path(store_path)
                result = subprocess.run(ycsb_size, capture_output=True, text=True).stdout
                with open(log_path, 'w') as log:
                    log.write(str(ycsb_size) + "\n" + result + "\n\n")
    remove_path(store_path)

    # configure kNBucketInSegment as 128 for record number 400 million
    # to prevent directory size from growing larger than 4MiB
    restart_nums = [100000000, 200000000, 400000000]
    restart_sizes = [[16, 32], [32, 200], [64, 400]]
    restart_types = ["recover", "reboot"]  # the order cannot be changed

    # slabstore recovery time
    for rec_num in restart_nums:
        for kv_size in restart_sizes:
            key_size, val_size = kv_size
            remove_path(store_path)
            insert_script = ["./build/test/test-ycsb-test", store_path, str(store_size),
                             str(2), str(nthd), str(rec_num), str(key_size), str(val_size),
                             str(100), str(0), str(0), str(0), str(nthd), str(pm_script)]
            exist = True
            for restart_type in restart_types:
                log_name = ("ycsb-restart-" + str(rec_num) + "-" + str(key_size) + "-" + str(
                    val_size) + "-" + restart_type + ".log")
                log_path = os.path.join(log_dir, log_name)
                if not os.path.exists(log_path): exist = False
            if not exist: subprocess.run(insert_script, capture_output=True, text=True)

            for restart_type in restart_types:
                restart_script = ["./build/test/test-ycsb-restart", store_path]
                log_name = ("ycsb-restart-" + str(rec_num) + "-" + str(key_size) + "-" + str(
                    val_size) + "-" + restart_type + ".log")
                log_path = os.path.join(log_dir, log_name)
                if not os.path.exists(log_path):
                    result = subprocess.run(restart_script, capture_output=True, text=True).stdout
                    with open(log_path, 'w') as log:
                        log.write(str(restart_script) + "\n" + result + "\n\n")
    remove_path(store_path)

    fig = plt.figure(figsize=(11, 6))
    gs = GridSpec(3, 3, figure=fig, height_ratios=[1, -0.1, 1], width_ratios=[1, -0.1, 1])

    left = fig.add_subplot(gs[:, 0])
    # throughput of YCSB Balance workload under different record sizes
    for sid in range(len(stores)):
        perf = []
        for kv_size in sizes:
            key_size, val_size = kv_size
            log_name = ("ycsb-size-" + str(nthd) + "-" + str(sid) + "-" + str(key_size) + "-" + str(val_size) + ".log")
            log_path = os.path.join(log_dir, log_name)
            with open(log_path) as log:
                result = log.read()
                if not result: exit("unknown error, " + log_name)
                match = re.search(r"run phase.*throughput:\s*([\d.]+)", result)
                if not match: exit("unknown error, match failed")
                perf.append(float(match.group(1)))
        xticks = [str(item) for item in sizes]
        left.plot(xticks, perf, label=stores[sid], marker=markers[sid], color=colors[sid],
                  linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
        left.set_ylabel('Million Operations per Second', fontsize=12)
        left.set_title('(a) Throughput of YCSB Balanced workload.', x=0.5, y=-0.15, fontsize=12)

    left.legend(loc='upper right', ncol=1, fontsize=12)

    x_labels = ["100 million", "200 million", "400 million"]
    size_colors = ['darkgreen', 'steelblue', 'slateblue']
    # slabstore reboot time
    right_top = fig.add_subplot(gs[0, 2])
    width, interval, offset = 0.23, 0.02, 0
    color_id = 0
    xticks = np.arange(len(restart_nums))
    for kv_size in restart_sizes:
        key_size, val_size = kv_size
        real_time, user_time, sys_time = [], [], []
        for rec_num in restart_nums:
            log_name = ("ycsb-restart-" + str(rec_num) + "-" + str(key_size) + "-" + str(
                val_size) + "-" + "reboot" + ".log")
            log_path = os.path.join(log_dir, log_name)
            with open(log_path) as log:
                result = log.read()
                if not result: exit("unknown error, " + log_name)
                match = re.search(r"reboot elapsed real time:\s*(\d+)\s*microseconds", result)
                if not match: exit("unknown error, match failed")
                real_time.append(float(match.group(1)) / 1000000)
                match = re.search(r"reboot total user CPU time:\s*(\d+)\s*microseconds", result)
                if not match: exit("unknown error, match failed")
                user_time.append(float(match.group(1)))
                match = re.search(r"reboot total sys CPU time:\s*(\d+)\s*microseconds", result)
                if not match: exit("unknown error, match failed")
                sys_time.append(float(match.group(1)))
        total_time = [user_time[idx] + sys_time[idx] for idx in range(len(restart_nums))]
        real_user_time = [real_time[idx] * user_time[idx] / total_time[idx] for idx in range(len(restart_nums))]
        real_sys_time = [real_time[idx] * sys_time[idx] / total_time[idx] for idx in range(len(restart_nums))]

        right_top.bar(xticks + offset, real_sys_time, width=width, label="kernel time", color='firebrick')
        right_top.bar(xticks + offset, real_user_time, width=width, bottom=real_sys_time, hatch='x', alpha=1,
                      label=str(kv_size) + " user time", color=size_colors[color_id])
        offset += width + interval
        color_id += 1

    right_top.set_xticks(xticks + width, x_labels, fontsize=12)
    right_top.set_ylabel('Fast reboot time (sec)', fontsize=12)
    legends = [Patch(facecolor=size_colors[idx], edgecolor='black', hatch='x', alpha=1,
                     label=str(restart_sizes[idx]) + " user time")
               for idx in range(len(restart_sizes))]
    legends.append(Patch(facecolor='firebrick', edgecolor='black', alpha=1, label="sys (kernel) time"))
    right_top.legend(handles=legends, loc='upper left', ncol=1, fontsize=11)

    # slabstore recover time
    right_bottom = fig.add_subplot(gs[2, 2])
    width, interval, offset = 0.23, 0.02, 0
    color_id = 0
    xticks = np.arange(len(restart_nums))
    for kv_size in restart_sizes:
        key_size, val_size = kv_size
        real_time, user_time, sys_time = [], [], []
        for rec_num in restart_nums:
            log_name = ("ycsb-restart-" + str(rec_num) + "-" + str(key_size) + "-" + str(
                val_size) + "-" + "recover" + ".log")
            log_path = os.path.join(log_dir, log_name)
            with open(log_path) as log:
                result = log.read()
                if not result: exit("unknown error, " + log_name)
                match = re.search(r"recover elapsed real time:\s*(\d+)\s*microseconds", result)
                if not match: exit("unknown error, match failed")
                real_time.append(float(match.group(1)) / 1000000)
                match = re.search(r"recover total user CPU time:\s*(\d+)\s*microseconds", result)
                if not match: exit("unknown error, match failed")
                user_time.append(float(match.group(1)))
                match = re.search(r"recover total sys CPU time:\s*(\d+)\s*microseconds", result)
                if not match: exit("unknown error, match failed")
                sys_time.append(float(match.group(1)))
        total_time = [user_time[idx] + sys_time[idx] for idx in range(len(restart_nums))]
        real_user_time = [real_time[idx] * user_time[idx] / total_time[idx] for idx in range(len(restart_nums))]
        real_sys_time = [real_time[idx] * sys_time[idx] / total_time[idx] for idx in range(len(restart_nums))]

        right_bottom.bar(xticks + offset, real_sys_time, width=width, label="kernel time", color='firebrick')
        right_bottom.bar(xticks + offset, real_user_time, width=width, bottom=real_sys_time, hatch='x', alpha=1,
                         label=str(kv_size) + " user time", color=size_colors[color_id])
        offset += width + interval
        color_id += 1

    right_bottom.set_xticks(xticks + width, x_labels, fontsize=12)
    right_bottom.set_ylabel('Recovery time (sec)', fontsize=12)
    # right_bottom.set_xlabel('Records number', loc="right", fontsize=12)
    legends = [Patch(facecolor=size_colors[idx], edgecolor='black', hatch='x', alpha=1,
                     label=str(restart_sizes[idx]) + " user time")
               for idx in range(len(restart_sizes))]
    legends.append(Patch(facecolor='firebrick', edgecolor='black', alpha=1, label="sys (kernel) time"))
    right_bottom.legend(handles=legends, loc='upper left', ncol=1, fontsize=11)
    right_bottom.set_title('(b) Fast reboot and recovery time.', x=0.5, y=-0.35, fontsize=12)

    fig.tight_layout()
    fig.savefig("size-and-recover.pdf", bbox_inches='tight')
    fig.show()


def figure_throughput_and_space_over_time():
    stores = ["pmemkv", "BasicSlabStore", "SlabStore", "Plush", "Viper", "Plush-Payload-Compact"]  # , "RocksDB"]
    colors = ['blue', 'steelblue', 'red', 'orange', 'green', 'tomato', 'gray', 'brown']
    markers = ["o", "X", "d", "s", "^", "v", "P", "*"]
    libs = ["pmemkv-var", "basic-slabstore-var", "slabstore-var", "plush-var", "viper-var", "plush-compact-var"]

    nthd = 48
    record_count = 200000000
    run_duration = 240
    pool_size = 484 * 1024 * 1024 * 1024
    key_size, val_size = 8, 32
    # run throughput over time, update only
    for lib in libs:
        input_lib = "./build/test/libpibench-" + lib + ".so"
        pibench = ["./build/test/PiBench", input_lib, "-n", str(record_count), "-r", "0", "-u", "1",
                   "-t", str(nthd), "-m", "time", "--seconds", str(run_duration), "--pool_size", str(pool_size),
                   "--key_size", str(key_size), "--value_size", str(val_size), "--script", str(pm_script)]
        log_name = ('throughput-over-time-' + lib + '-' + str(nthd) + '-' + str(key_size)
                    + '-' + str(val_size) + '-' + str(run_duration) + '.log')
        log_path = os.path.join(log_dir, log_name)
        if not os.path.exists(log_path):
            remove_path("/mnt/pmem0/pibench")
            os.mkdir("/mnt/pmem0/pibench")
            result = subprocess.run(pibench, capture_output=True, text=True).stdout
            with open(log_path, "w") as log:
                log.write(str(pibench) + "\n" + result)
    remove_path("/mnt/pmem0/pibench")

    hundred_million = 100000000
    counts = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 20]

    for lib in libs:
        input_lib = "./build/test/libpibench-" + lib + ".so"
        for count in counts:
            op_count = count * hundred_million
            pibench = ["./build/test/PiBench", input_lib, "-n", str(record_count), "-r", "0", "-u", "1",
                       "-p", str(op_count), "-t", str(nthd), "--pool_size", str(pool_size), "--get_size",
                       "--key_size", str(key_size), "--value_size", str(val_size), "--script", str(pm_script)]
            log_name = ('space-over-operation-' + lib + '-' + str(nthd) + '-' + str(key_size)
                        + '-' + str(val_size) + '-' + str(count) + '.log')
            log_path = os.path.join(log_dir, log_name)
            if not os.path.exists(log_path):
                remove_path("/mnt/pmem0/pibench")
                os.mkdir("/mnt/pmem0/pibench")
                result = subprocess.run(pibench, capture_output=True, text=True).stdout
                pmempool_info = ""
                if lib == "pmemkv-var":
                    pmempool = ['pmempool', 'info', '-s', '/mnt/pmem0/pibench/pmemkv']
                    pmempool_info = subprocess.run(pmempool, capture_output=True, text=True).stdout
                with open(log_path, "w") as log:
                    log.write(str(pibench) + "\n" + result + "\n" + pmempool_info)
    remove_path("/mnt/pmem0/pibench")

    fig, axes = plt.subplots(2, 1, figsize=(10, 5))
    # throughput over time
    xticks = np.arange(run_duration)
    for sid in range(len(stores)):
        log_name = ('throughput-over-time-' + libs[sid] + '-' + str(nthd) + '-' + str(key_size)
                    + '-' + str(val_size) + '-' + str(run_duration) + '.log')
        log_path = os.path.join(log_dir, log_name)
        perf = []
        with open(log_path) as log:
            result = log.read()
            if not result: exit("unknown error, " + log_name)
            res = re.findall(r"^[\s\t]*([\d.]+)", result, re.MULTILINE)
            if len(res) != run_duration: exit("sampling unknown error, " + str(len(res)) + ', ' + log_name)
            for r in res: perf.append(float(r) / 1000000)
        axes[0].plot(xticks[1:], perf[1:], label=stores[sid], marker=markers[sid], color=colors[sid],
                     linewidth=3, markersize=5, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
    axes[0].set_ylabel('Throughput (Mops/s)', fontsize=12)
    axes[0].set_xlabel('Time (sec)', loc="right", fontsize=12)
    axes[0].set_xlim(-1, run_duration + 1)
    axes[0].set_title('(a) Throughput of Write-Only workload over time.', y=-0.35, fontsize=12)

    # space over update operations
    for sid in range(len(stores)):
        space = []
        for count in counts:
            log_name = ('space-over-operation-' + libs[sid] + '-' + str(nthd) + '-' + str(key_size)
                        + '-' + str(val_size) + '-' + str(count) + '.log')
            log_path = os.path.join(log_dir, log_name)
            with open(log_path) as log:
                result = log.read()
                if not result: exit("unknown error, " + log_name)
                if libs[sid] == "pmemkv-var":
                    match = re.search(r"^Total used bytes[\s\t]*:[\s\t]*(\d+)", result, re.MULTILINE)
                    if not match: exit("unknown error, match failed")
                    space.append(float(match.group(1)) / (1024 * 1024 * 1024))
                else:
                    match = re.search(r"^PMem footprint \(bytes\):\s*(\d+)", result, re.MULTILINE)
                    if not match: exit("unknown error, match failed")
                    space.append(float(match.group(1)) / (1024 * 1024 * 1024))
        axes[1].plot(counts, space, label=stores[sid], marker=markers[sid], color=colors[sid],
                     linewidth=3, markersize=10, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
    axes[1].set_ylabel('PMem footprint (GiB)', fontsize=12)
    axes[1].set_xlabel('Hundred million operations', loc="right", fontsize=12)
    axes[1].set_xlim(-0.3, counts[-1] + 0.3)
    axes[1].set_xticks(counts)
    axes[1].set_title('(b) PMem footprint over the number of updates.', y=-0.35, fontsize=12)

    fig.tight_layout()
    lines, labels = fig.axes[-1].get_legend_handles_labels()
    fig.legend(lines, labels, loc='upper center', ncol=len(stores), bbox_to_anchor=(0.5, 1.08), fontsize=11)
    fig.savefig("tpt-and-space-over-time.pdf", bbox_inches='tight')
    fig.show()


def figure_overview():
    store_path = "/mnt/pmem0/ycsb-store"
    store_size = 128
    records_num = 200000000

    # recover and performance overview
    sids = [0, 2, 3, 6]
    for sid in sids:
        remove_path(store_path)
        ycsb_script = ["./build/test/test-ycsb-test", store_path, str(store_size),
                       str(sid), str(48), str(records_num), str(8), str(8), str(0),
                       str(60), str(0), str(0), str(48), str(pm_script), str(0), str(1)]
        log_name = ("ycsb-overview-" + str(sid) + ".log")
        log_path = os.path.join(log_dir, log_name)

        if not os.path.exists(log_path):
            result = subprocess.run(ycsb_script, capture_output=True, text=True).stdout
            with open(log_path, 'w') as log:
                log.write(str(ycsb_script) + "\n" + result + "\n\n")

            ycsb_restart = ["./build/test/test-ycsb-restart", store_path, str(sid)]
            restart_log_name = ("ycsb-overview-restart-" + str(sid) + ".log")
            restart_log_path = os.path.join(log_dir, restart_log_name)
            if not os.path.exists(restart_log_path):
                result = subprocess.run(ycsb_restart, capture_output=True, text=True).stdout
                with open(restart_log_path, 'w') as log:
                    log.write(str(ycsb_restart) + "\n" + result + "\n\n")
    remove_path(store_path)

    # for sid in sids:
    #     remove_path(store_path)
    #     ycsb_script = ["./build/test/test-ycsb-test", store_path, str(store_size),
    #                    str(sid), str(48), str(records_num), str(8), str(8), str(100),
    #                    str(60), str(0), str(0), str(48), str(pm_script), str(0), str(1)]
    #     log_name = ("ycsb-lookup-overview-" + str(sid) + ".log")
    #     log_path = os.path.join(log_dir, log_name)
    #     if not os.path.exists(log_path):
    #         result = subprocess.run(ycsb_script, capture_output=True, text=True).stdout
    #         with open(log_path, 'w') as log:
    #             log.write(str(ycsb_script) + "\n" + result + "\n\n")
    # remove_path(store_path)

    # space overview
    libs = ["pmemkv-var", "slabstore-var", "plush-var", "viper-var"]
    for lib in libs:
        input_lib = "./build/test/libpibench-" + lib + ".so"
        key_size, val_size = 8, 8
        records_num = 200000000
        pool_size = 128 * 1024 * 1024 * 1024
        pibench = ["./build/test/PiBench", input_lib, "-n", str(records_num), "-r", "0", "-u", "1",
                   "-p", str(records_num * 2), "-t", str(48), "--pool_size", str(pool_size), "--get_size",
                   "--key_size", str(key_size), "--value_size", str(val_size), "--script", str(pm_script)]
        log_name = ('space-overview-' + lib + '.log')
        log_path = os.path.join(log_dir, log_name)
        if not os.path.exists(log_path):
            remove_path("/mnt/pmem0/pibench")
            os.mkdir("/mnt/pmem0/pibench")
            result = subprocess.run(pibench, capture_output=True, text=True).stdout
            pmempool_info = ""
            if lib == "pmemkv-var":
                pmempool = ['pmempool', 'info', '-s', '/mnt/pmem0/pibench/pmemkv']
                pmempool_info = subprocess.run(pmempool, capture_output=True, text=True).stdout
            with open(log_path, "w") as log:
                log.write(str(pibench) + "\n" + result + "\n" + pmempool_info)
    remove_path("/mnt/pmem0/pibench")

    stores = ["pmemkv", "SlabStore", "Plush", "Viper"]
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), gridspec_kw={'wspace': 0.2})
    recover_time = []
    for ind in range(0, 4):
        restart_log_name = ("ycsb-overview-restart-" + str(sids[ind]) + ".log")
        restart_log_path = os.path.join(log_dir, restart_log_name)
        with open(restart_log_path) as log:
            result = log.read()
            if not result: exit("unknown error, " + restart_log_name)
            match = re.search(r"YCSB-Restart: Total Recovery time:\s*(\d+)\sms", result)
            if not match: exit("unknown error, match failed")
            recover_time.append(float(match.group(1)) / 1000)

    recax = axes[0]
    recax.plot(range(0, 4), recover_time, label="recover", marker='s', color='black',
               linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
    recax.set_xticks(range(0, 4), stores, fontsize=12)
    recax.set_ylabel('\u25A0 Recovery time (sec)', fontsize=12)

    total_space = []
    for ind in range(0, 4):
        log_name = ('space-overview-' + libs[ind] + '.log')
        log_path = os.path.join(log_dir, log_name)
        with open(log_path) as log:
            result = log.read()
            if not result: exit("unknown error, " + log_name)
            if libs[ind] == "pmemkv-var":
                match = re.search(r"^Total used bytes[\s\t]*:[\s\t]*(\d+)", result, re.MULTILINE)
                if not match: exit("unknown error, match failed")
                total_space.append(float(match.group(1)) / (1024 * 1024 * 1024))
            else:
                match = re.search(r"^PMem footprint \(bytes\):\s*(\d+)", result, re.MULTILINE)
                if not match: exit("unknown error, match failed")
                total_space.append(float(match.group(1)) / (1024 * 1024 * 1024))

    spaceax = axes[0].twinx()
    spaceax.plot(range(0, 4), total_space, label="space", marker='o', color='black',
                 linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
    spaceax.set_ylabel('\u25CF PMem footprint (GiB)', fontsize=12)

    insert_tpt, update_tpt = [], []
    for ind in range(0, 4):
        log_name = "ycsb-overview-" + str(sids[ind]) + ".log"
        log_path = os.path.join(log_dir, log_name)
        with open(log_path) as log:
            result = log.read()
            if not result: exit("unknown error, " + log_name)
            match = re.search(r"run phase.*throughput:\s*([\d.]+)", result)
            if not match: exit("unknown error, match failed")
            update_tpt.append(float(match.group(1)))
            match = re.search(r"load phase.*throughput:\s*([\d.]+)", result)
            if not match: exit("unknown error, match failed")
            insert_tpt.append(float(match.group(1)))
    axes[1].plot(range(0, 4), insert_tpt, label="insert", marker='P', color='black',
                 linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
    axes[1].plot(range(0, 4), update_tpt, label="update", marker='^', color='black',
                 linewidth=3, markersize=12, markeredgewidth=1, markeredgecolor='black', alpha=0.95)
    axes[1].set_xticks(range(0, 4), stores, fontsize=12)
    axes[1].yaxis.tick_right()
    axes[1].yaxis.set_label_position("right")
    axes[1].set_ylabel('Million Operations per Second', fontsize=12)
    axes[1].legend()

    fig.savefig("overview.pdf", bbox_inches='tight')
    fig.show()


if __name__ == "__main__":
    build_project()

    plt.rcParams['axes.linewidth'] = 2
    plt.rcParams['xtick.major.width'] = 2
    plt.rcParams['ytick.major.width'] = 2

    plt.rcParams['pdf.fonttype'] = 42
    plt.rcParams['ps.fonttype'] = 42
    plt.rcParams['font.weight'] = 'medium'

    # # fixed-size records
    # figure_core_ops()
    #
    # # variable-size records
    # figure_scalability(8, 32, False)
    # figure_scalability(32, 200, True)
    #
    # figure_ycsb_insert()
    #
    # figure_ycsb_access(8, 32)
    # figure_ycsb_access(32, 200)
    #
    # figure_size_sensitivity_and_recovery()
    #
    # figure_throughput_and_space_over_time()
    figure_overview()
