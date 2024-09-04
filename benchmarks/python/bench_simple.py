import os

os.environ['OMP_NUM_THREADS'] = '1'
os.environ['GOTO_NUM_THREADS'] = '1'
os.environ['MKL_NUM_THREADS'] = '1'

import python.dsc as dsc
import numpy as np
import time
from utils import WARMUP, BENCH_STEPS, random_nd
import random
import matplotlib.pyplot as plt
from tabulate import tabulate

DTYPES = [np.float32, np.float64, np.complex64, np.complex128]


def plot(np_latency, dsc_latency, unit: str):
    operations = list(np_latency.keys())
    x = np.arange(len(operations))
    width = 0.35

    fig, ax = plt.subplots(figsize=(12, 6))
    rects1 = ax.bar(x - width / 2, np_latency.values(), width, label='NumPy')
    rects2 = ax.bar(x + width / 2, dsc_latency.values(), width, label='DSC')

    ax.set_ylabel(f'Latency ({unit})')
    ax.set_title('Latency Comparison when X = [60 x 60000]')
    ax.set_xticks(x)
    ax.set_xticklabels(operations, rotation=45, ha='right')
    ax.legend()

    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)

    fig.tight_layout()

    plt.show()


def draw_table(np_latency, dsc_latency, unit: str):
    table_data = []
    for op in np_latency.keys():
        table_data.append([op, np_latency[op], dsc_latency[op], dsc_latency[op] / np_latency[op]])

    headers = ['Operation', f'NumPy Latency ({unit})', f'DSC Latency ({unit})', 'Ratio (DSC/NumPy)']
    table = tabulate(table_data, headers=headers, floatfmt=".2f", tablefmt="grid")
    print(table)


def bench(op, *args, out=None) -> float:
    def _call_op():
        if out is not None:
            op(*args, out=out)
        else:
            op(*args)

    for _ in range(WARMUP):
        _call_op()

    op_time = float('+inf')
    for _ in range(BENCH_STEPS):
        start_ = time.perf_counter()
        _call_op()
        this_time = time.perf_counter() - start_
        op_time = this_time if this_time < op_time else op_time
    return op_time


# To run from CLI
# export PYTHONPATH=/home/lowl/Scrivania/projects/dspcraft/dsc:$PYTHONPATH
def bench_binary(show_plot: bool = True):
    dsc.init(1024 * 1024 * 1024 * 2)
    ops = {
        'add': (np.add, dsc.add),
        'addc': (np.add, dsc.add),
        'sub': (np.subtract, dsc.sub),
        'subc': (np.subtract, dsc.sub),
        'mul': (np.multiply, dsc.mul),
        'mulc': (np.multiply, dsc.mul),
        'true_div': (np.true_divide, dsc.true_div),
        'true_divc': (np.true_divide, dsc.true_div)
    }
    np_latency = {}
    dsc_latency = {}
    for op_name in ops.keys():
        np_op, dsc_op = ops[op_name]
        is_scalar = op_name.endswith('c')
        for dtype in DTYPES:
            shape = [60, 60_000]
            a = random_nd(shape, dtype)
            if is_scalar:
                if dtype == np.complex64 or dtype == np.complex128:
                    b = complex(random.random(), random.random())
                else:
                    b = random.random()
            else:
                b = random_nd(shape, dtype)
            out = np.empty_like(a)

            a_dsc = dsc.from_numpy(a)
            if is_scalar:
                b_dsc = b
            else:
                b_dsc = dsc.from_numpy(b)
            out_dsc = dsc.from_numpy(out)

            np_latency[f'{op_name}_{dtype.__name__}'] = bench(np_op, a, b, out=out) * 1e3
            dsc_latency[f'{op_name}_{dtype.__name__}'] = bench(dsc_op, a_dsc, b_dsc, out=out_dsc) * 1e3

            dsc.clear()

    draw_table(np_latency, dsc_latency, 'ms')

    if show_plot:
        plot(np_latency, dsc_latency, 'ms')


def bench_unary(show_plot: bool = True):
    dsc.init(1024 * 1024 * 1024 * 2)
    ops = {
        'sin': (np.sin, dsc.sin),
        'cos': (np.cos, dsc.cos),
        'logn': (np.log, dsc.logn),
        'log2': (np.log2, dsc.log2),
        'log10': (np.log10, dsc.log10),
        'exp': (np.exp, dsc.exp),
        'sqrt': (np.sqrt, dsc.sqrt),
        'absolute': (np.absolute, dsc.absolute),
        'conj': (np.conj, dsc.conj),
    }
    np_latency = {}
    dsc_latency = {}

    for op_name in ops.keys():
        np_op, dsc_op = ops[op_name]
        for dtype in DTYPES:
            shape = [60, 60_000]
            a = random_nd(shape, dtype)
            out_dtype = a.dtype
            if op_name == 'abs':
                # These functions always return a real number
                if dtype == np.complex64:
                    out_dtype = np.float32
                elif dtype == np.complex128:
                    out_dtype = np.float64
                else:
                    out_dtype = dtype

            out = np.empty_like(a, dtype=out_dtype)
            a_dsc = dsc.from_numpy(a)
            out_dsc = dsc.from_numpy(out)

            np_latency[f'{op_name}_{dtype.__name__}'] = bench(np_op, a, out=out) * 1e3
            dsc_latency[f'{op_name}_{dtype.__name__}'] = bench(dsc_op, a_dsc, out=out_dsc) * 1e3

            dsc.clear()

    draw_table(np_latency, dsc_latency, 'ms')

    if show_plot:
        plot(np_latency, dsc_latency, 'ms')


if __name__ == '__main__':
    bench_unary(True)
    # bench_binary(True)
