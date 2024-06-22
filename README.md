# mt-redis

A multi-threaded Redis fork with [Read-Copy-Update](https://liburcu.org/) (RCU) support to achieve high performance.

## Features
* Implement Master-Worker Pattern, allowing key-value store to perform simultaneous processing across multiple threads.
* Use an event-loop per thread I/O model.
* Support for scheduling request operations between threads.
* RCU can support lock-free sharing between 1 writer thread and multiple reader threads to boost read operation performance.
* Can achieve over 1 million Ops/sec powered by an ordinary server.

## Prerequisites

To build mt-redis from source, install the build-essential meta-package from the Ubuntu repositories.
```shell
$ sudo apt install build-essential autoconf clang-format
```

## Building mt-redis

Compile using default setting:
```shell
$ make
```

Selecting a non-default memory allocator when building mt-redis is done by setting the MALLOC environment variable.

To force compiling against libc malloc, use:
```shell
$ make MALLOC=libc
```

To force compiling against jemalloc, use:
```shell
$ make MALLOC=jemalloc
```

## Problems with dependencies or cached build options

When you update the source code with `git pull` or when code inside the dependencies tree is modified in any other way, make sure to use the following command in order to really clean everything and rebuild from scratch:
```shell
$ make distclean
```
This will clean: jemalloc, lua, hiredis, linenoise and other dependencies.

Also if you force certain build options like 32bit target, no C compiler optimizations (for debugging purposes), and other similar build time options, those options are cached indefinitely until you issue a `make distclean` command.

## Running mt-redis

To run mt-redis with the default configuration, just type:
```shell
cd src && ./redis-server
```

If you want to provide your redis.conf, you have to run it using an additional parameter (the path of the configuration file):
```shell
cd src && ./redis-server /path/to/mt-redis.conf
```

## Playing with mt-redis

You can use redis-cli to play with mt-redis. Start a mt-redis-server instance, then in another terminal try the following:
```shell
$ cd src
$ ./redis-cli
127.0.0.1:6379> ping
PONG
127.0.0.1:6379> 
```

## Performance

The following benchmarks show the performance of mt-redis and Valkey using the [memtier\_benchmark](https://github.com/RedisLabs/memtier_benchmark) tool.
These benchmarks were produced on Ubuntu Linux 22.04-LTS with an AMD Ryzen 7 4800HS processor.

### Valkey 7.2.5

Launch the server:
```shell
valkey-server --appendonly no --save ""
```

Run benchmark:
```shell
memtier_benchmark --hide-histogram -p 6379
```

Reference results:
```
============================================================================================================================
Type         Ops/sec     Hits/sec   Misses/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec 
----------------------------------------------------------------------------------------------------------------------------
Sets         7154.42          ---          ---         2.59152         2.35100         4.79900        17.40700       551.02 
Gets        71465.60         0.00     71465.60         2.54465         2.35100         4.70300        15.23100      2783.89 
Waits           0.00          ---          ---             ---             ---             ---             ---          --- 
Totals      78620.03         0.00     71465.60         2.54891         2.35100         4.70300        15.48700      3334.91
```

### mt-redis

Launch the server:
```shell
redis-server --appendonly no --save ""
```

Run benchmark:
```shell
memtier_benchmark --hide-histogram -p 6379
```

Reference results:
```
============================================================================================================================
Type         Ops/sec     Hits/sec   Misses/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec 
----------------------------------------------------------------------------------------------------------------------------
Sets        38465.04          ---          ---         4.11819         2.28700        23.80700        30.71900      2962.48 
Gets       384227.73         0.00    384227.73         0.52990         0.27900        16.31900        24.31900     14967.33 
Waits           0.00          ---          ---             ---             ---             ---             ---          --- 
Totals     422692.77         0.00    384227.73         0.85643         0.28700        18.04700        26.36700     17929.81
```

## Programming style

Run the following command to format the code:
```shell
$ clang-format -i src/*.[ch]
```

