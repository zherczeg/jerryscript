/* This test contains comma separated expressions parser tests. */

next_statement;

/* Property form */

((f().a))();

(((a.b)))++;

--((a));

next_statement;

/* Value form */

((x, f().a))();

(((x, a.b)))++;

--((a, b));

next_statement;

(a, b, c.d);

next_statement;

with (x)
{
  (a, b)();
}

next_statement;

(a = 5, b = 6) == 6;
(a = 5, b = 6);
a = 5, b = 6;

next_statement;
