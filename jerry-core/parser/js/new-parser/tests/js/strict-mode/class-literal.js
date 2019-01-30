/* This test contains class literal parser tests. */

class A {
  constructor() {
    return {};
  }

  get static() {
    return 5;
  }

  static static() {
  }

  static get get() {
  }

  ;;;;;;;;;;;;;;

  "get"() {
  }

  "static"() {
  }

  [f()]() {
  }

  set [f()](v) {
  }

  static set [g()](v) {
  }
};
