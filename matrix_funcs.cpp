//
// Created by jason on 5/2/2026.
//

#include "matrix_funcs.hpp"
#include <omp.h>
#include <random>
#include <chrono>

int calculate(int size, long timeout) {
    // figure out omp
    int num_threads = omp_get_num_procs() - 1;
    if (num_threads < 1) num_threads = 1;
    omp_set_num_threads(num_threads);
    // create a matrix
    int *matrix1 = new int[size*size];
    int *matrix2 = new int[size*size];
    int *matrix3 = new int[size*size];
    // create mersenne twister rng (seeded with tangent of hive)
    std::mt19937 gen;
    gen.seed(56713);
    for (int i = 0; i < size*size; i++) {
        matrix1[i] = gen();
        matrix2[i] = gen();
    }
    // do the calc
    long long start = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now()
        ).time_since_epoch().count();
    bool flag = false;
    // MSVC OpenMP 2.0: no collapse clause; flatten manually.
    // Use a shared atomic flag to signal timeout across threads.
    int total = size * size;
    #pragma omp parallel for schedule(static)
    for (int ij = 0; ij < total; ij++) {
        if (flag) continue;  // respect timeout flag; omp cancel not in 2.0
        if (std::chrono::time_point_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now()
            ).time_since_epoch().count() - start > timeout) {
            flag = true;     // write is benign race: all threads write same value
            continue;
        }
        int i = ij / size;
        int j = ij % size;
        int sum = 0;
        for (int k = 0; k < size; k++) {
            sum += matrix1[i*size+k] * matrix2[k*size+j];
        }
        matrix3[i*size+j] = sum;
    }
    long long end = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now()
        ).time_since_epoch().count();
    delete[] matrix1;
    delete[] matrix2;
    delete[] matrix3;
    int difference = end - start;
    if (flag) return -1;
    return difference;
}