#include "vector"
#pragma once

template<typename T>
bool inVector(const std:: vector<T>& vec, const T& value) noexcept {
    for (const auto& e: vec) if (e == value) return true;
    return false;
}
