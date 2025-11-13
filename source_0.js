function factorial(n) {
  if (n <= 2) return n;
  return n * factorial(n - 1);
}


var counter = 0;

function main() {
  return factorial(5);
}
