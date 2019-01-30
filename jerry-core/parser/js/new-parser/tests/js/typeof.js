/* This test contains typeof parser tests. */

next_statement;

typeof ident;
typeof "xx";
typeof function () {}

next_statement;

func(typeof ident);
func(typeof "xx");

next_statement;

func(arg, typeof ident);
func(arg, typeof "xx");

next_statement;

typeof a.b;

next_statement;

!ident;
!"xx";

next_statement;

func(!ident);
func(!"xx");

next_statement;

func(arg, !ident);
func(arg, !"xx");

next_statement;
