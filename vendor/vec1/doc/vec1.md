
Vec1 Vector Extension
=====================

Contents:
========
<ul type=none>
  <li> 1. <a href=#overview>Overview</a>
  <li> 2. <a href=#building>Building The Extension</a>
  <li> 3. <a href=#usage>Usage</a>
  <li> 4. <a href=#roadmap>Roadmap</a>
</ul>

<a name=overview></a>
1.\ Overview
========

Vec1 is an SQLite extension that provides approximate nearest-neighbor (ANN)
vector search using SQLite's <a href=https://www.sqlite.org/vtab.html>virtual
table</a> interface.  Euclidean (L2) and cosine distances are supported.  Vec1
is implemented in portable C and has no external dependencies. It uses 
<a href="https://en.wikipedia.org/wiki/Advanced_Vector_Extensions">AVX2</a>
on x86 and 
<a href="https://en.wikipedia.org/wiki/ARM_architecture_family#Advanced_SIMD_(NEON)">NEON</a>
on ARM.

Vec1 uses IVFADC (Inverted File with Asymmetric Distance Computation) with OPQ
(Optimized Product Quantization).

Tests on publicly available datasets are <a href=vec1test.md>available here</a>.

<a name=building></a>
2.\ Building the Extension
======================

The extension is implemented in a 
<a href=../../../file?name=vec1.c>single C file, "vec1.c"</a>. It may be
compiled in the same way as other 
<a href=https://sqlite.org/loadext.html#compiling_a_loadable_extension>SQLite extensions</a>.
For best performance, compile with <a href=https://en.wikipedia.org/wiki/Single_instruction,_multiple_data>SIMD</a>
support and aggressive compiler optimizations.

For example, on Linux or macOS x86-64 with gcc or clang:

```
    cc -g -O3 -DNDEBUG -mavx2 -mfma vec1.c -shared -fPIC -o vec1.so
```

Or with MSVC on x86-64:

```
    cl /Zi /O2 /DNDEBUG /arch:AVX2 vec1.c -link -dll -out:vec1.dll
```

No special switches are required to enable NEON on ARM. The compiler
should still be passed -O3 or the equivalent to enable loop-unrolling and other
aggressive optimizations.

Binaries compiled with SIMD instructions enabled on x86-64 platforms as shown
above will not work on systems that lack them. A method for building vec1
to support multiple x86-64 architectures is found in
<a href=../../../file?name=Makefile&ci=tip>this Makefile</a> (target
"vec1multi.so").

<a name=usage></a>
3.\ Usage
=====

A <a href=vec1intro.md>user manual</a> and 
<a href=vec1ref.md>reference docs</a> are available. Also a video 
<a href=https://www.youtube.com/watch?v=sWKhPD4eIvE>
"Vector Queries with SQLite Vec1"</a>.

<a name=roadmap></a>
4.\ Roadmap
=======

No further features are required before first release. But:

  *  Testing is insufficient.

<hr>

Other things to be added and/or investigated following first release:

  *  Almost all paths require optimization.

  *  Optimization of "SELECT count(\*) FROM vec1tbl".

  *  Support for some sort of bit-encoding. RaBitQ?

  *  Add support for SIMD on wasm.

  *  Support vectors constructed of elements other than 32-bit IEEE floats -
     e.g. 8-bit or 32-bit integers, or 16-bit floats.

  *  Support for partition keys.

  *  Add an option for a modern graph-based index as an alternative to IVFADC.
     HNSW? DiskANN? Some variant? Or, better, something that preserves the
     advantages of IVFADC without the training requirement. If RaBitQ or
     TurboQuant or something can be used to quantize vectors without training,
     then perhaps there is also a way to do coarse quant without training
     as well.

  *  Add the ability to open and use databases created on platforms that use
     different byte orders for floating point values.

  *  Implement dot-product as a distance metric for search.
  
  *  Allow queries to use multiple threads.




