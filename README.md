# Parallel Sort

A C-based parallel sorting utility for fixed-size binary records, implemented using POSIX threads.

The project demonstrates multithreaded sorting, binary file processing, chunk-based parallelism, and k-way merging.

## Build Instructions

This project can be built on Unix/Linux/macOS environments.

For Ubuntu/Linux, install build tools:

```bash
sudo apt update
sudo apt install build-essential
```

For macOS, install command line tools if required:

```bash
xcode-select --install
```

Build the project:

```bash
make
```

The build creates the following executables:

```text
psort
gen_records
check_sorted
```

## Running the Program

Generate a test input file:

```bash
./gen_records 100000 input.bin 42
```

Run the parallel sorter:

```bash
./psort input.bin output.bin
```

Verify the sorted output:

```bash
./check_sorted input.bin output.bin
```

Expected output:

```text
OK: output.bin is sorted and matches input.bin (100000 records)
```

The full test can also be run using:

```bash
make test
```

## Configuring Threads

By default, `psort` uses the available CPU cores.

To manually specify the number of worker threads:

```bash
PSORT_THREADS=4 ./psort input.bin output.bin
```

## Input Format

The input file consists of fixed-size binary records.

```text
100 bytes total
first 4 bytes      -> integer key used for sorting
remaining 96 bytes -> payload/data
```

Records are sorted based on the first 4-byte key.

## Implementation Overview

```text
read binary records
  -> divide records into chunks
  -> sort chunks in parallel using pthreads
  -> merge sorted chunks using k-way merge
  -> write sorted records to output file
  -> verify output correctness
```

## Important Files

`psort.c`  
Main implementation of the parallel sorter.

`gen_records.c`  
Generates random fixed-size binary records for testing.

`check_sorted.c`  
Verifies that the output file is sorted and contains the same records as the input file.

`Makefile`  
Provides build, test, and clean commands.

## Clean Up

Remove compiled executables:

```bash
make clean
```

Remove generated binary files:

```bash
rm -f *.bin
```

## Concepts Covered

```text
C
POSIX Threads
Multithreading
Parallel Programming
Binary File I/O
K-way Merge
Makefile
Systems Programming
```

## Acknowledgement

This project is inspired by the OSTEP `concurrency-sort` project.

Original project link:  
https://github.com/remzi-arpacidusseau/ostep-projects/tree/master/concurrency-sort
