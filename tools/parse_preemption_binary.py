# Copyright (c) 2025 Advanced Micro Devices, Inc
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import argparse
import os
from pathlib import Path
import zlib
import fnmatch
import multiprocessing as mp
import time

def get_var_name(file_path):
    file_name = file_path.name
    dir_name = file_path.parent.name
    var_name = dir_name + '_' +  file_name.split('.')[0]
    return var_name

def bin_to_cpp(file_path):
    variable_name = get_var_name(file_path)
    decompressed_size = 0

    with open(file_path, 'rb') as file:
        binary_data = file.read()
        decompressed_size = len(binary_data)
        compressed_data = zlib.compress(binary_data)
        hex_data = ''.join(f'\\x{byte:02x}' for byte in compressed_data)
        # Windows has a 16380-char string limit. Break up long strings with ""
        max_string_len = 16380
        hex_data = "\"\"".join(hex_data[i:i+max_string_len] for i in range(0, len(hex_data), max_string_len))

    return file_path, variable_name, decompressed_size, len(compressed_data), hex_data

def write_bin_to_file(pm_hdrf, pm_srcf, data):
    file_path, variable_name, decompressed_size, len_compressed_data, hex_data = data
    with open(pm_hdrf, "a") as hdr:
        hdr.write(f'const std::string& get{variable_name.capitalize()}();\n')

    with open(pm_srcf, "a") as src:
        src.write(f'static char {variable_name}[] = "{hex_data}";\n')
        # write a function
        src.write(f"static std::string initialize_{variable_name}()" + "{\n")
        src.write("std::string ret = \"\";\n")
        src.write("uLongf ret_size = 0;\n")
        src.write(f"ret.resize({decompressed_size});\n")
        src.write("z_stream infstream = {};\n")
        src.write("infstream.zalloc = Z_NULL;\n")
        src.write("infstream.zfree = Z_NULL;\n")
        src.write("infstream.opaque = Z_NULL;\n")
        src.write(f"infstream.avail_in = {len_compressed_data};\n")
        src.write(f"infstream.next_in = reinterpret_cast<Bytef*>({variable_name});\n")
        src.write(f"infstream.avail_out = {decompressed_size};\n")
        src.write(f"infstream.next_out = reinterpret_cast<Bytef*>(&ret[0]);\n")
        src.write("inflateInit(&infstream);\n")
        src.write("inflate(&infstream, Z_NO_FLUSH);\n")
        src.write("inflateEnd(&infstream);\n")
        src.write("return ret;\n")
        src.write("}\n")

        src.write(f'const std::string& get{variable_name.capitalize()}() {{\n')
        src.write(f'static const std::string {variable_name}_str = initialize_{variable_name}();\n')
        src.write(f'return {variable_name}_str;\n')
        src.write('}\n')

def write_preemption_src(out_dir, pm_data, quiet, pm_hdr):
    # pm_list = [i[0] for i in pm_data]
    pm_list = pm_data
    with open(Path(out_dir) / Path('preemption.cpp'), 'w') as pm_src:
        pm_src.write('#include "pm_container.hpp"\n')
        pm_src.write(f'#include "{pm_hdr}"\n')
        pm_src.write('Preemption::Preemption(){{}};\n')
        pm_src.write('const std::string& Preemption::get_pm_str(const std::string& name) {\n')
        pm_src.write('\tstd::string op_name;\n')
        pm_src.write('\top_name = name;\n')
        for pm_file in pm_list:
            var_name = get_var_name(pm_file)
            func_name = get_var_name(pm_file).capitalize()
            pm_src.write(f'\tif (op_name == "{var_name}")\n')
            pm_src.write(f'\t\treturn get{func_name}();\n')
        pm_src.write('\telse\n')
        pm_src.write('\t\tthrow std::runtime_error("Invalid preemption binary string: " + op_name);\n')
        pm_src.write('}\n')

        filtering_words = ["param", "ddr_buffer_info"] #getting rid of repetition
        filtering_fun = lambda x: all([not substr in get_var_name(x) for substr in filtering_words])
        pm_list_filtered = list(filter(filtering_fun, pm_list))
        pm_src.write('std::vector<std::string> Preemption::match_prefix(const std::string& prefix) {\n')
        pm_src.write(f'\tconst static std::array<std::string, {len(pm_list_filtered)}> all_pm_strings =\u007b\n')
        for i, pm_file in enumerate(pm_list_filtered):
            var_name = get_var_name(pm_file)
            if i == len(pm_list) - 1 :
                pm_src.write(f'\t"{var_name}"\n')
            else:
                pm_src.write(f'\t"{var_name}",\n')
        pm_src.write('\t};\n')
        pm_src.write('\tstd::vector<std::string> matched_strings;\n')
        pm_src.write('\tstd::copy_if(all_pm_strings.begin(), all_pm_strings.end(), std::back_inserter(matched_strings),\n')
        pm_src.write('\t\t[prefix](const std::string& pm_var_name){ return pm_var_name.rfind(prefix, 0) == 0; });\n')
        pm_src.write('\treturn matched_strings;\n')
        pm_src.write('}\n')
        if not quiet:
            print('preemption.cpp is generated')

def get_bin_file(large_pm_ops):
    for root, dirs, files in os.walk(tr_path):
        for filename in fnmatch.filter(files, '*.bin'):
            file_path = Path(root) / filename
            fname = str(file_path)
            fname = fname.replace("\\", "/")
            if any(map(lambda large_op: large_op in fname, large_pm_ops)):
                continue
            yield file_path
        for filename in fnmatch.filter(files, '*.json'):
            file_path = Path(root) / filename
            fname = str(file_path)
            fname = fname.replace("\\", "/")
            if any(map(lambda large_op: large_op in fname, large_pm_ops)):
                continue
            yield file_path

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="", type=Path)
    parser.add_argument("--out-dir", required=False, type=Path)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--disable-large-pm-ops", action="store_true")
    args = parser.parse_args()

    large_pm_ops = []
    if args.disable_large_pm_ops:
        large_pm_ops = ["/mladfmatmulbias/", "/elwmul/"]

    root_dir = args.root or Path(os.environ.get('DD_ROOT'))
    out_dir = args.out_dir

    tr_path = root_dir / Path("xclbin/stx/aie_elf_ctrl_pkt/")

    pm_hdr_fname = "all_pm_pkg.hpp"
    pm_src_fname = "all_pm_pkg.cpp"

    start = time.time()
    if args.list:
        print("preemption.cpp")
        print(pm_src_fname)
    else:
        if not os.path.exists(out_dir):
            os.mkdir(out_dir)
        pm_list=[]
        pm_hdrf = Path(out_dir) / Path(pm_hdr_fname)
        pm_srcf = Path(out_dir) / Path(pm_src_fname)
        with open(pm_srcf, "w") as src:
            src.write('#include <string>\n')
            src.write('#include <zlib.h>\n')
            src.write(f'#include "{pm_hdr_fname}"\n')

        with open(pm_hdrf, "w") as hdr:
            hdr.write('#pragma once\n')
            hdr.write('#include <string>\n')
            hdr.write('#include <tuple>\n')

        # Process the files in parallel
        # NOTE: pool.imap_unordered can improve performance further, but not tested yet
        with mp.Pool(processes=8) as pool:
            files_iter = get_bin_file(large_pm_ops)
            pm_list_iter = pool.imap(bin_to_cpp, files_iter, chunksize=4)
            pm_list = []
            for data in pm_list_iter:
                if(len(data) == 0): continue
                write_bin_to_file(pm_hdrf, pm_srcf, data)
                pm_list.append(data[0])

        write_preemption_src(out_dir, pm_list, args.quiet, pm_hdr_fname)
        if not args.quiet:
            print(f"Header generated: {pm_hdr_fname}")
            print(f"Source generated: {pm_src_fname}")
    end = time.time()

    if(not args.quiet):
        print("Time elapsed (s) : ", end-start)
