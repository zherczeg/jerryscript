/* This test contains object literal parser tests. */

function f() {
  for ({ get 10() {} } ; ; ) break;
}

var a = { get get(arg) {} }
