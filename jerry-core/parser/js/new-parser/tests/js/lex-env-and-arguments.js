/* This test contains lexical environment and arguments tests. */

function f1() { var a = eval; }

function f2() { var a = eval("1"); }

function f3() { var a = 5; with (a) { } }

function f4() { var a = 10; try {} finally {} }

function f5() { var a = 10; try {} catch (e) {} }

function f6() { var a = function() {} }

function f7() { delete 1; delete a.b; }

function f8() { delete a; }

function f9() { var a; delete a; with(a) { b } }

function f10() { var a; delete a; with(a) { a } }

function f11() { return arguments; var arguments; }

(function arguments() { return 6; })

function f12(arguments) { a = arguments; }

function f13() { a = arguments; function arguments() {} }

function f14(a,a) { var c = 1; return a + c + arguments[0]; }

function f15(a,b) { "use strict"; var c = 1; return a + b + c + arguments[0]; }

function f16(a,b) { return a.arguments + { arguments: 5 } }
