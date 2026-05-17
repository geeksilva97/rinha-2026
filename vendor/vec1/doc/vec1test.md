
Tests on Publicly Available Data
================================

<style>
  .tbl th { padding-left: 1ex !important ; padding-right: 1ex !important }
  .tbl td { padding: 0.5em 2ex 0.5em 2ex !important }
  .tbl th { text-align: center }
  .tblfirst td { border-top: solid black 1px }
  pre span { font-weight: bold; }

  .tbl2 th { text-align: left !important }
  .tbl2 th { padding: 0.5em 4ex 0.5em 0ex !important }

</style>

This page contains some tests run on publicly available datasets distributed
for testing ANN systems. The following datasets are used:

  *  "sift1m", from <a href=http://corpus-texmex.irisa.fr/>corpus-texmex.irisa.fr</a>,

  *  "imagenet-clip-512-normalized", from the 
     <a href=https://github.com/vector-index-bench/vibe>VIBE project</a>, and

  *  "landmark-dino-768-cosine", also from the VIBE projbect, and

  *  "agnews-mxbai-1024-euclidean", from VIBE again.

All four datasets are distributed with sample query vectors and results. 
"sift1m" is distributed with dedicated training data (separate from the dataset
itself), and the others are trained by sampling the main dataset. In all
cases training and building the index are done with 16 threads. The "Training 
time" reported below is the wall-clock time the system spent running 
vec1\_train() to create the mode. The "Build time" is the wall-clock time
spent running the 'rebuild' command to build the index.

Queries are run with a variety of values for parameters K and nprobe and the
recall and throughput (queries/second) reported. The reported throughputs are
for a single thread only.

Two flavours of recall are reported - recall@1 and recall@10. Recall@1 is 
the proportion of queries for the single nearest neighbour that do, in fact
return the true nearest neighbour. So a recall@1 of .916 means that if 1000
queries for the nearest neighbour are run, 916 of them return the true
nearest neighbour.

Recall@10 is the average proportion of the true 10 nearest neighbours actually
returned when the index is queried for the best 10 matches. i.e. if of the 10
results the query returns 7 of them are actually in the best 10 matches, the
recall@10 is 0.7. The order of results returned does not matter - only the
proportion that are part of the actual 10 best matches.

The SQL used for each query is:

<pre>
        SELECT * 
        FROM vec1tbl($query, '{K: $K, nprobe: $nprobe}')
        ORDER BY vec1_l2_distance($query, vec1tbl.vector) 
        LIMIT $recall
</pre>

Where $query is the query vector, $K is replaced by query parameter K, $nprobe
by query parameter nprobe and $recall with the desired recall measure (1 or
10). For the "landmark-dino-768-cosine" index, `vec1_cos_distance()` is used
instead of `vec1_l2_distance()`. In cases where $K==$recall the ORDER BY and
LIMIT clauses are omitted from the query.

The results below were obtained on an AMD 5950X CPU based Linux workstation. 

<!-- Report starts here!!! -->
<h1>Dataset: sift-128-euclidean (1,000,000 128d vectors)</h1>
<table class=tbl2>
<tr><th>Vec1 Version: <td>version 0.5 (AVX2, multi-threaded)
<tr><th>Index Parameters: <td>{codesize:16, nbucket:1024, distance: "L2", opq: 0}
<tr><th>Training time: <td>2.43s (100,000 samples, 16 threads)
<tr><th>Build time: <td>1.32s (16 threads)
</table>
<p>
<table class=tbl><tr>
<th rowspan=2>Recall@ <th rowspan=2>nProbe 
<th><th colspan=2> K=1 
<th><th colspan=2> K=10 
<th><th colspan=2> K=50 
<th><th colspan=2> K=100 
<th><th colspan=2> K=200 
<th><th colspan=2> K=300 
<tr>
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<tr class=tblfirst> <td> @10 <td> 16 
<td><td> 
<td> 
<td><td> 0.558
<td> 6430
<td><td> 0.882
<td> 4666
<td><td> 0.919
<td> 3712
<td><td> 0.928
<td> 2661
<td><td> 0.929
<td> 2099
<tr> <td> @10 <td> 32 
<td><td> 
<td> 
<td><td> 0.567
<td> 3559
<td><td> 0.916
<td> 2925
<td><td> 0.962
<td> 2514
<td><td> 0.975
<td> 1985
<td><td> 0.977
<td> 1652
<tr> <td> @10 <td> 48 
<td><td> 
<td> 
<td><td> 0.569
<td> 2506
<td><td> 0.924
<td> 2161
<td><td> 0.973
<td> 1927
<td><td> 0.987
<td> 1594
<td><td> 0.990
<td> 1367
<tr> <td> @10 <td> 64 
<td><td> 
<td> 
<td><td> 0.569
<td> 1962
<td><td> 0.927
<td> 1729
<td><td> 0.976
<td> 1567
<td><td> 0.992
<td> 1340
<td><td> 0.995
<td> 1173
<tr class=tblfirst> <td> @1 <td> 16 
<td><td> 0.462
<td> 6533
<td><td> 0.879
<td> 5885
<td><td> 0.949
<td> 4769
<td><td> 0.953
<td> 3817
<td><td> 0.954
<td> 2741
<td><td> 0.954
<td> 2158
<tr> <td> @1 <td> 32 
<td><td> 0.467
<td> 3630
<td><td> 0.902
<td> 3421
<td><td> 0.981
<td> 3013
<td><td> 0.987
<td> 2615
<td><td> 0.987
<td> 2014
<td><td> 0.987
<td> 1656
<tr> <td> @1 <td> 48 
<td><td> 0.468
<td> 2533
<td><td> 0.908
<td> 2426
<td><td> 0.989
<td> 2184
<td><td> 0.996
<td> 1948
<td><td> 0.997
<td> 1609
<td><td> 0.997
<td> 1380
<tr> <td> @1 <td> 64 
<td><td> 0.469
<td> 1945
<td><td> 0.909
<td> 1907
<td><td> 0.991
<td> 1733
<td><td> 0.998
<td> 1600
<td><td> 0.999
<td> 1354
<td><td> 0.999
<td> 1176
</table>
<!-- Report ends here!!! -->

<!-- Report starts here!!! -->
<h1>Dataset: imagenet-clip-512-normalized (1,281,167 512d vectors)</h1>
<table class=tbl2>
<tr><th>Vec1 Version: <td>version 0.5 (AVX2, multi-threaded)
<tr><th>Index Parameters: <td>{codesize:32, nbucket:1024, distance: "L2", opq: 1, residual: 0}
<tr><th>Training time: <td>30.0s (100,000 samples, 16 threads)
<tr><th>Build time: <td>7.88s (16 threads)
</table>
<p>
<table class=tbl><tr>
<th rowspan=2>Recall@ <th rowspan=2>nProbe 
<th><th colspan=2> K=1 
<th><th colspan=2> K=10 
<th><th colspan=2> K=50 
<th><th colspan=2> K=100 
<th><th colspan=2> K=200 
<th><th colspan=2> K=300 
<tr>
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<tr class=tblfirst> <td> @10 <td> 16 
<td><td> 
<td> 
<td><td> 0.531
<td> 3132
<td><td> 0.902
<td> 2523
<td><td> 0.959
<td> 2172
<td><td> 0.977
<td> 1702
<td><td> 0.980
<td> 1401
<tr> <td> @10 <td> 32 
<td><td> 
<td> 
<td><td> 0.533
<td> 1897
<td><td> 0.910
<td> 1648
<td><td> 0.968
<td> 1484
<td><td> 0.988
<td> 1246
<td><td> 0.992
<td> 1078
<tr> <td> @10 <td> 48 
<td><td> 
<td> 
<td><td> 0.534
<td> 1362
<td><td> 0.912
<td> 1225
<td><td> 0.971
<td> 1133
<td><td> 0.991
<td> 988
<td><td> 0.995
<td> 879
<tr> <td> @10 <td> 64 
<td><td> 
<td> 
<td><td> 0.534
<td> 1067
<td><td> 0.912
<td> 980
<td><td> 0.971
<td> 919
<td><td> 0.992
<td> 821
<td><td> 0.996
<td> 744
<tr class=tblfirst> <td> @1 <td> 16 
<td><td> 0.426
<td> 3119
<td><td> 0.881
<td> 2675
<td><td> 0.984
<td> 2123
<td><td> 0.992
<td> 1998
<td><td> 0.992
<td> 1644
<td><td> 0.993
<td> 1399
<tr> <td> @1 <td> 32 
<td><td> 0.426
<td> 1917
<td><td> 0.884
<td> 1844
<td><td> 0.987
<td> 1664
<td><td> 0.995
<td> 1495
<td><td> 0.995
<td> 1255
<td><td> 0.996
<td> 1083
<tr> <td> @1 <td> 48 
<td><td> 0.428
<td> 1369
<td><td> 0.887
<td> 1331
<td><td> 0.990
<td> 1236
<td><td> 0.998
<td> 1138
<td><td> 0.998
<td> 992
<td><td> 0.999
<td> 883
<tr> <td> @1 <td> 64 
<td><td> 0.428
<td> 1071
<td><td> 0.888
<td> 1049
<td><td> 0.991
<td> 986
<td><td> 0.999
<td> 923
<td><td> 0.999
<td> 824
<td><td> 1.000
<td> 746
</table>
<!-- Report ends here!!! -->

<!-- Report starts here!!! -->
<h1>Dataset: landmark-dino-768-cosine (760,757 768d vectors)</h1>
<table class=tbl2>
<tr><th>Vec1 Version: <td>version 0.5 (AVX2, multi-threaded)
<tr><th>Index Parameters: <td>{codesize:48, nbucket:1024, distance: "cos", opq: 1, residual: 0}
<tr><th>Training time: <td>44.0s (100,000 samples, 16 threads)
<tr><th>Build time: <td>8.64s (16 threads)
</table>
<p>
<table class=tbl><tr>
<th rowspan=2>Recall@ <th rowspan=2>nProbe 
<th><th colspan=2> K=1 
<th><th colspan=2> K=10 
<th><th colspan=2> K=50 
<th><th colspan=2> K=100 
<th><th colspan=2> K=200 
<th><th colspan=2> K=300 
<tr>
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<tr class=tblfirst> <td> @10 <td> 16 
<td><td> 
<td> 
<td><td> 0.582
<td> 2495
<td><td> 0.905
<td> 2022
<td><td> 0.941
<td> 1754
<td><td> 0.950
<td> 1410
<td><td> 0.951
<td> 1196
<tr> <td> @10 <td> 32 
<td><td> 
<td> 
<td><td> 0.590
<td> 1571
<td><td> 0.927
<td> 1354
<td><td> 0.967
<td> 1218
<td><td> 0.978
<td> 1036
<td><td> 0.980
<td> 905
<tr> <td> @10 <td> 48 
<td><td> 
<td> 
<td><td> 0.593
<td> 1173
<td><td> 0.934
<td> 1048
<td><td> 0.976
<td> 963
<td><td> 0.988
<td> 844
<td><td> 0.990
<td> 755
<tr> <td> @10 <td> 64 
<td><td> 
<td> 
<td><td> 0.594
<td> 927
<td><td> 0.936
<td> 851
<td><td> 0.979
<td> 793
<td><td> 0.992
<td> 706
<td><td> 0.993
<td> 642
<tr class=tblfirst> <td> @1 <td> 16 
<td><td> 0.554
<td> 2528
<td><td> 0.910
<td> 2233
<td><td> 0.971
<td> 1816
<td><td> 0.975
<td> 1691
<td><td> 0.975
<td> 1392
<td><td> 0.975
<td> 1206
<tr> <td> @1 <td> 32 
<td><td> 0.557
<td> 1580
<td><td> 0.919
<td> 1525
<td><td> 0.985
<td> 1363
<td><td> 0.989
<td> 1226
<td><td> 0.990
<td> 1040
<td><td> 0.990
<td> 909
<tr> <td> @1 <td> 48 
<td><td> 0.559
<td> 1159
<td><td> 0.924
<td> 1128
<td><td> 0.991
<td> 1034
<td><td> 0.995
<td> 949
<td><td> 0.996
<td> 831
<td><td> 0.996
<td> 741
<tr> <td> @1 <td> 64 
<td><td> 0.558
<td> 940
<td><td> 0.926
<td> 919
<td><td> 0.993
<td> 854
<td><td> 0.997
<td> 792
<td><td> 0.998
<td> 695
<td><td> 0.998
<td> 630
</table>
<!-- Report ends here!!! -->

<!-- Report starts here!!! -->
<h1>Dataset: agnews-mxbai-1024-euclidean (769,382 1024d vectors)</h1>
<table class=tbl2>
<tr><th>Vec1 Version: <td>version 0.5 (AVX2, multi-threaded)
<tr><th>Index Parameters: <td>{codesize:32, nbucket:1024, distance: "L2", opq: 1, residual: 0}
<tr><th>Training time: <td>58.2s (100,000 samples, 16 threads)
<tr><th>Build time: <td>13.3s (16 threads)
</table>
<p>
<table class=tbl><tr>
<th rowspan=2>Recall@ <th rowspan=2>nProbe 
<th><th colspan=2> K=1 
<th><th colspan=2> K=10 
<th><th colspan=2> K=50 
<th><th colspan=2> K=100 
<th><th colspan=2> K=200 
<th><th colspan=2> K=300 
<tr>
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<tr class=tblfirst> <td> @10 <td> 16 
<td><td> 
<td> 
<td><td> 0.619
<td> 2776
<td><td> 0.891
<td> 2176
<td><td> 0.931
<td> 1833
<td><td> 0.948
<td> 1387
<td><td> 0.952
<td> 1126
<tr> <td> @10 <td> 32 
<td><td> 
<td> 
<td><td> 0.625
<td> 2042
<td><td> 0.906
<td> 1708
<td><td> 0.951
<td> 1478
<td><td> 0.969
<td> 1174
<td><td> 0.974
<td> 973
<tr> <td> @10 <td> 48 
<td><td> 
<td> 
<td><td> 0.627
<td> 1608
<td><td> 0.911
<td> 1409
<td><td> 0.957
<td> 1252
<td><td> 0.977
<td> 1033
<td><td> 0.982
<td> 876
<tr> <td> @10 <td> 64 
<td><td> 
<td> 
<td><td> 0.627
<td> 1383
<td><td> 0.913
<td> 1200
<td><td> 0.959
<td> 1091
<td><td> 0.979
<td> 919
<td><td> 0.985
<td> 790
<tr class=tblfirst> <td> @1 <td> 16 
<td><td> 0.616
<td> 2746
<td><td> 0.914
<td> 2336
<td><td> 0.959
<td> 1827
<td><td> 0.967
<td> 1726
<td><td> 0.969
<td> 1366
<td><td> 0.969
<td> 1125
<tr> <td> @1 <td> 32 
<td><td> 0.623
<td> 2065
<td><td> 0.928
<td> 1971
<td><td> 0.977
<td> 1719
<td><td> 0.986
<td> 1496
<td><td> 0.988
<td> 1182
<td><td> 0.988
<td> 976
<tr> <td> @1 <td> 48 
<td><td> 0.623
<td> 1646
<td><td> 0.932
<td> 1571
<td><td> 0.982
<td> 1405
<td><td> 0.992
<td> 1246
<td><td> 0.994
<td> 1022
<td><td> 0.994
<td> 868
<tr> <td> @1 <td> 64 
<td><td> 0.622
<td> 1393
<td><td> 0.933
<td> 1339
<td><td> 0.983
<td> 1215
<td><td> 0.992
<td> 1092
<td><td> 0.995
<td> 915
<td><td> 0.995
<td> 786
</table>
<!-- Report ends here!!! -->


