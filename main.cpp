#include <unordered_map>
#include <map>
#include <vector>
#include <algorithm>
#include <string>
#include <iostream>



void wordFreq(const std::vector<std::string>& words) {
    std::unordered_map<std::string, int> freq;   // O(1) поиск
    for (auto& w : words) ++freq[w];

    // Переносим в vector пар и сортируем по частоте
    std::vector<std::pair<std::string, int>> v(freq.begin(), freq.end());
    std::sort(v.begin(), v.end(),
              [](auto& a, auto& b) { return a.second > b.second; });

    for (auto& [w, c] : v)
        std::cout << w << ": " << c << "\n";
}

int main() {
    wordFreq({"a", "b", "a", "c", "b", "a"});
    // a: 3, b: 2, c: 1
}