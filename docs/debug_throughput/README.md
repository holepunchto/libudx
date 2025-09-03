# debugging throughput

to assist in debugging throughput libudx may be built with `-DUDX_DEBUG_THROUGHPUT`:

```
bare-make generate --debug -D CMAKE_C_FLAGS='-DUDX_DEBUG_THROUGHPUT'
bare-make build
bare-make install # if you need the libraries in ./prebuilds
```

This will create a `stream.%d.dat` for each stream, where %d is substituted with the stream's local id. The file is formatted as newline separated
records of the format `<timestamp_ms> <record-type> [<args> ...]`. The main record type is `tp` (throughput) and it looks like `297040999 tp 105503 105092`,
where the first arg is the first sequence number and the second arg is the current remote_acked number. Other records may simply be `<timestamp> <event>` to
record a specific event (e.g. a state transition, such as from the startup phase to the bandwidth probe phase. The `tp` records and other records can be used
to generate sequence-number "Stevens" graphs annotated with arbitrary events with the provided 'stevens.py' script:

```
./stevens.py stream.1.dat
```

Any lines in the file that start with the `#` character are considered comments and are skipped.

