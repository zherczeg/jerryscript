/* This test contains default arguments tests. */

switch(0) {
default:
next_statement;

function f(a = 5, b = ((Math.cos(0)))) {}

next_statement;

var o = {
    f: function(a, b = (1,2), c) {},
    g: 5,
  }

next_statement;
}
