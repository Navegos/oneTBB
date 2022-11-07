/*
    Copyright (c) 2005-2021 Intel Corporation

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include <algorithm> // std::max

#include "oneapi/tbb/parallel_for.h"
#include "oneapi/tbb/blocked_range.h"

static const std::size_t N = 9;

class SubStringFinder {
    const std::string &str;
    std::vector<std::size_t> &max_array;
    std::vector<std::size_t> &pos_array;

public:
    void operator()(const oneapi::tbb::blocked_range<std::size_t> &r) const {
        for (std::size_t i = r.begin(); i != r.end(); ++i) {
            std::size_t max_size = 0, max_pos = 0;
            for (std::size_t j = 0; j < str.size(); ++j) {
                if (j != i) {
                    std::size_t limit = str.size() - (std::max)(i, j);
                    for (std::size_t k = 0; k < limit; ++k) {
                        if (str[i + k] != str[j + k])
                            break;
                        if (k + 1 > max_size) {
                            max_size = k + 1;
                            max_pos = j;
                        }
                    }
                }
            }
            max_array[i] = max_size;
            pos_array[i] = max_pos;
        }
    }

    SubStringFinder(const std::string &s, std::vector<std::size_t> &m, std::vector<std::size_t> &p)
            : str(s),
              max_array(m),
              pos_array(p) {}
};

int main() {
    std::string str[N] = { std::string("a"), std::string("b") };
    for (std::size_t i = 2; i < N; ++i)
        str[i] = str[i - 1] + str[i - 2];
    std::string &to_scan = str[N - 1];
    const std::size_t num_elem = to_scan.size();
    std::cout << "String to scan: " << to_scan << "\n";

    std::vector<std::size_t> max(num_elem);
    std::vector<std::size_t> pos(num_elem);

    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<std::size_t>(0, num_elem, 100),
                              SubStringFinder(to_scan, max, pos));

    for (std::size_t i = 0; i < num_elem; ++i) {
        for (std::size_t j = 0; j < num_elem; ++j) {
            if (j >= i && j < i + max[i])
                std::cout << "_";
            else
                std::cout << " ";
        }
        std::cout << "\n" << to_scan << "\n";

        for (std::size_t j = 0; j < num_elem; ++j) {
            if (j >= pos[i] && j < pos[i] + max[i])
                std::cout << "*";
            else
                std::cout << " ";
        }
        std::cout << "\n";
    }

    return 0;
}