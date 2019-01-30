/* This test contains do-while statement parser tests. */

next_statement;

  do {
  } while (a)

next_statement;

  do //
  /* \u */;
  while (!new a)

next_statement;

  do /**/ do { x++; } while (
   true
  ); while (/**/a//
  );

next_statement;

  do
  { x(); do { y() } while(/**/false)
    z() } while // comment

    (a?true:false
    )

next_statement;

do do x; while(1); while(1); true;

next_statement;

do do x; while(1) while(1) true

next_statement;

do
do
x
while(1)
while(1) true;

next_statement;

if (x) do x; while(0) else 1

next_statement;
