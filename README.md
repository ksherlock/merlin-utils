# merlin-utils
utilities for the Merlin 8/16+ assembler

An OMF linker for Merlin 8/16+ REL files.  Why?  ummm....

`merlin-link [-D key=value] [-X] [-C] [-o outfile] files....`

* `-X`: inhibit expressload segment
* `-C`: inhibit super relocation records
* `-D`: define an absolute label.  value can use `$`, `0x`, or `%` prefix.
* `-o`: specify output file. default is `gs.out`

### Building

```
git submodule init
git submodule update
make
```

Requires a c++17 compiler. (ie, ubuntu bionic or OS X 10.13).  Also assumes a little-endian environment :/

