function add(x, y) {
    return x + y;
}

const num_iterations = 1000;
var results = [];

function Initialize() {
    for (let i = 0; i < num_iterations; ++i) {
        let sum = 0;
        for (let j = 0; j <= i; ++j) {
            sum += j;
        }
        results.push(sum);
    }
}

function main() {
    let sum = 0;
    for (let i = 0; i < num_iterations; ++i) {
        sum += add(i, i + 1);
    }
    return sum;
}

Initialize();
