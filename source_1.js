function factorial(n) {
    if (n <= 2) return n;
    return n * factorial(n - 1);
  }
  
  
var counter = 0;

function partition(arr, low, high) { 
    let pivot = arr[high]; 
    let i = low - 1; 
  
    for (let j = low; j <= high - 1; j++) { 
        // If current element is smaller than the pivot 
        if (arr[j] < pivot) { 
            // Increment index of smaller element 
            i++; 
            // Swap elements 
            [arr[i], arr[j]] = [arr[j], arr[i]];  
        } 
    } 
    // Swap pivot to its correct position 
    [arr[i + 1], arr[high]] = [arr[high], arr[i + 1]];  
    return i + 1; // Return the partition index 
} 
  
function quickSort(arr, low, high) { 
    if (low >= high) return; 
    let pi = partition(arr, low, high); 
  
    quickSort(arr, low, pi - 1); 
    quickSort(arr, pi + 1, high); 
} 

function main() {
    quickSort([2, 4, 6, 8, 10, 1, 3, 5, 7, 9], 0, 9);
    return factorial(5);
}