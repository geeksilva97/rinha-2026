
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

The results below were obtained on a Samsung S24 Android cellphone. Results
for running similar tests (and explanations of how the tests are run and what
the results mean) are <a href=vec1test.md>available here</a>.

<!-- Report starts here!!! -->
<h1>Dataset: sift1m (1,000,000 128d vectors)</h1>
<table class=tbl2>
<tr><th>Vec1 Version: <td>version 0.1 (built with NEON)
<tr><th>Index Parameters: <td>{codesize:16, nbucket:1024, nthread:8, distance: "l2"}
<tr><th>Training time: <td>18.5s (100,000 samples, 8 threads)
<tr><th>Build time: <td>9.01s (8 threads)
</table>
<p>
<table class=tbl><tr>
<th rowspan=2>Recall@ <th rowspan=2>nProbe
<th><th colspan=2> K=1
<th><th colspan=2> K=10
<th><th colspan=2> K=100
<th><th colspan=2> K=200
<tr>
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<th><th> Recall <th> QPS
<tr class=tblfirst> <td> @10 <td> 0.01
<td><td>
<td>
<td><td> 0.540
<td> 3838
<td><td> 0.868
<td> 2454
<td><td> 0.875
<td> 1874
<tr> <td> @10 <td> 0.05
<td><td>
<td>
<td><td> 0.563
<td> 1148
<td><td> 0.972
<td> 955
<td><td> 0.988
<td> 838
<tr class=tblfirst> <td> @1 <td> 0.01
<td><td> 0.451
<td> 2654
<td><td> 0.848
<td> 2467
<td><td> 0.913
<td> 2454
<td><td> 0.913
<td> 1903
<tr> <td> @1 <td> 0.05
<td><td> 0.466
<td> 1161
<td><td> 0.902
<td> 1127
<td><td> 0.994
<td> 964
<td><td> 0.995
<td> 843
</table>
<!-- Report ends here!!! -->

