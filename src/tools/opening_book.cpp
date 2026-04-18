#include <iostream>

#include "gomoku/OpeningBook.hpp"

int main() {
    const auto lines = gomoku::openingBookLines();
    for (const std::string& line : lines) {
        std::cout << line << '\n';
    }
    return 0;
}
