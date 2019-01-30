/* This test contains strict mode parser tests. */

(function eval(arguments) { var a = "x"; return arguments + a })

(function arguments(eval) { var a = "y"; return eval + a })

(function arguments() { 'use strict' })
