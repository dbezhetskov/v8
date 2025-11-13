function Point(x, y) {
    this.x = x;
    this.y = y;
}

const num_iterations = 100000;
var points = [];

function Initialize() {
    for (let i = 0; i < num_iterations; ++i) {
        points[i] = new Point(i, i +1);
    }
}

function main() {
    let sum = 0;
    for (let i = 0; i < num_iterations; ++i) {
        sum += points[i].x + points[i].y;
    }
    return sum;
}

Initialize();
