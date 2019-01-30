/* This test contains object literal parser tests. */

var a = {
  [a + b]: true,
  [a - b]: c,
  get [a * b]() {},
  set [a / b](v) {},
};

var b = {
  [a + b,]:1
}
