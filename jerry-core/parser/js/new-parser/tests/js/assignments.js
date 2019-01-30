/* This test contains assignment parser tests. */

next_statement;

a = b;

next_statement;

a = 2 * b;

next_statement;

a.x = b;

next_statement;

a.x = 2 * b;

next_statement;

a[x] = b;

next_statement;

a[x] = 2 * b;

next_statement;

a["x"] = b = c;

next_statement;

f() = b;

next_statement;

f()++;

next_statement;

f() /= 4;

next_statement;

a *= b;

next_statement;

a.x >>>= b;

next_statement;

a["x"] *= b += d;

next_statement;

a[2 * 3] /= f() -= f();

next_statement;

x = this.b + f(this, b);
x = this[b];

next_statement;

this.a = 6;
this[a] = 7;
this.a += this.f();

next_statement;

for (this.a in b) { }

next_statement;

String(i++);
String(a,++i);
String(args[i++]);
String(a,10000--);

next_statement;

String(i = j);
String(a, i = j);
String(a, i = j = k);
String(a, 10000 = j = k);

next_statement;

String(i += b);
String(a, i /= b);
String(a, i >>= j -= k);
String(a, 10000 >>= j -= k);

next_statement;
