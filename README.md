# merlin-utils
utilities for the Merlin 8/16+ assembler

An OMF linker for Merlin 8/16+ REL files.  Why?  ummm....

`merlin-link [-D key=value] [-X] [-C] [-o outfile] files....`

* `-X`: inhibit expressload segment
* `-C`: inhibit super relocation records
* `-D`: define an absolute label.  value can use `$`, `0x`, or `%` prefix.
* `-S`: treat input file as a linker command file
* `-o`: specify output file. default is `omf.out`
* `-v`: be verbose

If there is one input file and it ends with `.S` (case insensitive), it is treated as a linker command file.

### Linker Command File

The following opcodes are supported:

`END`,`DAT`, `PFX`, `TYP`, `ADR`, `ORG`, `KND`, `ALI`, `DS`, `LKV`, `VER`, `LNK`, `IMP`, `SAV`, `KBD`,
`POS`, `LEN`, `EQ`, `EQU`, `=`, `GEQ`, `EXT`, `DO`, `ELS`, `FIN`

* `VER`: only allows OMF version 2.
* `LKV`: 0 (binary), 1 (single segment OMF), 2 (multi segment OMF)
* `IMP`: (qasm) - import a binary file. Entry name is the file name with non alphanumerics converted to `_`
* `KBD`: won't prompt if label was previously defined (via `-D` for example)


No operand math is allowed at this time.




### Current Status

Usable, if you can find a use for it.  Linking Marinetti (after assembling with Merlin 16+) generates an equivalent
OMF file.  The file is not identical due to differences in relocation records but OMF Analyzer COMPARE considers them identical.

### Building

```
git submodule init
git submodule update
make
```

Requires a c++17 compiler. (ie, ubuntu bionic or OS X 10.13).  Also assumes a little-endian environment :/
