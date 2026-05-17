

if {[info exists ::tester_tcl_has_run]==0} {
  if {[info exists env(SQLITE_TEST_DIR)]==0} {
    puts stderr "define SQLITE_TEST_DIR to point to test/ dir of SQLite tree"
    exit 1
  }

  set dir [file dirname [file dirname [file normalize [info script]]]]
  set vec1_shlib [file join $dir vec1]

  set testdir $env(SQLITE_TEST_DIR)
  source $testdir/tester.tcl
}

proc vec1_load_only {db} {
  global vec1_shlib
  $db enable_load_extension 1
  $db eval { SELECT load_extension($vec1_shlib) }
  $db enable_load_extension 0
}

proc vec1_load {db} {
  if {[catch {db one {SELECT vec1_info()}}]} {
    global vec1_shlib
    $db enable_load_extension 1
    $db eval { SELECT load_extension($vec1_shlib) }
    $db enable_load_extension 0
  }
  $db func random_vector random_vector
  $db func random_vector2 random_vector2
}

proc random_vector {n} {
  set v [list]
  for {set i 0} {$i < $n} {incr i} {
    lappend v [expr {(rand() * 2.0) - 1.0}]
  }
  binary format f$n $v
}

proc random_vector2 {n} {
  set v [list]
  for {set i 0} {$i < $n} {incr i} {
    lappend v [expr {abs(rand() * 10.0)}]
  }
  binary format f$n $v
}

proc model2tbl {nm vec1tbl} {
  set model [db one "SELECT val FROM ${vec1tbl}_model WHERE id=1"]

  binary scan $model IIIIII iVersion flags nElem nCodebook nBucket eDistance

  set nBytePerVector [expr $nElem*4]
  set iOff [expr 4 * (6 + ($nCodebook>0 ? $nElem*256 : 0))]

  db eval "CREATE TABLE $nm (ibucket INTEGER PRIMARY KEY, coarse BLOB)"
  for {set iBucket 0} {$iBucket<$nBucket} {incr iBucket} {
    set vec [string range $model $iOff [expr $iOff+$nBytePerVector-1]]
    db eval "INSERT INTO $nm VALUES(\$iBucket, \$vec)"
    incr iOff $nBytePerVector
  }
}


