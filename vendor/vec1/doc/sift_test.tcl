
package require sqlite3

if {[llength $argv]!=2} {
  puts stderr "usage: $argv0 DATABASE SHLIB"
  exit 1
}

# Open the database. And load the vec1.so extension.
#
set shlib [lindex $argv 1]
sqlite3 db [lindex $argv 0]
db enable_load_extension 1
db eval { SELECT load_extension($shlib) }
db enable_load_extension 0

# Check that the "best" table exists. If it does not, create and 
# populate it now.
#
if {[db one {SELECT count(*) FROM sqlite_schema WHERE name='best'}]==0} {
  db eval {
    CREATE TABLE best(id INTEGER PRIMARY KEY, best);

    WITH groundtruth(qid, rank, val, dist) AS (
      SELECT q.id, j.key, j.value, vec1_l2_distance(q.val, b.val)
      FROM sift_query q, 
           sift_groundtruth g, 
           json_each(vec1_to_json_i(g.val)) j, 
           sift_base b
      WHERE q.id=g.id AND j.key<5 AND b.id=j.value
    )
    INSERT INTO best
      SELECT qid, json_group_array(val) 
      FROM groundtruth 
      GROUP BY qid, dist HAVING min(rank)==0;
  }
}

# Drop any old "v1" table.
db eval { DROP TABLE IF EXISTS v1 }
#db eval { PRAGMA mmap_size = 2000000000 }

proc train {codesize buckets} {

  if {$codesize==0 && $buckets==0} {
    db eval { INSERT INTO v1(cmd, vector) VALUES('rebuild', 'flat') }
  } elseif {$codesize>=0} {
    db eval "
      INSERT INTO v1(cmd, vector) VALUES('rebuild', (
         SELECT
            vec1_train(val, '{codesize: $codesize, buckets: $buckets}')
         FROM sift_learn
      ));
    "
  }
}

proc my_time {caption script} {
  puts -nonewline "$caption..."
  flush stdout
  set t [uplevel [list time $script]]
  set t [lindex $t 0]
  puts " ${t}us"
  return $t
}

if 1 {
set tn 0
foreach {name codesize buckets nquery K} {
  "none"                          -1   -1   100   1
  "flat"                           0    0   100   1
  "{codesize:  0, buckets: 1024}"  0 1024 10000   1
  "{codesize: 16, buckets:    0}"  0 1024 10000 100
  "{codesize: 16, buckets: 1024}" 16 1024 10000 100
} {
  db eval { PRAGMA mmap_size = 0 }
  db eval { DROP TABLE IF EXISTS v1 }
  db eval { CREATE VIRTUAL TABLE v1 USING vec1 }

  set TM($tn,name) $name
  set TM($tn,nquery) $nquery
  set TM($tn,train) [my_time "Training \"$name\"" {
    set T [train $codesize $buckets]
  }]

  set TM($tn,build) [my_time "Building \"$name\"" {
    db eval {
      INSERT INTO v1(rowid, vector) SELECT id, val FROM sift_base;
    }
  }]

  set sql "
    SELECT sum( EXISTS(SELECT 1 FROM json_each(a) WHERE value=b) ) FROM (
        SELECT id, 
        (SELECT best FROM best WHERE id=q.id) AS a,
        (SELECT rowid FROM v1(q.val, '{K: $K, nProbe: 0.05}') ORDER BY vec1_l2_distance(q.val, vector) ) AS b
        FROM sift_query q LIMIT $nquery
    );
  "

  set TM($tn,query) [my_time "Querying \"$name\"" {
    set TM($tn,ngood) [db one $sql]
  }]

  db eval { PRAGMA mmap_size = 2000000000 }
  set TM($tn,mmap_query) [my_time "Querying \"$name\"" {
    db one $sql
  }]

  my_time "Resting for 10s" { after 10000 }
  incr tn
}
puts [array get TM]
}

proc time_as_ms {t {high 0}} {
  set ms [expr $t/1000]
  if {$t>(10*1000*1000)} {
    return [format "%.1f s" [expr (1.0*$t)/1000000]]
  } elseif {$t>(1000*1000)} {
    return [format "%.2f s" [expr (1.0*$t)/1000000]]
  } elseif {$high} {
    if {$t>1000} {
      return [format "%.1f ms" [expr (1.0*$t)/1000]]
    }
    return [format "%.2f ms" [expr (1.0*$t)/1000]]
  }
  return "[expr $t/1000] ms"
}

proc time_per_query {t nquery} {
  set tpq [expr $t/$nquery]
  time_as_ms $tpq 1
}

proc query_throughput {t nquery} {
  set tput [expr (1000000.0 * $nquery) / $t]
  if {$tput<50} {
    set tput [format "%.1f qps" $tput]
  } else {
    set tput [format "%d qps" [expr int($tput + 0.5)]]
  }
}

puts "<table class=tbl>"
puts "<tr> <th>Index       <th>Training Time    <th> Index Build Time "
puts "     <th> Query Throughput <th> mmap Query Throughput<th> Recall"
for {set ii 0} {$ii < $tn} {incr ii} {
  puts -nonewline "<tr>"
  puts -nonewline "<td> $TM($ii,name)"
  puts -nonewline "<td> [time_as_ms $TM($ii,train)]"
  puts -nonewline "<td> [time_as_ms $TM($ii,build)]"
  puts -nonewline "<td> [query_throughput $TM($ii,query) $TM($ii,nquery)]"
  puts -nonewline "<td> [query_throughput $TM($ii,mmap_query) $TM($ii,nquery)]"
  puts            "<td> [expr (1.0*$TM($ii,ngood))/$TM($ii,nquery)]"
}
puts "</table>"


