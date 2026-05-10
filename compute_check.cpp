//
// Created by jason on 5/2/2026.
//


#include "matrix_funcs.hpp"
#include <cmath>
#include <algorithm>

double metric_calculation() {
	int sizes[3] = {256, 512, 1024};
	double scores[3] = {0.0, 0.0, 0.0};
	bool flag = false;
	for (int i = 0; i < 3; i++) {
		int score = 0;
		double count;
		for (count = 0.0; count < 5.0; count++) {
			int time = calculate(sizes[i], 200*pow(10, 6));
			if (time == -1) {
				flag = true;
				break;
			}
			score += time;
			if (time < 5*pow(10,6)) {
				break;
			}
		}
		if (flag) {
			scores[i] = -1.0;
			break;
		}
		if (score != 0) scores[i] = count*pow(sizes[i],3)/score;
		else scores[i] = -1;
	}
	if (scores[2] != -1) return scores[2];
	if (scores[1] != -1) return scores[1];
	return scores[0];
}

/*
break

#include "matrix_funcs.hpp"
#include <cmath>
#include <algorithm>

double metric_calculation() {
	int sizes[3] = {256, 512, 1024};
	double scores[3] = {0.0, 0.0, 0.0};
	bool flag = false;
	for (int i = 0; i < 3; i++) {
		int score = 0.0;
		double count;
		for (count = 0.0; count < 5.0; count++) {
			int time = calculate(sizes[i], 200*pow(10, 6));
			if (time == -1) {
				flag = true;
				break;
			}
			if (time < 5*pow(10,6)) {
				score += time;
				break;
			}
		}
		if (flag) {
			scores[i] = -1.0;
			break;
		}
		scores[i] = count*pow(sizes[i],3)/score;
	}
	return *std::max_element(scores, scores + 3);
}*/