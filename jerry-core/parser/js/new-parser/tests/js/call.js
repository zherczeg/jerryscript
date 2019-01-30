/* This test contains function call tests. */

next_statement;

f();
f(a);
f(a,b);
f(a,b,c);

next_statement;

o.f();
o.f(a);
o.f(a,b);
o.f(a,b,c);

next_statement;

f(function() {}, function() {});

next_statement;
