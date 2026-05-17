

Vec1 Release Testing
====================

1. Run "vibe\_test.tcl release" on one NEON and one AVX2 platform. Check 
   the output for regressions in core functions.

2. Run the "runtests.tcl" script once on each of (a) Linux, (b) Windows 
   and (c) OSX. Check there are no errors.

3. Build vec1.so for coverage testing with VEC1\_THREADS=1. Then check
   that runtests.tcl gives full coverage.



