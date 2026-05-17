
<style>
  .tbl td { padding: 0.5em 1ex 0.5em 1ex !important }
  .tbl th { padding: 0.5em 1ex 0.5em 1ex !important }
  pre span { font-weight: bold; }
</style>




Vec1 sift1m Data Tests
======================

One of the datasets routinely used for testing during routine development of 
vec1 is the "sift1m" dataset. It may be downloaded from 
<a href="http://corpus-texmex.irisa.fr/">http://corpus-texmex.irisa.fr/</a>.
It contains vectors describing local image patches. Specifically, it contains
1,000,000 128-dimension vectors. In this version the elements are 32-bit floating
point values.

The tar file from the URI above contains 1,000,000 dataset vectors, 100,000
training vectors and 10,000 query vectors. For each query vector it also
contains the ids of the best 100 matches in the dataset. These can be loaded
into an SQLite database using the "load\_vectors.tcl" script. For example,
if $VEC1 is set to the root of the vec1 source tree, the following
procedure may be used to download the sift1m data and load it into an
SQLite database:

<pre>
    $ <span>wget ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz</span>
    ...
    $ tar xvf sift.tar.gz
    sift/
    sift/sift_base.fvecs
    sift/sift_groundtruth.ivecs
    sift/sift_learn.fvecs
    sift/sift_query.fvecs
    $ <span>tclsh $VEC1/load_vectors.tcl siftdb.db sift/*</span>
</pre>

At this point the database contains 4 tables, each with vectors loaded
from one of the file in the sift.tar.gz archive:

<pre>
    sqlite> <span>.schema</span>
    CREATE TABLE sift_base (id INTEGER PRIMARY KEY, val BLOB);
    CREATE TABLE sift_groundtruth (id INTEGER PRIMARY KEY, val BLOB);
    CREATE TABLE sift_learn (id INTEGER PRIMARY KEY, val BLOB);
    CREATE TABLE sift_query (id INTEGER PRIMARY KEY, val BLOB);
    sqlite> <span>select id, length(val), typeof(val) from sift_base limit 5;</span>
    ╭────┬─────────────┬─────────────╮
    │ id │ length(val) │ typeof(val) │
    ╞════╪═════════════╪═════════════╡
    │  0 │         512 │ blob        │
    │  1 │         512 │ blob        │
    │  2 │         512 │ blob        │
    │  3 │         512 │ blob        │
    │  4 │         512 │ blob        │
    ╰────┴─────────────┴─────────────╯
</pre>

The sift\_query table contains 10,000 query vectors. For each row, there is
a corresponding row in the sift\_groundtruth table containing the 100
nearest neighbors to each query. This is useful for testing query recall, but
there are two problems with the data:

  *  Each set of 100 neighbors is stored as packed array of 32-bit integers,
     which is tricky to deal with in SQL, and

  *  There are some cases where there are two dataset vectors that are 
     equally close to a query vector. This is also tricky to deal with.

And so, we create our own table of nearest neighbours using the following:

<pre>
    sqlite> <span>CREATE TABLE neighbors(id INTEGER PRIMARY KEY, best);</span>
    sqlite> <span>WITH groundtruth(qid, rank, nid, dist) AS (
      SELECT q.id, j.key, j.value, vec1_l2_distance(q.val, b.val)
      FROM sift_query q,
           sift_groundtruth g,
           json_each(vec1_to_json_i(g.val)) j,
           sift_base b
      WHERE q.id=g.id AND j.key<5 AND b.id=j.value
    )
    INSERT INTO neighbors
      SELECT qid, json_group_array(nid)
      FROM groundtruth
      GROUP BY qid, dist HAVING min(rank)==0;</span>
</pre>

The above creates a table named "neighbors" with one row for each row in the
sift\_query table. For query vectors that have a single nearest neighbor, the
"best" column of "neighbors" contains a json array containing the id of the
neighbor. For cases where there are two equally near neighbors, the json array
contains both of them. As follows:

<pre>
    sqlite> <span>SELECT * FROM neighbors WHERE id BETWEEN 210 AND 220;</span>
    ╭─────┬─────────────────╮
    │ id  │      best       │
    ╞═════╪═════════════════╡
    │ 210 │ [176331]        │
    │ 211 │ [947639]        │
    │ 212 │ [902479]        │
    │ 213 │ [193524,272340] │
    │ 214 │ [160527]        │
    │ 215 │ [960303]        │
    │ 216 │ [744151]        │
    │ 217 │ [170149]        │
    │ 218 │ [924968]        │
    │ 219 │ [264459,200181] │
    │ 220 │ [510083]        │
    ╰─────┴─────────────────╯
</pre>

We can now run some experiments. First, create a vec1 virtual table. Then
train a model based on the contents of the sift\_learn table (100,000 vectors):

<pre>
    sqlite> <span>CREATE VIRTUAL TABLE v1 USING vec1();</span>
    sqlite> <span>.timer on</span>
    sqlite> <span>INSERT INTO v1(cmd, vector) VALUES('rebuild', (
      SELECT vec1_train(val, '{codesize: 16, buckets: 1024}') FROM sift_learn
    ));</span>
    Run Time: real 25.130811 user 25.064112 sys 0.055404
</pre>

Now insert the data into the virtual table. Because the table has already been
configured with a model, the index is automatically built as data is inserted:
<pre>
    sqlite> <span>INSERT INTO v1(rowid, vector) SELECT id, val FROM sift_base;</span>
    Run Time: real 15.993694 user 15.490449 sys 0.454168
</pre>

Now some queries. More precisely, run a query for each of the 10,000 rows in
the sift\_query table. Use the "neighbors" table created earlier to check if
the query really did find the exact nearest neighbor.

<pre>
    -- queries:
    --   Contains one row for each query. Consisting of the query-id and 
    --   the id of the row that the vec1 index identified as its nearest 
    --   neighbor.
    --
    -- queries_and_neighbors:
    --   Contains the same data as "queries", but also a column "best",
    --   containing the actual nearest neigbors. If the id value "vec1res"
    --   is in "best", then we have succeeded in finding the nearest neighbor.
    --
    -- Finally the outer query returns the total number of ANN queries, and 
    -- the total number for which the correct nearest neighbor was returned.
    --
    sqlite> <span>WITH queries(qid, vec1res) AS (
      SELECT 
        q.id, 
        (SELECT rowid FROM v1(q.val, '{K:1, nprobe: 0.05}'))
      FROM sift_query q
    ),
    queries_and_neighbors(qid, vec1res, best) AS (
      SELECT qid, vec1res, best
      FROM queries JOIN neighbors ON (qid=neighbors.id)
    )
    SELECT count(*) nquery, 
           sum(vec1res IN (SELECT value FROM json_each(best))) AS ncorrect
    FROM queries_and_neighbors;</span>
    ╭────────┬──────────╮
    │ nquery │ ncorrect │
    ╞════════╪══════════╡
    │  10000 │     4586 │
    ╰────────┴──────────╯
    Run Time: real 7.573562 user 6.235625 sys 1.336680
</pre>

Not good results. Of 10000 queries, only 4586 were answered accurately. But,
because the index stored compressed PQ codes instead of vectors (because
"codesize" was not set to 0 during training), vec1 determines the nearest
neighbor or neighbors based on approximate, not exact, distances from
the query vector. This means reranking can help. This time, instead of
asking the virtual table for the single nearest neighbor, ask it for the
nearest 100. Then recalculate the distances between the query vector and
each of these 100 candidates using the full vector. As follows:

<pre>
    sqlite> <span>WITH queries(qid, vec1res) AS (
      SELECT 
        q.id, 
        (
          SELECT rowid FROM v1(q.val, '{K:100, nprobe: 0.05}') 
          ORDER BY vec1_l2_distance(vector, q.val)
          LIMIT 1
        )
      FROM sift_query q
    ),
    queries_and_neighbors(qid, vec1res, best) AS (
      SELECT qid, vec1res, best
      FROM queries JOIN neighbors ON (qid=neighbors.id)
    )
    SELECT count(*) nquery, 
           sum(vec1res IN (SELECT value FROM json_each(best))) AS ncorrect
    FROM queries_and_neighbors;</span>
    ╭────────┬──────────╮
    │ nquery │ ncorrect │
    ╞════════╪══════════╡
    │  10000 │     9945 │
    ╰────────┴──────────╯
    Run Time: real 10.600256 user 7.663179 sys 2.936063
</pre>

Better. The SQL query is now slower (10.6 seconds against 7.6) but it gets the right
answer 99.45% of the time. Note the changes to the subquery inside the "queries" 
CTE - it now sets K to 100 and reranks those 100 approximate nearest neighbors using
the SQL ORDER BY clause.

The following table shows the results of running the above procedure with several
different index configurations. The "none" index means the virtual table was not
trained at all. The "flat" index means the constant 'flat' was used in place of
training data. Otherwise, the virtual table was configured with a model trained
as shown above using the specified values for "codesize" and "buckets".

The test platform was an AMD Ryzen 5950X CPU with AVX2 enabled. The 10,000
sift\_queries were run twice with each index, once using SQLite defaults (8MB
page-cache) and once with "PRAGMA mmap\_size = 1000000000", so that mmap() is
used to access the db. For the two configurations that use a non-zero codesize,
reranking was used with K set to 100. The "recall" column is the fraction of
the 10,000 queries for which the correct nearest neighbor was identified.

<table class=tbl>
<tr> <th>Index       <th>Training Time    <th> Index Build Time 
     <th> Query Throughput <th> mmap Query Throughput<th> Recall
<tr><td> none <td> 0 ms <td> 976 ms <td> 5.0 qps <td> 5.9 qps <td> 1.0
<tr><td> flat<td> 2 ms<td> 1.73 s<td> 9.4 qps<td> 23.8 qps<td> 1.0
<tr><td> {codesize:  0, buckets: 1024}<td> 12.2 s<td> 10.4 s<td> 155 qps<td> 411 qps<td> 0.9952
<tr><td> {codesize: 16, buckets:    0}<td> 12.1 s<td> 10.4 s<td> 149 qps<td> 378 qps<td> 0.9954
<tr><td> {codesize: 16, buckets: 1024}<td> 26.4 s<td> 16.6 s<td> 943 qps<td> 1279 qps<td> 0.9943
</table>

The following results are from running the same procedure on a 
<a href="https://en.wikipedia.org/wiki/Samsung_Galaxy_S24">Samsung S24 cellphone</a>:


<table class=tbl>
<tr> <th>Index       <th>Training Time    <th> Index Build Time 
     <th> Query Throughput <th> mmap Query Throughput<th> Recall
<tr><td> none<td> 0 ms<td> 2.92 s<td> 2.9 qps<td> 8.3 qps<td> 1.0
<tr><td> flat<td> 7 ms<td> 3.53 s<td> 3.1 qps<td> 6.6 qps<td> 1.0
<tr><td> {codesize:  0, buckets: 1024}<td> 35.0 s<td> 37.0 s<td> 65 qps<td> 95 qps<td> 0.9952
<tr><td> {codesize: 16, buckets:    0}<td> 43.5 s<td> 29.1 s<td> 44.9 qps<td> 75 qps<td> 0.9941
<tr><td> {codesize: 16, buckets: 1024}<td> 55.6 s<td> 44.0 s<td> 306 qps<td> 609 qps<td> 0.9939
</table>

