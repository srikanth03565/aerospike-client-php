--TEST--
listRemove() with index value is negative integer 

--DESCRIPTION--
This testcase will remove the element from the list at the given index,
Index value is negative integer so it will delete element indexing from end.

--FILE--
<?php
include dirname(__FILE__)."/../../astestframework/astest-phpt-loader.inc";
aerospike_phpt_runtest("ListRemove", "normal_002");
--EXPECT--
OK
