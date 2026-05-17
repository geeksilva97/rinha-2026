

set dirname [file normalize [file dirname [info script]]]
source [file join $dirname vec1_common.tcl]
db close

rename source real_source
proc source {fname} {
  uplevel [list real_source $fname]
  if {[file tail $fname]=="permutations.test"} {
    test_suite "veryquick" -prefix "" -description {
    } -files [
      glob [file join $::dirname *.test]
    ]
  }
}

set sv $argv0

set argv [list]
set argv0 [file join $testdir testrunner.tcl]
source $argv0

set argv0 $sv



