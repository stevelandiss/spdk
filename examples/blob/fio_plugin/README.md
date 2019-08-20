# BLOBSTORE FIO backend engine

This requires the spdk source available at https://github.com/spdk/spdk

## TO BUILD
You will be building this plugin under the SPDK source.

### STEPS
1. Copy this fio_plugin directory under the `spdk/examples/blob` directory of the SPDK source
2. Edit the following makefile from the SPDK source `spdk/examples/blob/Makefile` and add the `fio_plugin` targets section `DIRS-y`.  It should look like this: `DIRS-y += hello_world cli fio_plugin`.
3. Build spdk by running `make`

## TO RUN
Edit the `bdev.conf.in` file and point AIO at the correct block device on your system. 

Next, in the `spdk/examples/blob/fio_plugin` directory, run FIO as:
```
LD_PRELOAD=./fio_plugin fio job.fio
```
