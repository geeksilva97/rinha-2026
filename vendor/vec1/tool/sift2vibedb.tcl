

if {[llength $argv]<2} {
  puts stderr "usage: $argv0 <dirname> <db>"
  exit 1
}

set dirname [lindex $argv 0]
sqlite3 db [lindex $argv 1]
db eval {
  BEGIN;

  DROP TABLE IF EXISTS meta;
  DROP TABLE IF EXISTS __meta__;
  DROP TABLE IF EXISTS neighbors;
  DROP TABLE IF EXISTS test;
  DROP TABLE IF EXISTS train;
  DROP TABLE IF EXISTS learn;
  DROP TABLE IF EXISTS nearest;

  CREATE TABLE meta(field TEXT, value ANY);
  CREATE TABLE __meta__ (
    table_name TEXT PRIMARY KEY, dtype TEXT, shape TEXT
  );
  CREATE TABLE neighbors(id INTEGER PRIMARY KEY, vec BLOB NOT NULL);
  CREATE TABLE test(id INTEGER PRIMARY KEY, vec BLOB NOT NULL);
  CREATE TABLE train(id INTEGER PRIMARY KEY, vec BLOB NOT NULL);
  CREATE TABLE learn(id INTEGER PRIMARY KEY, vec BLOB NOT NULL);

  INSERT INTO __meta__(table_name, dtype) VALUES('neighbors','int64');
  INSERT INTO __meta__(table_name, dtype) VALUES('test','float32');
  INSERT INTO __meta__(table_name, dtype) VALUES('train','float32');
  INSERT INTO meta VALUES('distance', 'euclidean');
}

set lFile {
  sift_base.fvecs            train
  sift_groundtruth.ivecs     neighbors
  sift_learn.fvecs           learn
  sift_query.fvecs           test
}
if {[file exists [file join $dirname gist_base.fvecs]]} {
  set lFile {
    gist_base.fvecs            train
    gist_groundtruth.ivecs     neighbors
    gist_learn.fvecs           learn
    gist_query.fvecs           test
  }
}

foreach {f t} $lFile {
  set fname [file join $dirname $f]
  if {[file exists $fname]==0} {
    puts stderr "missing required file: $fname"
    exit 1
  }
}

foreach {f t} $lFile {
  set fname [file join $dirname $f]

  set fd [open $fname]
  fconfigure $fd -translation binary -encoding binary

  # Read in first 4 byte integer - the number of fields in each vector.
  binary scan [read $fd 4] i nField

  set nByte [expr $nField * 4]
  set nVec [expr [file size $fname] / (4+$nByte)]

puts "importing $nVec vectors from $fname -> $t.."
  for {set ii 0} {$ii < $nVec} {incr ii} {
    seek $fd [expr {($nByte+4)*$ii + 4}]
    set blob [read $fd $nByte]
    if {$t=="neighbors"} {
      binary scan $blob i* L
      set blob [binary format w* $L]
    }
    db eval "INSERT INTO $t VALUES(\$ii, \$blob)"
  }

  close $fd
}

db eval COMMIT

