
Vec1 User Manual
================

<style>
  pre i { color: blue ; font-style: normal ; font-weight: normal}
  pre { font-weight: bold }
</style>

Contents:
========
<ul type=none>
  <li> 1. <a href=#vtab>Using the Virtual Table</a>
  <li> 2. <a href=#nn_and_ann>NN and ANN Configurations</a>
<ul type=none>
  <li> 2.1. <a href=#exhaustive>Exhaustive Search (NN) Modes</a>
  <li> 2.2. <a href=#ann>Approximate (ANN) Mode</a>
<ul type=none>
  <li> 2.2.1 <a href=#anntheory>Theory</a>
  <li> 2.2.2 <a href=#anntraining>Training and Index Building</a>
  <li> 2.2.3 <a href=#annquery>Querying</a>
  <li> 2.2.4 <a href=#reranking>Reranking</a>
</ul>
</ul>
  <li> 4. <a href=#filters>Filtered Vector Queries</a>
<ul type=none>
  <li> 4.1. <a href=#streaming>Streaming Queries</a>
  <li> 4.2. <a href=#metadata>Metadata Columns</a>
</ul>
</ul>

This is a user manual that attempts to explain how vec1 may be used and
integrated with other SQL queries. It is not a complete reference guide. For
that, please see <a href=vec1ref.md>this page</a>.

<a name=vtab></a>
1.\ Using The Virtual Table
=======================

A vec1 virtual table is created using the following syntax:

<pre>
    CREATE VIRTUAL TABLE vec1tbl USING vec1(vector);
</pre>

where "vector" may be replaced with any legal SQLite column name. Or omitted
entirely, in which case it defaults to "vector". This creates a virtual table
that behaves similarly to a regular table with the following schema:

<pre>
    CREATE TABLE vec1tbl(vector BLOB);
</pre>

The virtual table can then be populated using the usual SQL commands (e.g.
INSERT, UPDATE, DELETE). The "vector" column of each row should be populated 
with a vector, in Vec1's <a href=vec1ref.md#native>native blob format</a>. 
Each row is identified by a "rowid" column, which may, as for regular SQLite
tables, be populated explicitly or automatically.

<pre>
    <i>-- Insert a row with explicitly specified rowid value 1234. A blob 
    -- containing an array of 32-bit IEEE floating point values in machine-byte
    -- format must be bound to parameter :v1.</i>
    INSERT INTO vec1tbl(rowid, vector) (1234, :v1);

    <i>-- Insert a row with an automatically assigned rowid - which will
    -- presumably be retrieved later using sqlite3_last_insert_rowid() or 
    -- similar.</i>
    INSERT INTO vec1tbl(vector) (:v2);
</pre>

The virtual table will resist attempts to insert anything other than a blob
into the "vector" column. And, once the first vector has been inserted, it
will also refuse to accept blobs of a different length. All vectors must have
the name number of dimensions.

Once the virtual table is populated, it may be used as a table-valued function
for nearest-neighbor queries on its contents. For example:

<pre>
    <i>-- Retrieve the rowid values identifying the 10 nearest-neighbors 
    -- to vector :v. Parameter :v must be a blob containing a vector with
    -- the same number of dimensions as the table contents. </i>
    SELECT rowid FROM vec1tbl(:v, '{k: 10}');
</pre>

The above works, but there are two drawbacks:

1. Performance is poorer than it could be, and
2. The distance metric used to determine the nearest-neighbors is always
   Euclidean.

Fortunately, both these problems can be addressed by configuring the
virtual table.

<a name=nn_and_ann></a>
2.\ NN and ANN Configurations
=============================

It is currently considered impractical to provide <i>exact</i> results to a
nearest-neighbor (NN) query without an exhaustive search, which can be slow.
So many systems compromise and implement <i>approximate</i> nearest-neighbor
(ANN) queries. The results returned may not be the true nearest-neighbors, but
they'll usually be pretty close, and can be computed far more quickly than
an exhaustive search. For many applications this tradeoff - slightly lower
accuracy in exchange for much lower latency - is acceptable.

Vec1 may be configured to support either NN or ANN queries. Use 
<a href=#exhaustive>NN mode</a> if:

  *  Exact results are required, or

  *  The database will contain relatively few (say < 5000) vectors, or

  *  Training data is unavailable or training is inconvenient. In vec1,
     ANN mode requires a trained model, which requires training 
     data - often a subset of the actual data - and training time. 

Otherwise, use <a href=#ann>ANN mode</a>.

<a name=exhaustive></a>
2.1.\ Exhaustive Search (NN) Modes
----------------------------------

If exact results are required, or using a trained ANN model is impractical
for some reason, vec1 may still be used in either of the following modes:

  *  <b>No index</b> mode. This is the default. In this mode vectors are simply
     stored by the vec1 extension in an SQLite table. One vector per row. 

  *  <b>Flat index</b> mode. In this mode vectors are stored in packed arrays
     in large blob fields. This can make exhaustive searches over twice as
     fast as in "no index" mode.

To set a vec1 table to one of the above NN modes, an SQL statement like:

<pre>
    <i>-- Persistently configure the vec1 table to use flat index mode with</i>
    <i>-- the Euclidian (l2) distance metric.</i>
    <i>-- </i>
    INSERT INTO vec1tbl(cmd, arg) VALUES('rebuild', '{index:"flat", distance:"l2"}');
</pre>

The JSON object written to the "arg" column supports two parameters:

  *  <b>index</b> - this may be set to either "flat", for flat index mode,
     or "none", for no index mode.

  *  <b>distance</b> - this may be set to either "l2", to configure vec1 to 
     use Euclidean distance to calculate nearest-neighbors, or "cos", to
     tell it to use cosine distance.

Because they always return exact results, queries against vec1 in NN mode
do NOT benefit from <a href=#reranking>reranking</a>.

<a name=ann></a>
2.2.\ Approximate (ANN) NN mode
-------------------------------

To be most useful with a substantial number of vectors, a vec1 table should
be configured to provide ANN search using a trained model. 

<a name=anntheory></a>
### 2.2.1.\ Theory

Approximate Nearest-Neighbor search in this system is implemented using IVFADC
(sometimes called IVFPQ). There are many good introductions to this on the
internet, but briefly:

  *  The model contains a fixed number of buckets, each with an associated
     centroid selected during training. During index population, each vector
     is assigned to the bucket with the closest centroid.

  *  The model also contains a set of product quantization (PQ) codebooks 
     used to compress vectors. The vectors in each bucket are stored as a
     packed list of their compressed forms.

  *  When querying the index, the subset of buckets (say 3%) with the closest
     centroids to the query vector are selected. All other buckets are ignored
     by the query. The content of the selected bucket's contents are scanned 
     and the nearest-neighbors identified based on calculating the distance
     between the query vector and the compressed form of the database vector
     stored in the index.

<a name=anntraining></a>
### 2.2.2.\ Training and Index Building

Practically, a vec1 ANN model is a blob returned by running the SQL aggregate
function vec1\_train() over a representative set of vectors - the training set.
For example, to create a model based on the first 100,000 vectors currently
stored in table vec1tbl:

<pre>
    <i>-- Obtain a model - a blob - trained on the first 10,000 rows of
    -- table vec1tbl.  </i>
    WITH training_set(vector) AS (
      SELECT vector FROM vec1tbl LIMIT 100000
    )
    SELECT 
      vec1_train(vector, '{codesize: 16, nbucket: 1024, distance: "cos"}')
    FROM training_set;
</pre>

The first parameter passed to vec1\_train() is a training vector. The second
parameter is a JSON object specifying parameters for the model training. These
parameters are currently supported:

<ul>
  <li> <b>distance</b> - the distance metric to use. This must be set to 
       either "l2" (Euclidean distance) or "cos" (cosine distance). The
       default value is "l2".

  <li> <b>nbucket</b> - the number of buckets to use for the coarse quantizer.
       This must be set to a value between 32 and 4096, or else 0. A value of
       0 disables the coarse quanitizer altogether.

  <li> <b>opq</b> - a Boolean set to true to enable training an Optimized
       Product Quantization (OPQ) rotation. Using OPQ makes training more
       expensive, but often significantly improves recall, especially with
       modern embedding datasets. The default value is false (disabled).

  <li> <b>codesize</b> - the size in bytes of the product quantization (PQ)
       codes to use. This must be set to a value between 8 and 256, or else
       0. A value of 0 disables PQ compression altogether, so that full
       vectors are stored in the index. Storing full vectors is almost always
       slower, but eliminates the need for <a href=#reranking>reranking</a>.
</ul>

Once obtained, a model is provided to the vec1 table using a special INSERT
command:

<pre>
    <i>-- Provide a model - a blob returned by vec1_train() - to the 
    -- vec1 virtual table. </i>
    INSERT INTO vec1tbl(cmd, arg) VALUES('rebuild', :model);
</pre>

The 'rebuild' command above provides the model to the vec1 virtual table and
tells it to build its index based on the model and its current contents.
Following this, nearest-neighbor queries should run faster and use the correct
distance metric. 

A new model may be provided to an existing table at any time. The old index
and model are discarded before the new ones are installed and built. Models
may be trained on one host and used on another, provided that the hosts
use the same byte-ordering for 32-bit floating point values.

Training models and building indexes are both compute-heavy operations. Vec1
can be configured to use multiple threads using the `vec1_config` scalar
function, as shown below. This setting applies to the database connection -
it affects all training and rebuilding operations performed using the
same (sqlite3\*) handle. This should not be set to value greater than the 
number of real cores that the CPU has.

<pre>
    <i>-- Configure vec1 to use 8 threads for both training and index building</i>
    SELECT vec1_config('nthread', 8);
</pre>

Selecting the nbucket and codesize parameters for an index can be difficult.
Oft repeated guidelines are:

  *  <b>nbucket</b> should be set to roughly the square root of the expected 
     size of the dataset. So for a 1,000,000 vector db, 1000 buckets is a
     reasonable value.

  *  <b>opq</b> - whether or not to use an OPQ rotation. This usually provides
     the most benefit for vectors that come from neural networks (e.g. text or
     image embeddings), but does not for engineered decriptors like SIFT, HOG
     or GIST where variance is already evenly distributed throughout
     dimentsions. So if your vectors are the modern embedding type, it is 
     probably worth experimenting with OPQ.

  *  <b>codesize</b> - the size of PQ codes in bytes. Without OPQ this is 
     often 1/8 or 1/16 the number of vector dimensions. For overall compression
     ratios of 32 or 64 times. So for a table containing 512-dimension vectors,
     64 is a reasonable value to try first. With OPQ enabled, smaller PQ
     codes often provide the same recall. With OPQ, 32, 48 or 64 byte PQ
     codes are often enough to provide good recall, even on large vectors.

Often the best thing to do is to experiment with a variety of different
parameters. Application developers need not regard these as set in stone. New
models may be trained and indexes rebuilt as the dataset evolves.

<a name=annquery></a>
### 2.2.3.\ Querying

Vec1 tables in ANN mode are queried in the same way as other vec1 tables,
except that an extra JSON parameter, "nprobe", is relevant. For example:

<pre>
    <i>-- Retrieve 50 approximate nearest-neighbors to vector :v. Search
    -- 8% of the buckets in the index to find results.  </i>
    SELECT rowid FROM vec1tbl(:v, '{k:50, nprobe:0.08}');
</pre>

Currently, three query parameters are supported:

  *  <b>K</b> is the number of neighbors to return.

  *  <b>nprobe</b> determines the number of buckets to search for
     nearest-neighbors. If this value is greater than or equal to 1.0, then it
     is an absolute number of buckets to search. Or, if it is less than 1.0,
     then it is the fraction of total buckets to search. For example, an
     "nprobe" value of 0.25 means to search 1/4 of the total buckets in the
     index. The default value is 0.05.

  *  <b>streaming</b> may be set to 0 or 1. See below 
     <a href=#streaming>for details</a>.

Larger values of nprobe improve the quality of results returned, but also
increase query processing time.

<a name=reranking></a>
### 2.2.4.\ Reranking

Unless the model uses codesize of 0, vec1 returns nearest-neighbors based
on computing the distance between the query vector and the compressed vectors
stored in the index. And because these vectors are heavily compressed (often 
32 times or more), there is a discrepancy between the distance between the
query vector and the compressed vector, and the distance between the query
vector and the <i>actual, uncompressed</i> database vector. This difference
is enough to make results look quite poor.

The standard solution is "reranking". Reranking is basically saying -
"instead of asking the index for the best 10 matches, I'll ask it for the best
100, then calculate the distances between the query vector and the actual
vectors for those 100 and return the best 10". Because it's more flexible and
just as easy to do in SQL, vec1 does provide an option to automatically rerank
results. For example:

<pre>
    SELECT rowid, vector 
    FROM vec1tbl(:v, '{k:100}')            <i>-- 1. grab 100 neighbors from the index</i>
    ORDER BY vec1_l2_distance(:v, vector)  <i>-- 2. order them by actual distance </i>
    LIMIT 10;                              <i>-- 3. return only the best 10 </i>
</pre>

The query above uses <a href=vec1ref.md#l2_distance>vec1\_l2\_distance()</a>, a
scalar function for calculating the Euclidean distance between two vectors of
the same length. There is also <a href=vec1ref.md#cos_distance>vec1\_cos\_distance()</a>, 
to be used with vec1 tables that use cosine distance.

There are some tests run on publicly available datasets 
<a href=vec1test.md>here</a> that show clearly the advantages of reranking.

To be 100% clear, <b>reranking is usually essential</b> for ANN queries
with vec1. Unless you know otherwise, you need it. Unless of course you have a
model with "codesize:0". Then it's pointless extra work.

<a name=filters></a>
4.\ Filtered Vector Queries
===========================

Many applications require more than simply the nearest K neighbors to a
query vector. Often the results must also satisfy additional database
criteria. For example, a user searching an online store might want the
nearest 10 items to a query vector that are in stock and cost less
than $200.

Vec1 offers two features to facilitate these types of queries:

  *  <a href=#streaming>Streaming queries</a>, for use when the metadata (e.g.
     price, stock levels) are stored in separate tables, and

  *  <a href=#metadata>Metadata columns</a>, which allow the metadata to be
     stored in the vector index itself.

<a name=streaming></a>
4.1.\ Streaming Queries
-----------------------

Consider the following schema:

<pre>
    <i>-- Each row in vec1products has a corresonding row in table products,</i>
    <i>-- linked by (vec1products.rowid=products.id).</i>
    CREATE VIRTUAL TABLE vec1products USING vec1(vector);
    CREATE TABLE products(
        id INTEGER PRIMARY KEY,
        in_stock BOOLEAN,         -- 1 for in stock
        price INTEGER             -- price in cents
    );
</pre>

The user wants to find the nearest neighbour vectors corresponding to rows
of the `products` table for which in\_stock is true and price is less 
than 20000. It is awkward  to formulate the query in SQL, because you have
to pick a fixed value of "K" to pass to the virtual table. No matter what value
you pick, it is possible that none of the best K results
match the criteria. 

A streaming query solves this problem. Internally, vec1 assumes that
SQLite will consume roughly K results. However, if SQLite continues to
request rows after the first K have been returned, vec1 will continue
producing additional results in roughly ascending order of distance to the
query vector. Streaming mode is enabled by adding "streaming:1" to the JSON
query parameters.

Suppose the user's query vector is :v:

<pre>
    <i>-- The CTE collects 100 approximate matches that survive the</i>
    <i>-- metadata filters.</i>
    <i>--</i>
    <i>-- "K:200" tells vec1 to expect that roughly 200 rows will be</i>
    <i>-- requested. We estimate that about half will pass the filter.</i>
    <i>-- If more rows are needed, vec1 continues streaming results until</i>
    <i>-- SQLite hits the "LIMIT 100" and stops requesting them.</i>
    WITH best100(rowid, vector) AS (
      SELECT vp.rowid, vp.vector
      FROM vec1products(:v, '{K:200, streaming:1, nprobe:32}') vp
      JOIN products p ON (vp.rowid=p.id)
      WHERE p.in_stock AND p.price < 20000
      LIMIT 100
    )

    <i>-- Rerank the 100 matches from the CTE and return the best 10.</i>
    SELECT rowid FROM best100 
    ORDER BY vec1_l2_distance(:v, vector)
    LIMIT 10;
</pre>

Using the SQL above, 100 post-filter approximate neighbors are accumulated
for reranking before returning the best 10 to the user.

<a name=metadata></a>
4.2.\ Metadata Columns
----------------------

A vec1 table may be configured with metadata columns. Values stored in
metadata columns are embedded directly within the ANN index. This allows
filters to be applied before computing the distance between database
vectors and the query vector. Metadata columns are declared by adding column
names after the vector column in the CREATE VIRTUAL TABLE statement. For
example:

<pre>
    CREATE VIRTUAL TABLE vec1products USING vec1(vector, in_stock, price);
</pre>

Metadata columns do not have declared types. Any valid SQLite column name
may be used, except that it must not contain the "=" character. This
character is reserved for future extensions.

Any value may be stored in a metadata column. However, the system is 
optimized for small values - NULL, numbers or small text or blob 
fields (say 8 bytes).

Metadata values may be read and written like ordinary columns. They also have
a useful property: simple WHERE clause expressions that reference metadata
columns may be evaluated inside the ANN index search. This allows filtering to
occur before the nearest-neighbors are accumulated, which is usually much more
efficient than filtering outside the index using streaming queries.

A WHERE clause expression may be evaluated as part of the ANN index 
search if it:

  *  uses a comparison operator (<, >, =, >=, <=, or IS), and
  *  one side of the comparison is a metadata column.

Expressions that cannot be evaluated inside the ANN search are applied after
candidate vectors are returned from the index.

So, for example, the following query retrieves 100 neighbors that match
the two constraints from the vec1 table, then reranks them and returns the
10 best results:

<pre>
    SELECT rowid 
    FROM vec1products(:v, '{K:100}')
    WHERE in_stock = 1 AND price < 20000
    ORDER BY vec1_l2_distance(:v, vector)
    LIMIT 10;
</pre>

But it is easy to make mistakes when writing this type of query. For example,
if the user wrote WHERE in\_stock AND price < 20000, which is logically
equivalent, the results may differ. In this case the constraint on in\_stock
would not be evaluated inside the ANN search, because it does not use a
comparison operator.

As a result, vec1 would retrieve 100 nearest-neighbors that satisfy (price <
20000), then apply the in\_stock filter afterwards. If the in\_stock filter
removes too many rows, fewer than 10 results may remain. Even if 10 or more
rows remain, this is effectively equivalent to silently reducing the value of
K, which may reduce recall.

This pitfall can be avoided by formulating the query as follows, which ensures
that the LIMIT clause determines how many rows vec1 must produce:

<pre>
    WITH best100(rowid, vector) AS (
      SELECT rowid, vector
      FROM vec1products(:v, '{}')
      WHERE in_stock = 1 AND price < 20000
      LIMIT 100
    )
    SELECT rowid FROM best100
    ORDER BY vec1_l2_distance(:v, vector)
    LIMIT 10;
</pre>

In this case, no "K" value at all is passed to the vec1products virtual table.
Usually, this would be an error. But, under some circumstances, vec1 is able
to use the value attached to a LIMIT clause as its K value. These
circumstances are:

  *  The query (or subquery) is not a join,
  *  the query is not an aggregate,
  *  there is no ORDER BY clause, and
  *  all WHERE clause terms are handled inside the ANN search.

If no K value is supplied, and the query cannot use a value extracted from a
LIMIT clause, it is an error. This means that if the user made a mistake
and accidentally used WHERE clause terms that cannot be handled within the ANN
search, SQLite would return an error trying to prepare the SQL statement.

Alternatively, if using the second formulation of the query the user could
supply a "K" value and also specify "streaming:1". Then vec1 would handle
filtering as part of the ANN search whenever possible, falling back to a
streaming query in other cases.



