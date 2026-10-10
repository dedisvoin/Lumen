#include "Strings.cpp"
#include "exception"
#include <fstream>

#pragma once

struct FileNotFound: std::exception {
    const char* what() const noexcept override {
        return "file not found";
    }
};

bool isFileFound(const String& file_name) noexcept {
    std:: ifstream stream(file_name.to_utf8(), std:: ios:: binary);
    auto check_result = stream.is_open();
    
    stream.close();
    return check_result;
}


inline void FOUND_FILE(String file_name) {if (!isFileFound(file_name)) throw FileNotFound{}; }


String openFile(const String& file_name) { FOUND_FILE(file_name);

    std:: ifstream stream(file_name.to_utf8(), std:: ios:: binary);

    stream.seekg(0, std:: ios:: end);
    size_t size = stream.tellg();
    stream.seekg(0, std:: ios:: beg);

    std:: string content(size, '\0');
    stream.read(&content[0], size);
    stream.close();

    // Normalise line endings: \r\n -> \n, standalone \r -> \n
    std:: string result;
    result.reserve(content.size());
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            result += '\n';
            if (i + 1 < content.size() && content[i + 1] == '\n')
                ++i;  // skip \n that follows \r
        } else {
            result += content[i];
        }
    }

    return String(result);
}


void saveFile(const String& file_name, const String& data) {
    std::ofstream stream(file_name.to_utf8());
    stream << data;
    stream.close();
}
