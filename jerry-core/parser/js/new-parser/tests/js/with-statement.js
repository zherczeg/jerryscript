/* This test contains with statement parser tests. */

function func(arg,val,c,arg,val)
{
  with (val) {
    v1 = arg;
    {
      try {
        with (ident)
          arg.other = v1.val;
      } finally {}
    }
    arg.other++
  }

  var v1, ident, other;
  other = val;
}

next_statement;

with (a)
{
  f(a, g());
  eval(a, b, eval("1"));
}

next_statement;

with (a)
{
  g = (function () {var a=5;f(a);eval(a);});
  g = { get x() { f(8); } };
  function h() {var a=2;f(a);}
}

next_statement;

with (a)
{
  (function () {
    function x() { return function(a) { f(a); } }
  })
}
