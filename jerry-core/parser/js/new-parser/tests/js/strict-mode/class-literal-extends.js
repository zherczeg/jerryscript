/* This test contains class literal parser tests. */

class A extends Array {
  constructor(a) {
    super(b);
    super.f = 6;
    super["g"]();
  }

  func() {
    super.g(5);
    eval('');
  }
};
