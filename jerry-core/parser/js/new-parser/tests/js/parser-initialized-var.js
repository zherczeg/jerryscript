/* This test contains with initialized var tests. */

next_statement;

(function g() { return g; })

next_statement;

(function f(f) { return f; })

next_statement;

(function f(a,a,f) { return a + f; })

next_statement;

function f(x)
{
  function f() {}
  function f() {}
  return f*x;
}

next_statement;

(function g(a,g) {
  function g() {}
  function g() {}
  function a() {}
  return a + g;
})

next_statement;

(function m() {
  var f, g;
  function g() {}
  function f() {}
  return f + g;
})

next_statement;

(function () {
  var a1,a2,a3;

  a1 = 5;
  a2 = 5;
  a3 = 5;

  function a1() {}
  function a2() {}
  function a3() {}
})
