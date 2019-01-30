/* This test contains property access parser tests. */

next_statement;

a.b;
a[x];
a[x + y];

next_statement;

a[true ? a : b];
a[a || b];

next_statement;

for (a.b in c) {}

next_statement;

a.b();
a[b] = c;
a.b %= c;
a[i] ++;
--a.j;

next_statement;
