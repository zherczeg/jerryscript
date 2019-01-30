/* This test contains no auto return parser tests. */

function f()
{
  a + b
}

function g()
{
  return 0 + y * 257;
}

function h()
{
  if (x) return 256 + -256 + -+-256;
}

function i()
{
  while (true) { return }
}

function k()
{
  if (x) return 1
  else return 2
}

