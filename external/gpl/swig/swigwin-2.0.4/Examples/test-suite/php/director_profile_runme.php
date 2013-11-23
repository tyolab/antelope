<?php
require "tests.php";
require "director_profile.php";

// No new functions
check::functions ( array (
		b_fn,
		b_vfi,
		b_fi,
		b_fj,
		b_fk,
		b_fl,
		b_get_self,
		b_vfs,
		b_fs 
) );
// No new classes
check::classes ( array (
		A,
		B 
) );
// now new vars
check::globals ( array () );
class MyB extends B {
	function vfi($a) {
		return $a + 3;
	}
}

$a = new A ();
$myb = new MyB ();
$b = B::get_self ( $myb );

$i = 50000;
$a = 1;

while ( $i ) {
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); // 0
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); //
	$a = $b->fi ( $a ); // 0
	$i -= 1;
}

print $a . "\n";

check::done ();
?>
