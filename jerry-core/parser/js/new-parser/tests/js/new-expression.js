/* This test contains new expression parser tests. */

next_statement;

new new a;

next_statement;

new new a(x)(y);

next_statement;

new new a()(x,y)(z);

next_statement;

new new a().b(c);

next_statement;

new new a()[b](c);

next_statement;

new a(x,y,z);

next_statement;

new (func());

next_statement;

new (a++);

next_statement;
