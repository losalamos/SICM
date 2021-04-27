#!/usr/bin/env bash

set -e

DIR="$(dirname ${BASH_SOURCE[0]})"

if [[ "$#" -lt "5" ]]
then
    echo "Syntax: $0 size_file count reps out [NUMA nodes ...]" 2>&1
    exit 1
fi

size_file="$1"
count="$2"
reps="$3"
out="$4"
shift
shift
shift
shift
numa_nodes="$@"

for alloc in posix_memalign mmap numa sicm
do
    echo "${alloc}"
    (
        echo "$# ${numa_nodes}"
        for dst in ${numa_nodes}
        do
            echo -n "${dst}"
            for src in ${numa_nodes}
            do
                times=()
                for rep in $(seq ${reps})
                do
                    times+=($(${DIR}/move_perf "${alloc}" "$(nproc --all)" "${count}" "${size_file}" "${src}" "${dst}"))
                done
                avg=$(python3 -c "times=[float(x) for x in '$(echo ${times[@]})'.split()]; print(sum(times) / len(times))")
                echo -n " ${avg}"
            done
            echo
        done
    ) | tee "${out}.${alloc}"
done
