/* This test contains object literal parser tests. */

switch(0) {
default:
next_statement;

var o = {
    [1 + 2 + f()]: 5,
    func() {
      return 7;
    },
    ["fu" + nc()]() {
      return 8;
    }
  };

next_statement;
}
