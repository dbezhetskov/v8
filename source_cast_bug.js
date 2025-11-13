function PrintPoint(point) {
    console.log(point.x, point.y);
}

function Point(x, y) {
    this.x = x;
    this.y = y;
    this.print = () => { PrintPoint(this); };
}

const num_iterations = 400000;
var points = [];

function Initialize() {
    for (let i = 0; i < num_iterations; ++i) {
        points.push(new Point(i, i +1));
    }
}

function main() {
    let sum = 0;
    for (let i = 0; i < num_iterations; ++i) {
        points[i].print();
        sum += points[i].x + points[i].y;
    }
    return sum;
}

Initialize();
