/* This test contains object literal parser tests. */

next_statement;

if ({},{/**/},{//
}) x++

next_statement;

var get = {member:"exp", "set" /**/ : //
 a + b, /**/ 'get' /**/ : /**/ c // comment
}

next_statement;

var set = {get:a?b:c, set : value, }

next_statement;

({ /*
*/  function : function () {}
 , get get() // comment
 {}, set set(arg) {},
 get/**/"get2"/**/()
 {}, })

next_statement;

({/**/.3/**/:/**/0x09Af/**/,
  12e-1 : 14., 0XFaBe0189 : { 0e+3 : 0.077 + 9074 } })

next_statement;

({//
a
:
f(x)
,
a:f(y)
+5
})

next_statement;

with(a) { throw ({
   err/**/:msg(), err:/**/msg2("another")
 });
}
